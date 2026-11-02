"""Handlers for the Turn 10 services we answer for real.

LAYOUT STATUS: the byte layouts below are NOT yet verified against FM4. No
FM4 server source exists; they follow forza-live's documented conventions and
the method and class names found in the game image. Every schema lives in one
table (SCHEMAS) so that when a layout is read from the game's own descriptor
tables, or seen in calls.jsonl, it is a one-line change. A request that does
not decode is answered as a service failure and logged with its body in hex.

Licence: AGPL-3.0-or-later. Structure after forza-live (AGPL-3.0).
"""
import time
import xtsw
from fm4_live_server import handler, ServiceError
from auction_store import AuctionError

# --- schemas (unverified) -----------------------------------------------------
USER_DATA = [("xuid", "u64"), ("gamertag", "str"), ("credits", "u32")]
PERMISSION = [("method_id", "u32"), ("tier", "u32"), ("enabled", "bool"), ("access", "u32")]
AUCTION_SUMMARY = [
    ("id", "u32"), ("title", "str"), ("seller_xuid", "u64"), ("seller_tag", "str"),
    ("car_ordinal", "u32"), ("class", "u32"), ("pi", "u32"), ("year", "u32"),
    ("power", "u32"), ("start_price", "u32"), ("buyout", "u32"), ("top_bid", "u32"),
    ("top_tag", "str"), ("ends", "time"), ("viewer_status", "u32"),
    ("allowed_actions", "u32"),
]

# ForzaUserLogon2_Params.loginData (110 bytes for a 6-character gamertag).
LOGIN_DATA = [
    ("xuid", "u64"), ("gamertag", "str"), ("userTier", "i32"), ("language", "i32"), ("region", "i32"),
    ("locale", "i32"), ("lcid", "i32"), ("ipAddress", "u32"), ("machineId", "u64"), ("macAddress", "bytes"),
    ("ageGroup", "u8"), ("clientProfile", [("profileId", "bytes"), ("transactionCount", "i32")]),
    ("titleVersion", "i32"), ("careerLevel", "u8"), ("badgeId", "i16"), ("playerTitleId", "i16"),
    ("friendXuids", ("array", "u64")), ("gameCarIds", ("array", "guid"))]
# ForzaUserLogon2_Results.responseData: the permission table is {MethodId i32, Tier i32, IsEnabled bool, Access i32}.
METHOD_PERMISSION = [("method_id", "i32"), ("tier", "i32"), ("enabled", "bool"), ("access", "i32")]
LOGON_RESULT = [
    ("isProfileValid", "bool"), ("lastOnlineProfileUpdateAllTablesTranId", "i32"),
    ("permissions", ("array", METHOD_PERMISSION)), ("featureAreasBanMask", "u32"), ("userFlags", "u8"),
    ("upsellOfferId", "i64"), ("upsellText", "str"), ("vipOfferIds", ("array", "i64")),
    ("userData", [("xuid", "u64"), ("gamertag", "str"), ("flags", "u8"), ("clanId", "i32"), ("clanTag", "str"),
                  ("roleInClan", "i32"), ("careerLevel", "u8"), ("badgeId", "i16"), ("playerTitleId", "i16")]),
    ("notificationNonce", "u16"), ("notificationPollIntervalSeconds", "i32"),
    ("notificationCategories", ("array", [("type", "i32"), ("name", "str"), ("description", "str"),
                                          ("backgroundImageFileId", "i32")])),
    ("storefrontCap", "i32"),
    ("gameplayLogConfig", [("enabled", "bool"), ("flushInterval", "u32"), ("throttle", "bytes")]),
    ("carsToUnshareFromGame", ("array", "guid")), ("defaultTimeoutMs", "i32"), ("defaultContentTimeoutMs", "i32")]

