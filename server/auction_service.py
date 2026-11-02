"""Auction.* handlers: wire records (from the game's descriptors) over auction_store rules.

Per-viewer status codes follow forza-live's documentation (0 active, 1 expired, 2 sold,
3 cancelled, 4 top bidder, 5 won, 6 outbid, 7 lost). The allowedActions bit order is
likewise from there and is the least certain part: bid, buyout, collect won car, collect
failed car, restart, cancel, report, remove banned, receive refund, receive sale.

Licence: AGPL-3.0-or-later.
"""
import time
import xtsw
import layouts
from fm4_live_server import handler, ServiceError
from auction_store import AuctionError
from services import dec, enc, xuid_of, guarded

STATUS_CODE = {"active": 0, "expired": 1, "sold": 2, "cancelled": 3, "top_bidder": 4,
               "won": 5, "outbid": 6, "lost": 7}
(BID, BUYOUT, COLLECT_WON, COLLECT_FAILED, RESTART, CANCEL, REPORT, REMOVE_BANNED,
 RECV_REFUND, RECV_SALE) = (1 << i for i in range(10))

CREATE = lambda: layouts.request("Auction", "Create")  # noqa: E731


def _pending(store, a, xuid, kind):
    return store.db.execute("SELECT 1 FROM payments WHERE auction=? AND xuid=? AND kind=? AND collected=0",
                            (a["id"], xuid, kind)).fetchone() is not None


def actions_for(store, a, xuid):
    v = store.viewer_status(a, xuid)
    m = 0
    mine = a["seller"] == xuid
    if v in ("active", "outbid") and not mine and not a["banned"]:
        m |= BID | (BUYOUT if a["buyout"] else 0)
    if v == "won" and not a["car_collected"]:
        m |= COLLECT_WON
    if mine:
        if a["status"] == "active" and not a["top_bidder"]:
            m |= CANCEL
        if a["status"] in ("expired", "cancelled") and not a["car_collected"] and not a["banned"]:
            m |= COLLECT_FAILED | RESTART
        if a["banned"]:
            m |= REMOVE_BANNED
        if _pending(store, a, xuid, "sold"):
            m |= RECV_SALE
    elif a["status"] == "active":
        m |= REPORT
    if _pending(store, a, xuid, "lost"):
        m |= RECV_REFUND
    return v, m


def base_record(store, a, xuid):
    """ForzaAuction."""
    v, m = actions_for(store, a, xuid)
    return {"id": a["id"], "topBidAmount": a["top_bid"],
            "topBidder": {"qwXuid": int(a["top_bidder"] or 0), "wzGamerTag": a["top_tag"] or ""},
            "nextBidAmount": store.min_next_bid(a), "numBids": store.bid_count(a["id"]),
            "status": STATUS_CODE[v], "reportingState": 0,
            "closingDate": xtsw.iso(a["ends"]), "allowedActions": m}


def extended_record(store, a, xuid):
    """ForzaAuctionExtended."""
    _, thumb = store.get_textures(a["texture_id"])
    return {"base": base_record(store, a, xuid), "carModelId": a["car_ordinal"],
            "buyoutAmount": a["buyout"], "carClass": a["class"], "carYear": a["year"],
            "performanceIndex": float(a["pi"]), "isUnicorn": False,
            "LiveryThumbnail": thumb, "liveryCreator": int(a["seller"])}


def stored_car(a):
    """The Create request as stored, decoded again: car, carHistory, parts, tuning, ..."""
    return xtsw.decode(CREATE(), a["car_blob"])


def gamertag(store, xuid):
    return store.user(xuid)["gamertag"]


@handler("Auction", "GetGlobalConfig")
@guarded
def get_config(ctx, body):
    c = ctx.state.store.cfg
    sch = layouts.result("Auction", "GetGlobalConfig")
    return xtsw.encode(sch, {
        "feeData": [{"hours": h, "credits": f} for h, f in sorted(c["fees"].items())],
        "searchPriceThresholds": [10_000, 25_000, 50_000, 100_000, 250_000, 500_000, 1_000_000, 5_000_000],
        "searchYearThresholds": [1950, 1960, 1970, 1980, 1990, 2000, 2010],
        "searchPowerThresholds": [100, 200, 300, 400, 500, 750, 1000],
        "refreshParams": {"auctionsRefreshSeconds": 30},
        "defaultTime": 24 * 3600,
        "serverCurrentTime": xtsw.iso(ctx.state.store.clock())})


@handler("Auction", "GetAuctions")
@guarded
def get_auctions(ctx, body):
    r = dec("Auction", "GetAuctions", body)
    s, x = ctx.state.store, xuid_of(ctx)
    rows = s.get_many(r["auctionIds"][:r["maxAuctionResults"] or 100])
    ext = s.get_many(r["extendedAuctionIds"][:r["maxExtendedAuctionResults"] or 100])
    return enc("Auction", "GetAuctions", {
        "auctions": [base_record(s, a, x) for a in rows],
        "extendedAuctions": [extended_record(s, a, x) for a in ext]})


@handler("Auction", "SearchForAuction")
@guarded
def search_auction(ctx, body):
    r = dec("Auction", "SearchForAuction", body)
    p = r["searchParams"]
    s, x = ctx.state.store, xuid_of(ctx)
    rows = s.search(
        seller=str(r["xuidOwner"]) if r["xuidOwner"] else None,
        price_min=p["PriceMin"], price_max=p["PriceMax"], make=p["MakeId"], car_ordinal=p["CarId"],
        country=p["CountryId"], **{"class": p["CarClass"]},
        year_min=p["YearMin"], year_max=p["YearMax"], drive=p["DriveType"],
        power_min=p["PowerMin"], power_max=p["PowerMax"], limit=1000)
    ids = [a["id"] for a in rows][:r["maxResultIds"] or 1000]
    return enc("Auction", "SearchForAuction", {
        "auctionIds": ids, "totalAvailable": len(rows),
        "auctionResults": [extended_record(s, a, x) for a in rows[:r["maxResults"] or 50]]})


