import json, os, struct, tempfile, threading, unittest
import fm4_live_server as s
import services, xtsw, layouts, auction_service
from auction_store import Store, AuctionError
from client import Client


class Clock:
    def __init__(self): self.t = 1_000_000.0
    def __call__(self): return self.t


class Base(unittest.TestCase):
    def setUp(self):
        self.log = os.path.join(tempfile.mkdtemp(), "c.jsonl")
        self.clock = Clock()
        self.store = Store(clock=self.clock)
        self.srv = s.serve("127.0.0.1", 0, self.log, s.State(self.store))
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.port = self.srv.server_address[1]

    def tearDown(self): self.srv.shutdown(); self.srv.server_close()

    def cl(self, x): return Client("127.0.0.1", self.port, x)


class Framing(Base):
    def test_unknown_is_empty_success_and_logged(self):
        st, code, msg, tail = self.cl(1).raw("Hopper", "GetList", b"\1\2")
        self.assertEqual((st, code, msg), (0, 0, ""))
        rec = json.loads(open(self.log).read().splitlines()[0])
        self.assertFalse(rec["handled"]); self.assertEqual(rec["body"], "0102")
        self.assertEqual(rec["method"], "GetList")

    def test_handler_and_error(self):
        @s.handler("Test", "Echo")
        def echo(ctx, body): return body
        @s.handler("Test", "Boom")
        def boom(ctx, body): raise ValueError("nope")
        self.assertEqual(self.cl(1).raw("Test", "Echo", b"abc")[3][:3], b"abc")
        st, code, msg, _ = self.cl(1).raw("Test", "Boom")
        self.assertEqual((st, code), (2, 1)); self.assertIn("nope", msg)

    def test_bad_request_layout_is_service_error(self):
        st, code, msg, _ = self.cl(1).raw("Auction", "PlaceBid", b"\0")
        self.assertEqual((st, code), (2, 3)); self.assertIn("layout", msg)


class Codec(unittest.TestCase):
    def test_roundtrip(self):
        sch = [("a", "u32"), ("s", "str"), ("l", ("array", [("x", "i32"), ("t", "time")])),
               ("b", "bytes"), ("f", "bool")]
        rec = {"a": 7, "s": "héllo", "l": [{"x": -2, "t": xtsw.iso(0)}], "b": b"\1\2", "f": True}
        self.assertEqual(xtsw.decode(sch, xtsw.encode(sch, rec)), rec)

    def test_truncated(self):
        with self.assertRaises(xtsw.DecodeError):
            xtsw.decode([("a", "u32")], b"\0\0")


class Logon(Base):
    def test_zero_reply_registers_user(self):
        os.environ["FM4_LOGON_ZERO"] = "1"; self.addCleanup(os.environ.pop, "FM4_LOGON_ZERO")
        st, code, msg, tail = self.cl(5).raw("User", "Logon2", xtsw.encode(
            [("x", "u64"), ("g", "str")], {"x": 5, "g": "Tag"}))
        self.assertEqual((st, tail), (0, bytes(64)))
        self.assertEqual(self.store.user("5")["gamertag"], "Tag")

    def test_logon_permissions(self):
        login = {"xuid": 5, "gamertag": "Tag", "clientProfile": {"profileId": bytes(24)}}
        st, _, _, r = self.cl(5).call("User", "Logon2", {"loginData": login})
        self.assertEqual(st, 0)
        r = r["responseData"]
        self.assertTrue(r["isProfileValid"]); self.assertEqual(r["userData"]["gamertag"], "Tag")
        self.assertTrue(all(p["enabled"] and p["access"] == 0 for p in r["permissions"]))


CARW = {"make": 1, "carId": 1234, "carClass": 3, "perfIndex": 640.0, "year": 1999, "peakPower": 300.0}
CAR = {"title": "T", "car_ordinal": 9, "class": 2, "pi": 500, "power": 250, "car_blob": b"blob"}