SCHEMAS = {
    # Verified: read from the game's own type descriptors (server/xtsw-layouts.txt), ForzaUserLogon2_Params and _Results.
    ("User", "Logon2", "req"): [("loginData", LOGIN_DATA)],
    ("User", "Logon2", "res"): [("responseData", LOGON_RESULT)],
    ("Auction", "GetGlobalConfig", "res"): [
        ("min_start_price", "u32"), ("max_start_price", "u32"), ("min_increment_pct", "u32"),
        ("fee_pct", "u32"), ("durations_hours", ("array", "u32"))],
    ("Auction", "GetAuctions", "req"): [
        ("offset", "u32"), ("limit", "u32"), ("class", "u32"), ("country", "u32"),
        ("drive", "u32"), ("price_min", "u32"), ("price_max", "u32"),
        ("power_min", "u32"), ("power_max", "u32"), ("year", "u32"), ("car_ordinal", "u32")],
    ("Auction", "GetAuctions", "res"): [("auctions", ("array", AUCTION_SUMMARY))],
    ("Auction", "GetDetails", "req"): [("id", "u32")],
    ("Auction", "GetDetails", "res"): [("auction", AUCTION_SUMMARY), ("car_blob", "bytes"),
                                        ("min_next_bid", "u32")],
    ("Auction", "GetMyAuctions", "res"): [("auctions", ("array", AUCTION_SUMMARY))],
    ("Auction", "GetMyBids", "res"): [("auctions", ("array", AUCTION_SUMMARY))],
    ("Auction", "Create", "req"): [
        ("title", "str"), ("car_ordinal", "u32"), ("class", "u32"), ("pi", "u32"),
        ("year", "u32"), ("power", "u32"), ("country", "u32"), ("drive", "u32"),
        ("start_price", "u32"), ("buyout", "u32"), ("hours", "u32"), ("car_blob", "bytes")],
    ("Auction", "Create", "res"): [("id", "u32")],
    ("Auction", "PlaceBid", "req"): [("id", "u32"), ("amount", "u32")],
    ("Auction", "PlaceBid", "res"): [("top_bid", "u32"), ("bought_out", "bool"), ("credits", "u32")],
    ("Auction", "Restart", "req"): [("id", "u32"), ("hours", "u32")],
    ("Auction", "RetrieveCarForAuction", "req"): [("id", "u32")],
    ("Auction", "RetrieveCarForAuction", "res"): [("car_blob", "bytes")],
    ("Auction", "RetrievePaymentForAuctionSold", "req"): [("id", "u32")],
    ("Auction", "RetrievePaymentForAuctionLost", "req"): [("id", "u32")],
    ("Auction", "RetrievePaymentForAuctionSold", "res"): [("amount", "u32"), ("credits", "u32")],
    ("Auction", "RetrievePaymentForAuctionLost", "res"): [("amount", "u32"), ("credits", "u32")],
    ("Auction", "RemoveMyBannedAuction", "req"): [("id", "u32")],
    ("Auction", "UploadAuctionTextures", "req"): [("id", "u32"), ("textures", "bytes")],
}


def dec(cls, method, body):
    try:
        return xtsw.decode(SCHEMAS[(cls, method, "req")], body)
    except xtsw.DecodeError as e:
        raise ServiceError(3, f"{cls}.{method}: request layout mismatch: {e}")


def enc(cls, method, rec):
    return xtsw.encode(SCHEMAS[(cls, method, "res")], rec)


def xuid_of(ctx):
    try:
        return str(int(ctx.xuid, 0))
    except ValueError:
        raise ServiceError(4, "missing or bad X-UserId")


def guarded(fn):
    def w(ctx, body):
        try:
            return fn(ctx, body)
        except AuctionError as e:
            raise ServiceError(e.code, str(e))
    w.__name__ = fn.__name__
    return w


# --- User -------------------------------------------------------------------------
# Every method is permitted: an absent table entry makes the game show an
# "unavailable" error without sending a request, so we list the ids we know of.
PERMISSION_METHODS = range(1, 257)


