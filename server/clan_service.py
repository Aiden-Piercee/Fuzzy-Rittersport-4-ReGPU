"""Car clubs (Clan.*, CarClub.*) kept in the same SQLite database as the Auction House.

Role numbers are not given by the game's descriptors: 1 member, 2 officer, 3 founder is our
choice and unverified. Invitations, join requests and removals are applied at once; there is no
real second player yet, so a lone profile can create a club, rename it and see itself as
founder. Licence: AGPL-3.0-or-later.
"""
import xtsw, layouts
from fm4_live_server import handler, ServiceError
from services import dec, enc, xuid_of, guarded

MEMBER, OFFICER, FOUNDER = 1, 2, 3
ACTION = {"joined": 1, "left": 2, "created": 3}   # history action codes: unverified


def init(db):
    db.executescript("""
    CREATE TABLE IF NOT EXISTS clans(id INTEGER PRIMARY KEY AUTOINCREMENT, tag TEXT UNIQUE, name TEXT,
      description TEXT DEFAULT '', open INTEGER DEFAULT 1, created REAL);
    CREATE TABLE IF NOT EXISTS clan_members(xuid TEXT PRIMARY KEY, clan INTEGER, role INTEGER, since REAL);
    CREATE TABLE IF NOT EXISTS clan_invites(id INTEGER PRIMARY KEY AUTOINCREMENT, clan INTEGER, xuid TEXT);
    CREATE TABLE IF NOT EXISTS clan_requests(clan INTEGER, xuid TEXT, PRIMARY KEY(clan, xuid));
    CREATE TABLE IF NOT EXISTS clan_history(id INTEGER PRIMARY KEY AUTOINCREMENT, xuid TEXT, clan_name TEXT,
      at REAL, role INTEGER, action INTEGER);
    CREATE TABLE IF NOT EXISTS clan_prefs(xuid TEXT PRIMARY KEY, accepts INTEGER);
    """)


def _db(ctx):
    db = ctx.state.store.db
    if not getattr(ctx.state, "clans_ready", False):
        init(db)
        ctx.state.clans_ready = True
    return db


def user_data(ctx, xuid):
    """UserData record for `xuid`, with club fields filled from the clan tables."""
    db = _db(ctx)
    u = ctx.state.store.user(xuid)
    m = db.execute("SELECT m.clan, m.role, c.tag FROM clan_members m JOIN clans c ON c.id=m.clan "
                   "WHERE m.xuid=?", (xuid,)).fetchone()
    return {"qwXuid": int(xuid), "wzGamerTag": u["gamertag"], "clanId": m["clan"] if m else 0,
            "clanTag": m["tag"] if m else "", "roleInClan": m["role"] if m else 0}


def clan_row(db, clan_id):
    r = db.execute("SELECT * FROM clans WHERE id=?", (clan_id,)).fetchone()
    if r is None:
        raise ServiceError(50, "no such club")
    return r


def details(db, r):
    n = db.execute("SELECT COUNT(*) FROM clan_members WHERE clan=?", (r["id"],)).fetchone()[0]
    return {"clanId": r["id"], "clanTag": r["tag"], "clanName": r["name"], "clanDescription": r["description"],
            "isOpenForMembership": bool(r["open"]), "creationDate": xtsw.iso(r["created"]),
            "carCount": 0, "memberCount": n}


def _join(ctx, xuid, clan_id, role=MEMBER):
    db, s = _db(ctx), ctx.state.store
    if db.execute("SELECT 1 FROM clan_members WHERE xuid=?", (xuid,)).fetchone():
        raise ServiceError(51, "already in a club")
    c = clan_row(db, clan_id)
    db.execute("INSERT INTO clan_members VALUES(?,?,?,?)", (xuid, clan_id, role, s.clock()))
    db.execute("DELETE FROM clan_requests WHERE xuid=?", (xuid,))
    db.execute("DELETE FROM clan_invites WHERE xuid=?", (xuid,))
    db.execute("INSERT INTO clan_history(xuid,clan_name,at,role,action) VALUES(?,?,?,?,?)",
               (xuid, c["name"], s.clock(), role, ACTION["created" if role == FOUNDER else "joined"]))