class Rules(unittest.TestCase):
    def setUp(self):
        self.c = Clock(); self.s = Store(clock=self.c)

    def listed(self, **kw):
        return self.s.create("1", "Seller", CAR, kw.get("start", 10000), kw.get("buyout", 0), 24)

    def test_bid_flow_and_payments(self):
        a = self.listed(buyout=0)
        self.s.bid(a, "2", "B2", 10000)
        with self.assertRaises(AuctionError): self.s.bid(a, "3", "B3", 10100)  # under +5%
        self.s.bid(a, "3", "B3", 10500)
        self.assertEqual(self.s.credits("2"), 1_000_000 - 10000)  # held until refunded
        self.assertEqual(self.s.retrieve_payment(a, "2", "lost"), 10000)
        self.assertEqual(self.s.credits("2"), 1_000_000)
        with self.assertRaises(AuctionError): self.s.retrieve_payment(a, "2", "lost")  # once
        self.c.t += 24 * 3600 + 1
        self.assertEqual(self.s.get(a)["status"], "sold")
        self.assertEqual(self.s.viewer_status(self.s.get(a), "3"), "won")
        self.assertEqual(self.s.viewer_status(self.s.get(a), "2"), "lost")
        self.assertEqual(self.s.retrieve_car(a, "3"), b"blob")
        with self.assertRaises(AuctionError): self.s.retrieve_car(a, "3")
        with self.assertRaises(AuctionError): self.s.retrieve_car(a, "1")
        self.assertEqual(self.s.retrieve_payment(a, "1", "sold"), 10500 - 525)  # 5% fee

    def test_expire_unsold_returns_car_to_seller(self):
        a = self.listed()
        self.c.t += 24 * 3600 + 1
        self.assertEqual(self.s.get(a)["status"], "expired")
        with self.assertRaises(AuctionError): self.s.retrieve_car(a, "9")
        self.assertEqual(self.s.retrieve_car(a, "1"), b"blob")

    def test_buyout_closes_at_once(self):
        a = self.listed(buyout=30000)
        r = self.s.bid(a, "2", "B2", 99999)
        self.assertTrue(r["bought_out"]); self.assertEqual(r["top_bid"], 30000)
        self.assertEqual(self.s.get(a)["status"], "sold")
        with self.assertRaises(AuctionError): self.s.bid(a, "3", "B3", 40000)

    def test_rules(self):
        a = self.listed()
        with self.assertRaises(AuctionError): self.s.bid(a, "1", "Seller", 10000)  # own
        with self.assertRaises(AuctionError): self.s.bid(a, "2", "B", 999_999_999)  # poor
        with self.assertRaises(AuctionError): self.s.create("1", "S", CAR, 5, 0, 24)
        with self.assertRaises(AuctionError): self.s.create("1", "S", CAR, 10000, 0, 5)
        self.s.bid(a, "2", "B", 10000)
        with self.assertRaises(AuctionError): self.s.cancel(a, "1")  # has a bid

    def test_cancel_restart_ban(self):
        a = self.listed()
        self.s.cancel(a, "1"); self.s.restart(a, "1", 72)
        self.assertEqual(self.s.get(a)["status"], "active")
        self.s.bid(a, "2", "B", 10000)
        self.s.ban(a)
        self.assertEqual(self.s.retrieve_payment(a, "2", "lost"), 10000)
        self.assertEqual(self.s.search(), [])

    def test_search_filters(self):
        self.listed(); self.s.create("1", "S", dict(CAR, **{"class": 5, "power": 700}), 20000, 0, 24)
        self.assertEqual(len(self.s.search()), 2)
        self.assertEqual(len(self.s.search(**{"class": 5})), 1)
        self.assertEqual(len(self.s.search(power_min=600)), 1)
        self.assertEqual(len(self.s.search(price_max=15000)), 1)