# Real request seen from the game (110 bytes): u64 xuid, string gamertag, then 66 bytes we have not
# decoded yet (u32 words 1, 1, 0xffff, 36, 9, 0, then small fields, a 20-byte hash-like blob and
# flags). The reply layout is unknown; until it is, the default answer is a success header with a
# zero tail (what the game received on its first contact). FM4_LOGON_GUESS=1 sends the guessed
# full layout instead, for experiments only.
import os


@handler("User", "Logon2")
@guarded
def logon2(ctx, body):
    if os.environ.get("FM4_LOGON_ZERO"):  # the old known-safe answer, for experiments
        r = xtsw.Reader(body)
        try:
            ctx.state.store.user(str(r.scalar("u64")), r.scalar("str") or None)
        except xtsw.DecodeError:
            pass
        return bytes(64)
    try:
        req = xtsw.decode(SCHEMAS[("User", "Logon2", "req")], body)["loginData"]
    except xtsw.DecodeError:
        req = {"xuid": int(xuid_of(ctx)), "gamertag": ""}
    xuid = str(req["xuid"] or xuid_of(ctx))
    u = ctx.state.store.user(xuid, req["gamertag"] or None)
    return enc("User", "Logon2", {"responseData": {
        "isProfileValid": True,
        "permissions": [{"method_id": m, "tier": 0, "enabled": True, "access": 0}
                        for m in PERMISSION_METHODS],
        "userData": {"xuid": int(xuid), "gamertag": u["gamertag"]},
        "notificationPollIntervalSeconds": 300,
        "storefrontCap": 100,
        "defaultTimeoutMs": 30000, "defaultContentTimeoutMs": 60000}})


# --- Auction ------------------------------------------------------------------------
STATUS_CODE = {"active": 0, "expired": 1, "sold": 2, "cancelled": 3, "top_bidder": 4,
               "won": 5, "outbid": 6, "lost": 7}
BID, BUYOUT, COLLECT_WON, COLLECT_FAILED, RESTART, CANCEL, RECV_REFUND, RECV_SALE = (
    1 << i for i in range(8))


def actions_for(store, a, xuid):
    v = store.viewer_status(a, xuid)
    m = 0
    if v in ("active", "outbid") and a["seller"] != xuid:
        m |= BID | (BUYOUT if a["buyout"] else 0)
    if v == "won" and not a["car_collected"]:
        m |= COLLECT_WON
    if a["seller"] == xuid:
        if a["status"] == "active" and not a["top_bidder"]:
            m |= CANCEL
        if a["status"] in ("expired", "cancelled") and not a["car_collected"]:
            m |= COLLECT_FAILED | RESTART
        pend = store.db.execute("SELECT 1 FROM payments WHERE auction=? AND xuid=? AND kind='sold' AND collected=0",
                                (a["id"], xuid)).fetchone()
        if pend:
            m |= RECV_SALE
    if store.db.execute("SELECT 1 FROM payments WHERE auction=? AND xuid=? AND kind='lost' AND collected=0",
                        (a["id"], xuid)).fetchone():
        m |= RECV_REFUND
    return v, m


def summary(store, a, xuid):
    v, m = actions_for(store, a, xuid)
    return {"id": a["id"], "title": a["title"], "seller_xuid": int(a["seller"]),
            "seller_tag": a["seller_tag"], "car_ordinal": a["car_ordinal"], "class": a["class"],
            "pi": a["pi"], "year": a["year"], "power": a["power"],
            "start_price": a["start_price"], "buyout": a["buyout"], "top_bid": a["top_bid"],
            "top_tag": a["top_tag"] or "", "ends": xtsw.iso(a["ends"]),
            "viewer_status": STATUS_CODE[v], "allowed_actions": m}


def listing(ctx, rows):
    s = ctx.state.store
    x = xuid_of(ctx)
    return [summary(s, a, x) for a in rows]


@handler("Auction", "GetGlobalConfig")
def get_config(ctx, body):
    c = ctx.state.store.cfg
    return enc("Auction", "GetGlobalConfig", c)


