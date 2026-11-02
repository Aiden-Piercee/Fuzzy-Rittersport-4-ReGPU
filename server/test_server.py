import json, os, struct, tempfile, threading, unittest
import fm4_live_server as s
import services, xtsw
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
    def test_logon_permissions_and_credits(self):
        st, _, _, r = self.cl(5).call("User", "Logon2", {"xuid": 5, "gamertag": "Tag"})
        self.assertEqual(st, 0)
        self.assertTrue(r["profile_valid"]); self.assertEqual(r["user"]["gamertag"], "Tag")
        self.assertTrue(all(p["enabled"] and p["access"] == 0 for p in r["permissions"]))
        self.assertEqual(r["user"]["credits"], self.store.cfg["starting_credits"])


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
    def test_full_walk_over_http(self):
        sel, buy = self.cl(1001), self.cl(2002)
        sel.call("User", "Logon2", {"xuid": 1001, "gamertag": "Seller"})
        buy.call("User", "Logon2", {"xuid": 2002, "gamertag": "Buyer"})
        st, _, _, r = sel.call("Auction", "Create", dict(CAR, start_price=20000, buyout=0, hours=24))
        self.assertEqual(st, 0); aid = r["id"]
        st, code, msg, _ = sel.call("Auction", "PlaceBid", {"id": aid, "amount": 30000})
        self.assertEqual((st, code), (2, 21))
        st, _, _, r = buy.call("Auction", "PlaceBid", {"id": aid, "amount": 20000})
        self.assertEqual(r["top_bid"], 20000)
        lst = buy.call("Auction", "GetAuctions", {"limit": 10})[3]["auctions"]
        self.assertEqual(lst[0]["viewer_status"], 4)  # top bidder
        det = buy.call("Auction", "GetDetails", {"id": aid})[3]
        self.assertEqual(det["car_blob"], b"blob"); self.assertEqual(det["min_next_bid"], 21000)
        self.assertEqual(len(buy.call("Auction", "GetMyBids")[3]["auctions"]), 1)
        self.clock.t += 24 * 3600 + 1
        won = buy.call("Auction", "GetMyBids")[3]["auctions"][0]
        self.assertEqual(won["viewer_status"], 5)
        self.assertTrue(won["allowed_actions"] & services.COLLECT_WON)
        self.assertEqual(buy.call("Auction", "RetrieveCarForAuction", {"id": aid})[3]["car_blob"], b"blob")
        paid = sel.call("Auction", "RetrievePaymentForAuctionSold", {"id": aid})[3]
        self.assertEqual(paid["amount"], 19000)

    def test_config(self):
        r = self.cl(1).call("Auction", "GetGlobalConfig")[3]
        self.assertEqual(r["fee_pct"], 5); self.assertIn(24, r["durations_hours"])


if __name__ == "__main__": unittest.main()