class Wire(Base):
    def logon(self, c, tag):
        return c.call("User", "Logon2", {"loginData": {"xuid": c.xuid, "gamertag": tag}})

    def test_full_walk_over_http(self):
        sel, buy = self.cl(1001), self.cl(2002)
        self.logon(sel, "Seller"); self.logon(buy, "Buyer")
        st, _, _, r = sel.call("Auction", "Create", {"openingPrice": 20000, "buyoutPrice": 0,
                               "durationMs": 24 * 3600_000, "car": CARW, "tuning": b"tune",
                               "partsInstalled": b"parts"})
        self.assertEqual(st, 0); aid = r["auctionId"]
        self.assertEqual(self.store.credits("1001"), 1_000_000 - 2000)  # listing fee
        st, code, msg, _ = sel.call("Auction", "PlaceBid", {"auctionId": aid, "amount": 30000})
        self.assertEqual((st, code), (2, 21))
        self.assertEqual(buy.call("Auction", "PlaceBid", {"auctionId": aid, "amount": 20000})[0], 0)
        s = buy.call("Auction", "SearchForAuction", {"maxResultIds": 10, "maxResults": 10})[3]
        self.assertEqual(s["auctionIds"], [aid]); self.assertEqual(s["totalAvailable"], 1)
        b = s["auctionResults"][0]["base"]
        self.assertEqual((b["status"], b["topBidAmount"], b["nextBidAmount"], b["numBids"]), (4, 20000, 21000, 1))
        self.assertEqual(s["auctionResults"][0]["carModelId"], 1234)
        d = buy.call("Auction", "GetDetails", {"auctionId": aid})[3]["auctionDetails"]
        self.assertEqual(d["car"]["carId"], 1234); self.assertEqual(d["tuning"], b"tune")
        self.assertEqual(d["seller"]["wzGamerTag"], "Seller")
        g = buy.call("Auction", "GetAuctions", {"auctionIds": [aid], "maxAuctionResults": 5})[3]
        self.assertEqual(g["auctions"][0]["id"], aid)
        self.assertEqual(len(buy.call("Auction", "GetMyBids", {"maxResultIds": 5, "maxResults": 5})[3]["auctionIds"]), 1)
        self.clock.t += 24 * 3600 + 1
        won = buy.call("Auction", "GetMyBids", {"maxResultIds": 5, "maxResults": 5})[3]["auctionResults"][0]["base"]
        self.assertEqual(won["status"], 5)
        self.assertTrue(won["allowedActions"] & auction_service.COLLECT_WON)
        car = buy.call("Auction", "RetrieveCarForAuction", {"auctionId": aid})[3]
        self.assertEqual((car["car"]["carId"], car["partsInstalled"], car["isOwner"]), (1234, b"parts", False))
        paid = sel.call("Auction", "RetrievePaymentForAuctionSold", {"auctionId": aid})[3]
        self.assertEqual(paid["bidAmount"], 19000)

    def test_textures_and_lost_payment(self):
        sel, a, b = self.cl(1), self.cl(2), self.cl(3)
        tid = sel.call("Auction", "UploadAuctionTextures", {"liveryTexture": b"L", "LiveryThumbnail": b"T"})[3]["texturesId"]
        aid = sel.call("Auction", "Create", {"openingPrice": 10000, "durationMs": 24 * 3600_000,
                                             "car": CARW, "texturesId": tid})[3]["auctionId"]
        a.call("Auction", "PlaceBid", {"auctionId": aid, "amount": 10000})
        b.call("Auction", "PlaceBid", {"auctionId": aid, "amount": 10500})
        s = a.call("Auction", "SearchForAuction", {"maxResults": 5})[3]["auctionResults"][0]
        self.assertEqual(s["LiveryThumbnail"], b"T"); self.assertEqual(s["base"]["status"], 6)  # outbid
        self.assertTrue(s["base"]["allowedActions"] & auction_service.RECV_REFUND)
        self.assertEqual(a.call("Auction", "RetrievePaymentsForAuctionLost", {"auctionId": aid})[3]["bidAmount"], 10000)
        self.assertEqual(a.call("Auction", "RetrievePaymentsForAuctionLost", {"auctionId": aid})[1], 42)

    def test_known_call_without_reply_fields_is_plain_success(self):
        st, code, msg, tail = self.cl(1).raw("User", "ValidateProfileCheckpoint", bytes(36))
        self.assertEqual((st, tail), (0, b""))
        self.assertEqual(self.cl(1).call("User", "GetForzaAssetCounts", {"maxAssets": 16})[3], {"assets": []})
        self.assertEqual(self.cl(1).call("Storefront", "QueryOnlineCredit")[3], {"credit": 0})

    def test_self_bid_switch(self):
        os.environ["FM4_ALLOW_SELF_BID"] = "1"; self.addCleanup(os.environ.pop, "FM4_ALLOW_SELF_BID")
        c = self.cl(1)
        aid = c.call("Auction", "Create", {"openingPrice": 10000, "durationMs": 24 * 3600_000, "car": CARW})[3]["auctionId"]
        self.assertEqual(c.call("Auction", "PlaceBid", {"auctionId": aid, "amount": 10000})[0], 0)

    def test_config_matches_game_layout(self):
        r = self.cl(1).call("Auction", "GetGlobalConfig", {"maxFeeResults": 10})[3]
        self.assertEqual(r["feeData"][0], {"hours": 24, "credits": 2000})
        self.assertTrue(r["searchPriceThresholds"]); self.assertEqual(r["refreshParams"]["auctionsRefreshSeconds"], 30)

    def test_defaults_and_acks(self):
        c = self.cl(1)
        self.assertEqual(c.raw("OnlineProfile", "UpdateAll", b"junk")[:3], (0, 0, ""))
        self.assertEqual(c.call("DailyCreditBonus", "GetPayout")[3], {"amount": 0})
        # unhandled call with a known reply record: empty lists in the right layout, not bare zeros
        st, _, _, tail = c.raw("Scoreboard", "GetRollupRows")
        self.assertEqual(st, 0)
        self.assertEqual(xtsw.decode(layouts.result_for("Scoreboard", "GetRollupRows"), tail) is not None, True)
        self.assertEqual(c.call("DLCMetadata", "GetStringTableInfo", {"language": "EN"})[0], 0)


if __name__ == "__main__": unittest.main()