@handler("Auction", "GetAuctions")
@guarded
def get_auctions(ctx, body):
    r = dec("Auction", "GetAuctions", body)
    r["limit"] = min(r["limit"] or 50, 100)
    rows = ctx.state.store.search(**r)
    return enc("Auction", "GetAuctions", {"auctions": listing(ctx, rows)})


@handler("Auction", "SearchForAuction")
@guarded
def search_auction(ctx, body):
    return get_auctions(ctx, body)


@handler("Auction", "GetDetails")
@guarded
def get_details(ctx, body):
    r = dec("Auction", "GetDetails", body)
    s = ctx.state.store
    a = s.get(r["id"])
    return enc("Auction", "GetDetails", {
        "auction": summary(s, a, xuid_of(ctx)), "car_blob": a["car_blob"],
        "min_next_bid": s.min_next_bid(a)})


@handler("Auction", "GetMyAuctions")
@guarded
def my_auctions(ctx, body):
    return enc("Auction", "GetMyAuctions",
               {"auctions": listing(ctx, ctx.state.store.mine(xuid_of(ctx)))})


@handler("Auction", "GetMyBids")
@guarded
def my_bids(ctx, body):
    return enc("Auction", "GetMyBids",
               {"auctions": listing(ctx, ctx.state.store.my_bids(xuid_of(ctx)))})


@handler("Auction", "Create")
@guarded
def create(ctx, body):
    r = dec("Auction", "Create", body)
    x = xuid_of(ctx)
    tag = ctx.state.store.user(x)["gamertag"]
    aid = ctx.state.store.create(x, tag, r, r["start_price"], r["buyout"], r["hours"])
    return enc("Auction", "Create", {"id": aid})


@handler("Auction", "PlaceBid")
@guarded
def place_bid(ctx, body):
    r = dec("Auction", "PlaceBid", body)
    x = xuid_of(ctx)
    tag = ctx.state.store.user(x)["gamertag"]
    return enc("Auction", "PlaceBid", ctx.state.store.bid(r["id"], x, tag, r["amount"]))


@handler("Auction", "Restart")
@guarded
def restart(ctx, body):
    r = dec("Auction", "Restart", body)
    ctx.state.store.restart(r["id"], xuid_of(ctx), r["hours"])
    return b""


@handler("Auction", "RetrieveCarForAuction")
@guarded
def retrieve_car(ctx, body):
    r = dec("Auction", "RetrieveCarForAuction", body)
    return enc("Auction", "RetrieveCarForAuction",
               {"car_blob": ctx.state.store.retrieve_car(r["id"], xuid_of(ctx))})


def _payment(kind, method):
    @handler("Auction", method)
    @guarded
    def h(ctx, body):
        r = dec("Auction", method, body)
        x = xuid_of(ctx)
        paid = ctx.state.store.retrieve_payment(r["id"], x, kind)
        return enc("Auction", method, {"amount": paid, "credits": ctx.state.store.credits(x)})
    return h


_payment("sold", "RetrievePaymentForAuctionSold")
_payment("lost", "RetrievePaymentForAuctionLost")


@handler("Auction", "RemoveMyBannedAuction")
@guarded
def remove_banned(ctx, body):
    r = dec("Auction", "RemoveMyBannedAuction", body)
    s = ctx.state.store
    a = s.get(r["id"])
    if a["seller"] != xuid_of(ctx) or not a["banned"]:
        raise ServiceError(30, "not a banned auction of yours")
    s.db.execute("DELETE FROM auctions WHERE id=?", (a["id"],))
    return b""


@handler("Auction", "UploadAuctionTextures")
@guarded
def upload_textures(ctx, body):
    r = dec("Auction", "UploadAuctionTextures", body)
    s = ctx.state.store
    a = s.get(r["id"])
    if a["seller"] != xuid_of(ctx):
        raise ServiceError(30, "not your auction")
    s.db.execute("UPDATE auctions SET textures=? WHERE id=?", (r["textures"], a["id"]))
    return b""