@handler("Auction", "GetDetails")
@guarded
def get_details(ctx, body):
    r = dec("Auction", "GetDetails", body)
    s, x = ctx.state.store, xuid_of(ctx)
    a = s.get(r["auctionId"])
    livery, _ = s.get_textures(a["texture_id"])
    details = {**base_record(s, a, x), **stored_car(a),
               "seller": {"qwXuid": int(a["seller"]), "wzGamerTag": a["seller_tag"]},
               "liveryData": livery}
    sch = layouts.result("Auction", "GetDetails")
    return xtsw.encode(sch, layouts.fill(sch, {"auctionDetails": details}))


def _listing(ctx, rows, r):
    s, x = ctx.state.store, xuid_of(ctx)
    return {"auctionIds": [a["id"] for a in rows][:r["maxResultIds"] or 1000],
            "totalAvailable": len(rows),
            "auctionResults": [extended_record(s, a, x) for a in rows[:r["maxResults"] or 50]],
            "auctionUpdateResults": []}


@handler("Auction", "GetMyAuctions")
@guarded
def my_auctions(ctx, body):
    r = dec("Auction", "GetMyAuctions", body)
    return enc("Auction", "GetMyAuctions", _listing(ctx, ctx.state.store.mine(xuid_of(ctx)), r))


@handler("Auction", "GetMyBids")
@guarded
def my_bids(ctx, body):
    r = dec("Auction", "GetMyBids", body)
    return enc("Auction", "GetMyBids", _listing(ctx, ctx.state.store.my_bids(xuid_of(ctx)), r))


def _hours(duration_ms):
    return int(round(duration_ms / 3_600_000))


@handler("Auction", "Create")
@guarded
def create(ctx, body):
    r = dec("Auction", "Create", body)
    s, x = ctx.state.store, xuid_of(ctx)
    c = r["car"]
    car = {"title": "", "make": c["make"], "car_ordinal": c["carId"], "class": c["carClass"],
           "pi": c["perfIndex"], "year": c["year"], "power": c["peakPower"],
           "country": c["countryId"], "drive": c["powertrainId"],
           "texture_id": r["texturesId"], "car_blob": body}
    aid = s.create(x, gamertag(s, x), car, r["openingPrice"], r["buyoutPrice"], _hours(r["durationMs"]))
    return enc("Auction", "Create", {"auctionId": aid})


@handler("Auction", "PlaceBid")
@guarded
def place_bid(ctx, body):
    r = dec("Auction", "PlaceBid", body)
    s, x = ctx.state.store, xuid_of(ctx)
    s.bid(r["auctionId"], x, gamertag(s, x), r["amount"])
    return enc("Auction", "PlaceBid", {"amountRefunded": 0})


@handler("Auction", "Cancel")
@guarded
def cancel(ctx, body):
    r = dec("Auction", "Cancel", body)
    ctx.state.store.cancel(r["auctionId"], xuid_of(ctx))
    return b""


@handler("Auction", "Restart")
@guarded
def restart(ctx, body):
    r = dec("Auction", "Restart", body)
    ctx.state.store.restart(r["auctionId"], xuid_of(ctx), _hours(r["durationMs"]),
                            r["openingPrice"], r["buyoutPrice"])
    return enc("Auction", "Restart", {"auctionIdNew": r["auctionId"]})


@handler("Auction", "RetrieveCarForAuction")
@guarded
def retrieve_car(ctx, body):
    r = dec("Auction", "RetrieveCarForAuction", body)
    s, x = ctx.state.store, xuid_of(ctx)
    a = s.get(r["auctionId"])
    s.retrieve_car(r["auctionId"], x)
    c = stored_car(a)
    sch = layouts.result("Auction", "RetrieveCarForAuction")
    return xtsw.encode(sch, layouts.fill(sch, {"isOwner": a["seller"] == x,
                                              "liveryDetailsEncrypted": c.get("liveryDetails", b"")}, c))


def _payment(kind, method):
    @handler("Auction", method)
    @guarded
    def h(ctx, body):
        r = dec("Auction", method, body)
        paid = ctx.state.store.retrieve_payment(r["auctionId"], xuid_of(ctx), kind)
        return enc("Auction", method, {"bidAmount": paid})
    return h


_payment("sold", "RetrievePaymentForAuctionSold")
_payment("lost", "RetrievePaymentsForAuctionLost")


@handler("Auction", "RemoveMyBannedAuction")
@guarded
def remove_banned(ctx, body):
    r = dec("Auction", "RemoveMyBannedAuction", body)
    s = ctx.state.store
    a = s.get(r["auctionId"])
    if a["seller"] != xuid_of(ctx) or not a["banned"]:
        raise ServiceError(30, "not a banned auction of yours")
    s.db.execute("DELETE FROM auctions WHERE id=?", (a["id"],))
    return b""


@handler("Auction", "ReportContent")
@guarded
def report(ctx, body):
    dec("Auction", "ReportContent", body)  # no moderation queue yet: accepted and dropped
    return b""


@handler("Auction", "UploadAuctionTextures")
@guarded
def upload_textures(ctx, body):
    r = dec("Auction", "UploadAuctionTextures", body)
    tid = ctx.state.store.put_textures(xuid_of(ctx), r["liveryTexture"], r["LiveryThumbnail"])
    return enc("Auction", "UploadAuctionTextures", {"texturesId": tid})