def _leave(ctx, xuid):
    """A member leaves. When the founder leaves, or the last member does, the club is deleted."""
    db, s = _db(ctx), ctx.state.store
    m = db.execute("SELECT * FROM clan_members WHERE xuid=?", (xuid,)).fetchone()
    if not m:
        return
    c = clan_row(db, m["clan"])
    leaving = [xuid] if m["role"] != FOUNDER else [r["xuid"] for r in db.execute(
        "SELECT xuid FROM clan_members WHERE clan=?", (c["id"],))]
    for x in leaving:
        r = db.execute("SELECT role FROM clan_members WHERE xuid=?", (x,)).fetchone()
        db.execute("DELETE FROM clan_members WHERE xuid=?", (x,))
        db.execute("INSERT INTO clan_history(xuid,clan_name,at,role,action) VALUES(?,?,?,?,?)",
                   (x, c["name"], s.clock(), r["role"], ACTION["left"]))
    if m["role"] == FOUNDER or not db.execute("SELECT 1 FROM clan_members WHERE clan=?", (c["id"],)).fetchone():
        db.execute("DELETE FROM clans WHERE id=?", (c["id"],))
        db.execute("DELETE FROM clan_invites WHERE clan=?", (c["id"],))
        db.execute("DELETE FROM clan_requests WHERE clan=?", (c["id"],))


def _my_clan(ctx, xuid, need_role=OFFICER):
    m = _db(ctx).execute("SELECT * FROM clan_members WHERE xuid=?", (xuid,)).fetchone()
    if not m:
        raise ServiceError(52, "not in a club")
    if m["role"] < need_role:
        raise ServiceError(53, "not allowed in your role")
    return m


@handler("Clan", "CreateClan")
@guarded
def create(ctx, body):
    r = dec("Clan", "CreateClan", body)
    db, x = _db(ctx), xuid_of(ctx)
    tag, name = r["clanTag"].strip(), r["clanName"].strip()
    if not tag or not name or len(tag) > 4 or len(name) > 20:
        raise ServiceError(54, "club tag or name not allowed")
    if db.execute("SELECT 1 FROM clans WHERE tag=? OR name=?", (tag, name)).fetchone():
        raise ServiceError(55, "club tag or name already taken")
    ctx.state.store.user(x)
    cid = db.execute("INSERT INTO clans(tag,name,created) VALUES(?,?,?)",
                     (tag, name, ctx.state.store.clock())).lastrowid
    _join(ctx, x, cid, FOUNDER)
    return enc("Clan", "CreateClan", {"clanId": cid})


@handler("Clan", "GetClanStatus")
@guarded
def status(ctx, body):
    r = dec("Clan", "GetClanStatus", body)
    db, x = _db(ctx), str(r["xuid"] or xuid_of(ctx))
    m = db.execute("SELECT clan FROM clan_members WHERE xuid=?", (x,)).fetchone()
    p = db.execute("SELECT accepts FROM clan_prefs WHERE xuid=?", (x,)).fetchone()
    return enc("Clan", "GetClanStatus", {"clanStatus": {"acceptsClanInvites": bool(p["accepts"]) if p else True,
                                                        "clanId": m["clan"] if m else 0}})


@handler("User", "AcceptClanInvites")
@guarded
def accept_invites(ctx, body):
    r = dec("User", "AcceptClanInvites", body)
    _db(ctx).execute("INSERT OR REPLACE INTO clan_prefs VALUES(?,?)", (xuid_of(ctx), int(r["acceptingClanInvites"])))
    return b""


@handler("Clan", "GetClanMembers")
@guarded
def members(ctx, body):
    r = dec("Clan", "GetClanMembers", body)
    db = _db(ctx)
    rows = db.execute("SELECT xuid FROM clan_members WHERE clan=? ORDER BY role DESC, since", (r["clanId"],)).fetchall()
    return enc("Clan", "GetClanMembers", {
        "clanMembers": [{"user": user_data(ctx, m["xuid"])} for m in rows[:r["maxMembers"] or 100]],
        "totalMembers": len(rows)})


