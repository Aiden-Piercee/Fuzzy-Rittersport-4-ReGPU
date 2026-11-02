"""Handlers for the Turn 10 services we answer for real.

Request and reply layouts come from the game's own type descriptors
(xtsw-layouts.txt, loaded by layouts.py) unless SCHEMAS overrides them. The
wire encoding of the scalar kinds follows forza-live; timestamps (systemtime)
are sent as ISO text there and that is the one encoding here not yet confirmed
against FM4. A request that does not decode is answered as a service failure
and logged with its body in hex.

Licence: AGPL-3.0-or-later. Structure after forza-live (AGPL-3.0).
"""
import time
import xtsw
import layouts
from fm4_live_server import handler, ServiceError
from auction_store import AuctionError

# --- schemas (unverified) -----------------------------------------------------
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
}


def dec(cls, method, body):
    try:
        return xtsw.decode(SCHEMAS.get((cls, method, "req")) or layouts.request(cls, method), body)
    except xtsw.DecodeError as e:
        raise ServiceError(3, f"{cls}.{method}: request layout mismatch: {e}")


def enc(cls, method, rec):
    return xtsw.encode(SCHEMAS.get((cls, method, "res")) or layouts.result(cls, method), rec)


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


def _clan_fields(ctx, xuid):
    import clan_service
    d = clan_service.user_data(ctx, xuid)
    return {"clanId": d["clanId"], "clanTag": d["clanTag"], "roleInClan": d["roleInClan"]}


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
        "userData": {"xuid": int(xuid), "gamertag": u["gamertag"], **_clan_fields(ctx, xuid)},
        "notificationPollIntervalSeconds": 300,
        "storefrontCap": 100,
        "defaultTimeoutMs": 30000, "defaultContentTimeoutMs": 60000}})



# --- other methods the game calls right after logon ---------------------------------------
def _ack(cls, method):
    @handler(cls, method)
    def h(ctx, body):
        return b""
    return h


# The profile sync calls carry the whole online profile; they have no reply fields.
for _m in ("UpdateAll", "UpdateAllTables", "UpdateCareerCalendar"):
    _ack("OnlineProfile", _m)


@handler("DailyCreditBonus", "GetPayout")
def get_payout(ctx, body):
    # No daily bonus, except a one-off grant per profile for testing: FM4_GRANT_CREDITS=<amount> pays that
    # amount through the next GetPayout of each XUID, once (recorded in the database, so polling cannot repeat it).
    amount = 0
    try:
        grant = int(os.environ.get("FM4_GRANT_CREDITS", "0") or 0)
    except ValueError:
        grant = 0
    if grant > 0:
        db = ctx.state.store.db
        db.execute("CREATE TABLE IF NOT EXISTS grants(xuid TEXT PRIMARY KEY, amount INTEGER)")
        xuid = str(int(ctx.xuid, 0)) if ctx.xuid else ""
        if xuid and db.execute("INSERT OR IGNORE INTO grants VALUES(?,?)", (xuid, grant)).rowcount:
            amount = grant
    return enc("DailyCreditBonus", "GetPayout", {"amount": amount})


@handler("User", "RetrieveOnlineCredits")
def retrieve_online_credits(ctx, body):
    # Credits earned online, collected by the game after a daily-bonus poll. With FM4_GRANT_CREDITS=<amount> the
    # amount is paid here once per XUID as well (a separate record from the GetPayout grant), for testing.
    amount = 0
    try:
        grant = int(os.environ.get("FM4_GRANT_CREDITS", "0") or 0)
    except ValueError:
        grant = 0
    if grant > 0:
        db = ctx.state.store.db
        db.execute("CREATE TABLE IF NOT EXISTS online_grants(xuid TEXT PRIMARY KEY, amount INTEGER)")
        xuid = str(int(ctx.xuid, 0)) if ctx.xuid else ""
        if xuid and db.execute("INSERT OR IGNORE INTO online_grants VALUES(?,?)", (xuid, grant)).rowcount:
            amount = grant
    return enc("User", "RetrieveOnlineCredits", {"credit": amount})


def _no_file_info(method, key):
    """"Nothing to download": a file record with Id -1 (the game then finishes without a download)."""
    @handler("DLCMetadata", method)
    def h(ctx, body):
        return xtsw.encode(layouts.result("DLCMetadata", method), {key: {"base": {"Id": -1}}})
    return h


_no_file_info("GetStringTableInfo", "stringTableInfo")
_no_file_info("GetDBPatchFileInfo", "patchInfo")


import auction_service  # noqa: E402,F401  registers the Auction.* handlers
import clan_service  # noqa: E402,F401
import community_service  # noqa: E402,F401  Storefront, Content, Gifting, SharedCar, Scoreboard  registers Clan.*, CarClub.*, notifications, GetUserData