@handler("Clan", "GetClanHistory")
@guarded
def history(ctx, body):
    r = dec("Clan", "GetClanHistory", body)
    db, x = _db(ctx), str(r["userXuid"] or xuid_of(ctx))
    rows = db.execute("SELECT * FROM clan_history WHERE xuid=? ORDER BY at DESC LIMIT ?",
                      (x, max(1, r["maxEntries"]))).fetchall()
    return enc("Clan", "GetClanHistory", {"userClanHistory": [
        {"clanName": h["clan_name"], "date": xtsw.iso(h["at"]), "user": user_data(ctx, x),
         "role": h["role"], "action": h["action"]} for h in rows]})


@handler("Clan", "GetUsersToJoin")
@guarded
def users_to_join(ctx, body):
    r = dec("Clan", "GetUsersToJoin", body)
    db = _db(ctx)
    out = []
    for c in db.execute("SELECT * FROM clans WHERE open=1 LIMIT ?", (r["maxResults"] or 50,)).fetchall():
        f = db.execute("SELECT xuid FROM clan_members WHERE clan=? ORDER BY role DESC LIMIT 1", (c["id"],)).fetchone()
        n = db.execute("SELECT COUNT(*) FROM clan_members WHERE clan=?", (c["id"],)).fetchone()[0]
        if f:
            out.append({"user": user_data(ctx, f["xuid"]), "clanMemberCount": n, "clanName": c["name"],
                        "clanIsOpenForMembership": True})
    return enc("Clan", "GetUsersToJoin", {"usersInClans": out})


@handler("Clan", "GetUsersToInvite")
@guarded
def users_to_invite(ctx, body):
    r = dec("Clan", "GetUsersToInvite", body)
    db, x = _db(ctx), xuid_of(ctx)
    out = []
    for u in db.execute("SELECT xuid FROM users WHERE xuid<>? LIMIT ?", (x, r["maxResults"] or 50)).fetchall():
        d = user_data(ctx, u["xuid"])
        out.append({"user": d, "acceptsClanInvites": True, "hasEverLoggedIn": True,
                    "clanName": ""})
    return enc("Clan", "GetUsersToInvite", {"candidatesForInvite": out})


@handler("Clan", "ChangeClanDetails")
@guarded
def change_details(ctx, body):
    r = dec("Clan", "ChangeClanDetails", body)
    m = _my_clan(ctx, xuid_of(ctx), FOUNDER)
    _db(ctx).execute("UPDATE clans SET name=?, description=?, open=? WHERE id=?",
                     (r["newClanName"] or clan_row(_db(ctx), m["clan"])["name"], r["newClanDescription"],
                      int(r["isOpenForMembership"]), m["clan"]))
    return b""


@handler("Clan", "InviteToClan")
@guarded
def invite(ctx, body):
    r = dec("Clan", "InviteToClan", body)
    m = _my_clan(ctx, xuid_of(ctx))
    _db(ctx).execute("INSERT INTO clan_invites(clan,xuid) VALUES(?,?)", (m["clan"], str(r["userToInvite"])))
    return b""


@handler("Clan", "InviteToClanByName")
@guarded
def invite_by_name(ctx, body):
    r = dec("Clan", "InviteToClanByName", body)
    m = _my_clan(ctx, xuid_of(ctx))
    u = _db(ctx).execute("SELECT xuid FROM users WHERE gamertag=?", (r["usernameToInvite"],)).fetchone()
    if not u:
        raise ServiceError(56, "no such player")
    _db(ctx).execute("INSERT INTO clan_invites(clan,xuid) VALUES(?,?)", (m["clan"], u["xuid"]))
    return b""


@handler("Clan", "AcceptClanInvite")
@guarded
def accept(ctx, body):
    r = dec("Clan", "AcceptClanInvite", body)
    x = xuid_of(ctx)
    if not _db(ctx).execute("SELECT 1 FROM clan_invites WHERE xuid=? AND clan=?", (x, r["clanId"])).fetchone():
        raise ServiceError(57, "no such invitation")
    _join(ctx, x, r["clanId"])
    return b""


@handler("Clan", "RequestJoiningClan")
@guarded
def request_join(ctx, body):
    r = dec("Clan", "RequestJoiningClan", body)
    x, db = xuid_of(ctx), _db(ctx)
    c = clan_row(db, r["clanId"])
    if c["open"]:
        _join(ctx, x, c["id"])
    else:
        db.execute("INSERT OR IGNORE INTO clan_requests VALUES(?,?)", (c["id"], x))
    return b""


@handler("Clan", "RequestJoiningClanByName")
@guarded
def request_join_by_name(ctx, body):
    r = dec("Clan", "RequestJoiningClanByName", body)
    c = _db(ctx).execute("SELECT id FROM clans WHERE name=?", (next(iter(r.values())),)).fetchone()
    if not c:
        raise ServiceError(50, "no such club")
    return request_join(ctx, xtsw.encode(layouts.request("Clan", "RequestJoiningClan"), {"clanId": c["id"]}))


@handler("Clan", "ApproveRequestToJoin")
@guarded
def approve(ctx, body):
    r = dec("Clan", "ApproveRequestToJoin", body)
    m = _my_clan(ctx, xuid_of(ctx))
    x = str(r["approvedUser"])
    if not _db(ctx).execute("SELECT 1 FROM clan_requests WHERE clan=? AND xuid=?", (m["clan"], x)).fetchone():
        raise ServiceError(58, "no such request")
    _join(ctx, x, m["clan"])
    return b""


@handler("Clan", "RemoveFromClan")
@guarded
def remove(ctx, body):
    r = dec("Clan", "RemoveFromClan", body)
    me, x = xuid_of(ctx), str(r["userToRemove"])
    if x != me:
        _my_clan(ctx, me)
    _leave(ctx, x)
    return b""


@handler("Clan", "ChangeMemberRole")
@guarded
def change_role(ctx, body):
    r = dec("Clan", "ChangeMemberRole", body)
    m = _my_clan(ctx, xuid_of(ctx), FOUNDER)
    _db(ctx).execute("UPDATE clan_members SET role=? WHERE xuid=? AND clan=?",
                     (r["newRole"], str(r["userToChange"]), m["clan"]))
    return b""


@handler("CarClub", "GetClanDetails")
@guarded
def clan_details(ctx, body):
    r = dec("CarClub", "GetClanDetails", body)
    db = _db(ctx)
    return enc("CarClub", "GetClanDetails", {"clanDetails": details(db, clan_row(db, r["clanId"]))})


@handler("CarClub", "GetClanFullInfo")
@guarded
def clan_full(ctx, body):
    r = dec("CarClub", "GetClanFullInfo", body)
    db = _db(ctx)
    lead = db.execute("SELECT xuid FROM clan_members WHERE clan=? ORDER BY role DESC, since LIMIT ?",
                      (r["clanId"], max(1, r["maxLeaders"]))).fetchall()
    return enc("CarClub", "GetClanFullInfo", {"clanDetails": details(db, clan_row(db, r["clanId"])),
                                              "leaders": [user_data(ctx, m["xuid"]) for m in lead]})


@handler("User", "GetUserData")
@guarded
def get_user_data(ctx, body):
    return enc("User", "GetUserData", {"userData": user_data(ctx, xuid_of(ctx))})


@handler("Notifications", "RetrieveForUser")
@guarded
def notifications(ctx, body):
    # Nothing to deliver. nextRetrieveSeconds is how long the game waits before asking again:
    # 0 makes it poll in a tight loop.
    return enc("Notifications", "RetrieveForUser", {"nextRetrieveSeconds": 300})


@handler("Clan", "LeaveClan")
def leave(ctx, body):
    # No record for this call in the descriptors (no fields assumed); it leaves the caller's own club.
    _leave(ctx, xuid_of(ctx))
    return b""


@handler("Clan", "DisbandClan")
def disband(ctx, body):
    _my_clan(ctx, xuid_of(ctx), FOUNDER)
    _leave(ctx, xuid_of(ctx))
    return b""


@handler("Clan", "RecommendMemberForRemoval")
@guarded
def recommend_removal(ctx, body):
    dec("Clan", "RecommendMemberForRemoval", body)   # a vote with no effect on its own: accepted
    _my_clan(ctx, xuid_of(ctx), MEMBER)
    return b""
