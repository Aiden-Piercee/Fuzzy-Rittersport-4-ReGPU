"""Community menu services: Storefront (shared liveries, tunes, photos, replays, layer groups),
content upload/download, online credit, Gifting, shared club cars, Scoreboard and time trials.

Layouts come from the game's descriptors. Where the descriptors do not say what a number means
(file Type values, scoreboard types, gift types, score direction) the choice made here is marked
"unverified". Credits live in the player's profile on the console: the server only reports
amounts, so a purchase tells the buyer's game the price and records the seller's earnings, which
the seller collects through Storefront.RetrieveOnlineCredit.

Licence: AGPL-3.0-or-later.
"""
import hashlib, json, random, struct, threading, uuid
import xtsw, layouts
import clan_service
from fm4_live_server import handler, ServiceError
from services import dec, enc, xuid_of, guarded

KINDS = ("Livery", "LayerGroup", "Tuning", "Photo", "Replay")
PAGE_SLOTS = 20       # slots per storefront page (unverified)
GIFT_CAR, GIFT_CREDITS, GIFT_FILE = 0, 1, 2   # gift type numbers (unverified)


def init(db):
    db.executescript("""
    CREATE TABLE IF NOT EXISTS files(id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT, type INTEGER,
      owner TEXT, title TEXT, description TEXT, price INTEGER, quantity INTEGER, downloads INTEGER DEFAULT 0,
      rating_sum REAL DEFAULT 0, rating_n INTEGER DEFAULT 0, submitted REAL, kw1 INTEGER, kw2 INTEGER,
      payload BLOB DEFAULT x'', payload_size INTEGER DEFAULT 0, preview BLOB, descriptor BLOB,
      make INTEGER, model INTEGER, class INTEGER, env INTEGER, track INTEGER, layers INTEGER,
      page INTEGER DEFAULT 0, ordinal INTEGER, review INTEGER DEFAULT 0, removed INTEGER DEFAULT 0,
      gift_for TEXT);
    CREATE TABLE IF NOT EXISTS ratings(file INTEGER, rater TEXT, rating INTEGER, PRIMARY KEY(file, rater));
    CREATE TABLE IF NOT EXISTS favorites(owner TEXT, fav TEXT, PRIMARY KEY(owner, fav));
    CREATE TABLE IF NOT EXISTS earnings(xuid TEXT PRIMARY KEY, credit INTEGER DEFAULT 0, sales INTEGER DEFAULT 0,
      total INTEGER DEFAULT 0);
    CREATE TABLE IF NOT EXISTS prefs(xuid TEXT PRIMARY KEY, default_page INTEGER DEFAULT 0);
    CREATE TABLE IF NOT EXISTS friends(xuid TEXT, friend TEXT, PRIMARY KEY(xuid, friend));
    CREATE TABLE IF NOT EXISTS gifts(id INTEGER PRIMARY KEY AUTOINCREMENT, recipient TEXT, sender TEXT,
      type INTEGER, reason INTEGER DEFAULT 0, amount INTEGER DEFAULT 0, file INTEGER DEFAULT 0,
      body BLOB, retrieved INTEGER DEFAULT 0);
    CREATE TABLE IF NOT EXISTS shared_cars(id INTEGER PRIMARY KEY AUTOINCREMENT, owner TEXT, data BLOB,
      texture_id INTEGER, shared REAL, used REAL, races INTEGER DEFAULT 0, removed INTEGER DEFAULT 0);
    CREATE TABLE IF NOT EXISTS scores(id INTEGER PRIMARY KEY AUTOINCREMENT, guid TEXT, kind INTEGER,
      board INTEGER, track INTEGER, owner TEXT, score REAL, car INTEGER, class INTEGER, pi REAL,
      flags INTEGER, clean INTEGER, submitted REAL, ghost INTEGER DEFAULT 0, seeded INTEGER DEFAULT 0);
    CREATE TABLE IF NOT EXISTS seeded_boards(board INTEGER, track INTEGER, PRIMARY KEY(board, track));
    """)


def _db(ctx):
    db = ctx.state.store.db
    if not getattr(ctx.state, "community_ready", False):
        clan_service._db(ctx)
        init(db)
        ctx.state.community_ready = True
    return db


def unset(v):
    """-1, 0xFFFF and negative numbers mean "no filter" in the game's search structs."""
    return None if (isinstance(v, (int, float)) and not isinstance(v, bool) and (v < 0 or v == 0xFFFF)) else v


# --- files ------------------------------------------------------------------------------------
def file_record(ctx, f):
    """ForzaStorefrontFile."""
    n = f["rating_n"]
    return {"Id": f["id"], "Type": f["type"], "Title": f["title"], "Description": f["description"],
            "Price": f["price"], "Quantity": f["quantity"], "DownloadCount": f["downloads"],
            "Rating": (f["rating_sum"] / n) if n else 0.0, "SubmissionTime": xtsw.iso(f["submitted"]),
            "KeywordOne": f["kw1"], "KeywordTwo": f["kw2"], "Owner": clan_service.user_data(ctx, f["owner"]),
            "PayloadSize": f["payload_size"], "PreviewPayload": f["preview"] or b"", "Ordinal": f["ordinal"],
            "IsUnderReview": bool(f["review"])}


def light_record(f):
    n = f["rating_n"]
    return {"Id": f["id"], "Rating": (f["rating_sum"] / n) if n else 0.0, "Price": f["price"],
            "DownloadCount": f["downloads"], "Ordinal": f["ordinal"]}


def _files(ctx, where="", args=(), order="submitted DESC", limit=100):
    db = _db(ctx)
    q = "SELECT * FROM files WHERE removed=0 AND gift_for IS NULL" + (" AND " + where if where else "")
    return db.execute(q + f" ORDER BY {order} LIMIT ?", (*args, max(1, min(limit, 500)))).fetchall()


def _descriptor(kind, r):
    d = r.get({"Livery": "liveryDesc", "LayerGroup": "layerGroupDesc", "Tuning": "tuningDesc",
               "Photo": "photoDesc", "Replay": "replayDesc"}[kind], {})
    return d.get("base", d)


def _make_file(ctx, kind, body, gift_for=None):
    r = dec("Storefront" if gift_for is None else "Gift", kind + "Create", body)
    db, x = _db(ctx), xuid_of(ctx)
    h = r["fileHeader"]
    d = _descriptor(kind, r)
    page = db.execute("SELECT default_page FROM prefs WHERE xuid=?", (x,)).fetchone()
    ordinal = db.execute("SELECT COALESCE(MAX(ordinal),0)+1 FROM files WHERE owner=?", (x,)).fetchone()[0]
    fid = db.execute(
        "INSERT INTO files(kind,type,owner,title,description,price,quantity,submitted,kw1,kw2,payload_size,preview,"
        "descriptor,make,model,class,env,track,layers,page,ordinal,gift_for) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        (kind, h.get("Type", 0), x, h.get("Title", ""), h.get("Description", ""), h.get("Price", 0),
         h.get("Quantity", 0), ctx.state.store.clock(), h.get("KeywordOne", 0), h.get("KeywordTwo", 0),
         r["payloadSize"] or h.get("PayloadSize", 0), r["previewData"], body,
         d.get("MakeId"), d.get("ModelId"), d.get("ClassId"), d.get("EnvironmentId"), d.get("TrackId"),
         d.get("NumLayers"), page["default_page"] if page else 0, ordinal, gift_for)).lastrowid
    return fid, r


def _make_create(kind):
    @handler("Storefront", kind + "Create")
    @guarded
    def create(ctx, body):
        fid, _ = _make_file(ctx, kind, body)
        return enc("Storefront", kind + "Create", {"contentFileId": fid})
    return create


def _make_search(kind, method):
    @handler("Storefront", method)
    @guarded
    def search(ctx, body):
        r = dec("Storefront", method, body)
        p = {k: unset(v) for k, v in r["searchParams"].items()}
        where, args = ["kind=?"], [kind]
        for col, key in (("make", "MakeId"), ("model", "ModelId"), ("class", "ClassId"), ("env", "EnvironmentId"),
                         ("track", "TrackId"), ("layers", "NumLayers"), ("kw1", "KeywordOneId"), ("kw2", "KeywordTwoId")):
            if p.get(key) is not None and p[key] != 0:
                where.append(f"{col}=?")
                args.append(p[key])
        if p.get("PriceMax") is not None:
            where.append("price<=?")
            args.append(p["PriceMax"])
        for w in (p.get("ManualSearchKeywords") or "").split():
            where.append("(title LIKE ? OR description LIKE ?)")
            args += [f"%{w}%", f"%{w}%"]
        if p.get("GamerTag"):
            where.append("owner IN (SELECT xuid FROM users WHERE gamertag=?)")
            args.append(p["GamerTag"])
        rows = _files(ctx, " AND ".join(where), args, limit=r["maxFiles"] or 100)
        return enc("Storefront", method, {"searchResults": [light_record(f) for f in rows]})
    return search


for _k in KINDS:
    _make_create(_k)
    # the game spells the method SearchLayergroupFiles
    _make_search(_k, "SearchLayergroupFiles" if _k == "LayerGroup" else f"Search{_k}Files")


@handler("Storefront", "GetFeaturedFiles")
@guarded
def featured(ctx, body):
    r = dec("Storefront", "GetFeaturedFiles", body)
    rows = _files(ctx, "downloads>=0", (), "(rating_sum/MAX(rating_n,1)) DESC, downloads DESC", r["maxFiles"] or 20)
    return enc("Storefront", "GetFeaturedFiles", {"FeaturedFiles": [file_record(ctx, f) for f in rows]})


@handler("Storefront", "GetMostDownloadedFiles")
@guarded
def most_downloaded(ctx, body):
    r = dec("Storefront", "GetMostDownloadedFiles", body)
    rows = _files(ctx, "", (), "downloads DESC", r["maxFiles"] or 20)
    return enc("Storefront", "GetMostDownloadedFiles", {"MostDownloadedFiles": [file_record(ctx, f) for f in rows]})


@handler("Storefront", "GetTopRatedFiles")
@guarded
def top_rated(ctx, body):
    r = dec("Storefront", "GetTopRatedFiles", body)
    rows = _files(ctx, "rating_n>0", (), "(rating_sum/rating_n) DESC", r["maxFiles"] or 20)
    return enc("Storefront", "GetTopRatedFiles", {"TopRatedFiles": [file_record(ctx, f) for f in rows]})


@handler("Storefront", "GetFileHeavyWeightData")
@guarded
def heavy(ctx, body):
    r = dec("Storefront", "GetFileHeavyWeightData", body)
    ids = r["FileIds"][:max(0, r["fileCount"] or len(r["FileIds"]))]
    rows = [f for f in _files(ctx, f"id IN ({','.join('?' * len(ids)) or 'NULL'})", ids, limit=500)]
    return enc("Storefront", "GetFileHeavyWeightData", {"fileData": [file_record(ctx, f) for f in rows]})


@handler("Storefront", "GetPageList")
@guarded
def page_list(ctx, body):
    r = dec("Storefront", "GetPageList", body)
    db, owner = _db(ctx), str(r["ownerXuid"] or xuid_of(ctx))
    used = {p["page"]: p["n"] for p in db.execute(
        "SELECT page, COUNT(*) n FROM files WHERE owner=? AND removed=0 AND gift_for IS NULL GROUP BY page", (owner,))}
    pages = sorted(set(used) | {0})
    return enc("Storefront", "GetPageList", {"Pages": [{"Page": p, "TotalSlots": PAGE_SLOTS, "UsedSlots": used.get(p, 0)}
                                                       for p in pages[:r["maxPages"] or 10]]})


@handler("Storefront", "GetFileListForPage")
@guarded
def file_list_for_page(ctx, body):
    r = dec("Storefront", "GetFileListForPage", body)
    owner = str(r["ownerXuid"] or xuid_of(ctx))
    rows = _files(ctx, "owner=? AND page=?", (owner, r["page"]), "ordinal", 500)
    sel = rows[r["startAt"]:r["startAt"] + (r["maxFiles"] or 50)]
    return enc("Storefront", "GetFileListForPage", {"Files": [file_record(ctx, f) for f in sel], "totalFiles": len(rows)})


@handler("Storefront", "SetDefaultPage")
@guarded
def set_default_page(ctx, body):
    r = dec("Storefront", "SetDefaultPage", body)
    _db(ctx).execute("INSERT OR REPLACE INTO prefs(xuid, default_page) VALUES(?,?)", (xuid_of(ctx), r["page"]))
    return b""


def _owned(ctx, fid):
    f = _db(ctx).execute("SELECT * FROM files WHERE id=? AND removed=0", (fid,)).fetchone()
    if not f:
        raise ServiceError(60, "no such file")
    return f


def _mine(ctx, fid):
    f = _owned(ctx, fid)
    if f["owner"] != xuid_of(ctx):
        raise ServiceError(61, "not your file")
    return f


@handler("Storefront", "RemoveFile")
@guarded
def remove_file(ctx, body):
    r = dec("Storefront", "RemoveFile", body)
    _mine(ctx, r["fileId"])
    _db(ctx).execute("UPDATE files SET removed=1 WHERE id=?", (r["fileId"],))
    return b""


@handler("Storefront", "ChangeFileTitle")
@guarded
def change_title(ctx, body):
    r = dec("Storefront", "ChangeFileTitle", body)
    _mine(ctx, r["fileId"])
    _db(ctx).execute("UPDATE files SET title=? WHERE id=?", (r["newTitle"], r["fileId"]))
    return b""


@handler("Storefront", "ChangeFileDescription")
@guarded
def change_desc(ctx, body):
    r = dec("Storefront", "ChangeFileDescription", body)
    _mine(ctx, r["fileId"])
    _db(ctx).execute("UPDATE files SET description=? WHERE id=?", (r["newDescription"], r["fileId"]))
    return b""


@handler("Storefront", "ChangeFilePriceQuantity")
@guarded
def change_price(ctx, body):
    r = dec("Storefront", "ChangeFilePriceQuantity", body)
    _mine(ctx, r["fileId"])
    _db(ctx).execute("UPDATE files SET price=?, quantity=? WHERE id=?", (r["newPrice"], r["newQuantity"], r["fileId"]))
    return b""


@handler("Storefront", "IncrementViews")
@guarded
def views(ctx, body):
    dec("Storefront", "IncrementViews", body)
    return b""


@handler("Storefront", "SubmitFileRating")
@guarded
def rate(ctx, body):
    r = dec("Storefront", "SubmitFileRating", body)
    db = _db(ctx)
    _owned(ctx, r["fileId"])
    db.execute("INSERT OR REPLACE INTO ratings VALUES(?,?,?)", (r["fileId"], xuid_of(ctx), r["rating"]))
    s = db.execute("SELECT COALESCE(SUM(rating),0), COUNT(*) FROM ratings WHERE file=?", (r["fileId"],)).fetchone()
    db.execute("UPDATE files SET rating_sum=?, rating_n=? WHERE id=?", (s[0], s[1], r["fileId"]))
    return b""


@handler("Storefront", "ReportContent")
@guarded
def report(ctx, body):
    r = dec("Storefront", "ReportContent", body)
    _owned(ctx, r["fileId"])   # no moderation queue: accepted and dropped
    return b""


@handler("Storefront", "AddFavoriteUser")
@guarded
def add_fav(ctx, body):
    r = dec("Storefront", "AddFavoriteUser", body)
    _db(ctx).execute("INSERT OR IGNORE INTO favorites VALUES(?,?)", (xuid_of(ctx), str(r["favoriteXuid"])))
    return b""


@handler("Storefront", "RemoveFavoriteUser")
@guarded
def remove_fav(ctx, body):
    r = dec("Storefront", "RemoveFavoriteUser", body)
    _db(ctx).execute("DELETE FROM favorites WHERE owner=? AND fav=?", (xuid_of(ctx), str(r["favoriteXuid"])))
    return b""


@handler("Storefront", "GetStorefrontFavorites")
@guarded
def favs(ctx, body):
    r = dec("Storefront", "GetStorefrontFavorites", body)
    rows = _db(ctx).execute("SELECT fav FROM favorites WHERE owner=? LIMIT ?",
                            (str(r["storefrontXuid"] or xuid_of(ctx)), r["maxResults"] or 50)).fetchall()
    return enc("Storefront", "GetStorefrontFavorites", {"results": [clan_service.user_data(ctx, f["fav"]) for f in rows]})


@handler("Storefront", "GetFriendsPopulatedStorefrontXUIDs")
@guarded
def friends_with_storefronts(ctx, body):
    r = dec("Storefront", "GetFriendsPopulatedStorefrontXUIDs", body)
    db = _db(ctx)
    rows = db.execute("SELECT DISTINCT f.friend FROM friends f JOIN files x ON x.owner=f.friend AND x.removed=0 "
                      "WHERE f.xuid=? LIMIT ?", (str(r["xuid"] or xuid_of(ctx)), r["maxResults"] or 50)).fetchall()
    return enc("Storefront", "GetFriendsPopulatedStorefrontXUIDs",
               {"results": [clan_service.user_data(ctx, f["friend"]) for f in rows]})


@handler("User", "SetFriends")
@guarded
def set_friends(ctx, body):
    r = dec("User", "SetFriends", body)
    db, x = _db(ctx), xuid_of(ctx)
    db.execute("DELETE FROM friends WHERE xuid=?", (x,))
    db.executemany("INSERT OR IGNORE INTO friends VALUES(?,?)", [(x, str(f)) for f in r["friendXuids"]])
    return b""


@handler("Storefront", "GetStats")
@guarded
def stats(ctx, body):
    r = dec("Storefront", "GetStats", body)
    db, x = _db(ctx), str(r["xuid"] or xuid_of(ctx))
    up = {k: db.execute("SELECT COUNT(*) FROM files WHERE owner=? AND kind=? AND removed=0", (x, k)).fetchone()[0] for k in KINDS}
    dn = {k: db.execute("SELECT COALESCE(SUM(downloads),0) FROM files WHERE owner=? AND kind=?", (x, k)).fetchone()[0] for k in KINDS}
    return enc("Storefront", "GetStats", {"stats": {
        "liveriesUploaded": up["Livery"], "liveriesDownloaded": dn["Livery"],
        "layerGroupsUploaded": up["LayerGroup"], "layerGroupsDownloaded": dn["LayerGroup"],
        "tuningFilesUploaded": up["Tuning"], "tuningFilesDownloaded": dn["Tuning"],
        "photosUploaded": up["Photo"], "photosDownloaded": dn["Photo"],
        "replaysUploaded": up["Replay"], "replaysDownloaded": dn["Replay"]}})


def _earn(ctx, xuid):
    db = _db(ctx)
    db.execute("INSERT OR IGNORE INTO earnings(xuid) VALUES(?)", (xuid,))
    return db.execute("SELECT * FROM earnings WHERE xuid=?", (xuid,)).fetchone()


@handler("Storefront", "GetTotalSales")
@guarded
def total_sales(ctx, body):
    r = dec("Storefront", "GetTotalSales", body)
    e = _earn(ctx, str(r["xuid"] or xuid_of(ctx)))
    return enc("Storefront", "GetTotalSales", {"count": e["sales"], "earnings": e["total"]})


def online_credit(ctx, x):
    """Storefront earnings plus auction proceeds and outbid refunds not yet collected."""
    return _earn(ctx, x)["credit"] + ctx.state.store.pending_payments(x)


@handler("Storefront", "QueryOnlineCredit")
@guarded
def query_credit(ctx, body):
    return enc("Storefront", "QueryOnlineCredit", {"credit": online_credit(ctx, xuid_of(ctx))})


@handler("Storefront", "RetrieveOnlineCredit")
@guarded
def retrieve_credit(ctx, body):
    x = xuid_of(ctx)
    total = online_credit(ctx, x)
    ctx.state.store.collect_all_payments(x)
    _db(ctx).execute("UPDATE earnings SET credit=0 WHERE xuid=?", (x,))
    return enc("Storefront", "RetrieveOnlineCredit", {"credit": total})


@handler("Storefront", "AddOnlineCredit")
@guarded
def add_credit(ctx, body):
    r = dec("Storefront", "AddOnlineCredit", body)
    _earn(ctx, xuid_of(ctx))
    _db(ctx).execute("UPDATE earnings SET credit=credit+? WHERE xuid=?", (r["credit"], xuid_of(ctx)))
    return b""


@handler("Storefront", "Purchase")
@guarded
def purchase(ctx, body):
    r = dec("Storefront", "Purchase", body)
    db, x = _db(ctx), xuid_of(ctx)
    f = _owned(ctx, r["fileId"])
    if f["quantity"] > 0 and f["owner"] != x:
        pass  # limited quantity is tracked but not enforced (unverified: what the game expects when sold out)
    price = f["price"] if f["owner"] != x else 0
    if price != r["price"] and f["price"] != r["price"]:
        raise ServiceError(62, "price changed")
    db.execute("UPDATE files SET downloads=downloads+1 WHERE id=?", (f["id"],))
    if price > 0:
        _earn(ctx, f["owner"])
        db.execute("UPDATE earnings SET credit=credit+?, sales=sales+1, total=total+? WHERE xuid=?",
                   (price, price, f["owner"]))
    return enc("Storefront", "Purchase", {
        "returnCredits": 0, "payload": bytes(f["payload"])[:r["maxPayloadSize"] or None] if f["payload"] else b"",
        "previewPayload": (f["preview"] or b"")[:r["maxPreviewSize"] or None],
        "purchasedFileInfo": {"Title": f["title"], "Ordinal": f["ordinal"], "Type": f["type"],
                              "Owner": clan_service.user_data(ctx, f["owner"])}})


# --- content upload / download ------------------------------------------------------------------
@handler("LargeFileUpload", "Create")
@guarded
def large_create(ctx, body):
    r = dec("LargeFileUpload", "Create", body)
    db, x = _db(ctx), xuid_of(ctx)
    fid = db.execute("INSERT INTO files(kind,type,owner,title,description,price,quantity,submitted,payload_size,gift_for)"
                     " VALUES('Raw',0,?,?,'',0,0,?,?,'raw')",
                     (x, r["userFileName"], ctx.state.store.clock(), r["payloadSize"])).lastrowid
    return enc("LargeFileUpload", "Create", {"contentFileId": fid})


@handler("Content", "UploadContent")
@guarded
def upload_content(ctx, body):
    r = dec("Content", "UploadContent", body)
    db = _db(ctx)
    f = db.execute("SELECT * FROM files WHERE id=?", (r["contentId"],)).fetchone()
    if not f or f["owner"] != xuid_of(ctx):
        raise ServiceError(61, "not your upload")
    cur = bytearray(f["payload"] or b"")
    off, data = r["payloadOffset"], r["uploadData"]
    if off > len(cur):
        raise ServiceError(63, "upload offset past the end of what was received")
    cur[off:off + len(data)] = data
    db.execute("UPDATE files SET payload=?, payload_size=? WHERE id=?",
               (bytes(cur), max(f["payload_size"] or 0, r["payloadTotalSize"]), f["id"]))
    return enc("Content", "UploadContent", {"payloadUploadedSize": len(cur)})


@handler("Content", "DownloadContent")
@guarded
def download_content(ctx, body):
    r = dec("Content", "DownloadContent", body)
    f = _db(ctx).execute("SELECT * FROM files WHERE id=?", (r["contentId"],)).fetchone()
    if not f:   # unknown id (the string-table "patch" we do not have): an empty, complete file
        return enc("Content", "DownloadContent", {"isLastBuffer": True, "downloadData": b"", "totalSize": 0})
    data = bytes(f["payload"] or b"")
    chunk = data[r["payloadOffset"]:r["payloadOffset"] + (r["requestedSize"] or len(data))]
    return enc("Content", "DownloadContent", {
        "isLastBuffer": r["payloadOffset"] + len(chunk) >= len(data), "downloadData": chunk, "totalSize": len(data)})


# --- gifting ------------------------------------------------------------------------------------
@handler("Gifting", "GetGiftsForUser")
@guarded
def gifts(ctx, body):
    r = dec("Gifting", "GetGiftsForUser", body)
    rows = _db(ctx).execute("SELECT * FROM gifts WHERE recipient=? AND retrieved=0 ORDER BY id", (xuid_of(ctx),)).fetchall()
    sel = rows[r["startAt"]:r["startAt"] + (r["maxResults"] or 50)]
    return enc("Gifting", "GetGiftsForUser", {"results": [
        {"giftId": g["id"], "senderXuid": int(g["sender"]), "reason": g["reason"], "giftType": g["type"]} for g in sel],
        "nAvailable": len(rows)})


def _gift(ctx, g_id, who=None):
    g = _db(ctx).execute("SELECT * FROM gifts WHERE id=? AND retrieved=0", (g_id,)).fetchone()
    if not g or g["recipient"] != xuid_of(ctx):
        raise ServiceError(64, "no such gift")
    return g


@handler("Gifting", "SendCarGift")
@guarded
def send_car(ctx, body):
    r = dec("Gifting", "SendCarGift", body)
    gid = _db(ctx).execute("INSERT INTO gifts(recipient,sender,type,body) VALUES(?,?,?,?)",
                           (str(r["recipientXuid"]), xuid_of(ctx), GIFT_CAR, body)).lastrowid
    return enc("Gifting", "SendCarGift", {"giftId": gid})


@handler("Gifting", "RetrieveCarGift")
@guarded
def retrieve_car(ctx, body):
    r = dec("Gifting", "RetrieveCarGift", body)
    g = _gift(ctx, r["giftId"])
    sent = xtsw.decode(layouts.request("Gifting", "SendCarGift"), g["body"])
    _db(ctx).execute("UPDATE gifts SET retrieved=1 WHERE id=?", (g["id"],))
    sch = layouts.result("Gifting", "RetrieveCarGift")
    return xtsw.encode(sch, layouts.fill(sch, {"giftListing": {"giftId": g["id"], "senderXuid": int(g["sender"]),
                                                               "reason": g["reason"], "giftType": g["type"]}}, sent))


@handler("Gifting", "SendCreditsGift")
@guarded
def send_credits(ctx, body):
    r = dec("Gifting", "SendCreditsGift", body)
    gid = _db(ctx).execute("INSERT INTO gifts(recipient,sender,type,amount) VALUES(?,?,?,?)",
                           (str(r["recipientXuid"]), xuid_of(ctx), GIFT_CREDITS, r["credits"])).lastrowid
    return enc("Gifting", "SendCreditsGift", {"giftId": gid})


@handler("Gifting", "RetrieveCreditsGift")
@guarded
def retrieve_credits(ctx, body):
    r = dec("Gifting", "RetrieveCreditsGift", body)
    g = _gift(ctx, r["giftId"])
    _db(ctx).execute("UPDATE gifts SET retrieved=1 WHERE id=?", (g["id"],))
    return enc("Gifting", "RetrieveCreditsGift", {"creditAmount": g["amount"]})


def _make_gift_create(kind):
    @handler("Gift", kind + "Create")
    @guarded
    def create(ctx, body):
        r = dec("Gift", kind + "Create", body)
        x = xuid_of(ctx)
        rec = str(r["fileHeader"]["RecipientXuid"])
        fid, _ = _make_file(ctx, kind, body, gift_for=rec)
        gid = _db(ctx).execute("INSERT INTO gifts(recipient,sender,type,file) VALUES(?,?,?,?)",
                               (rec, x, GIFT_FILE, fid)).lastrowid
        return enc("Gift", kind + "Create", {"contentFileId": fid, "giftId": gid})

    @handler("Gift", kind + "DownloadInfo")
    @guarded
    def info(ctx, body):
        r = dec("Gift", kind + "DownloadInfo", body)
        f = _owned(ctx, r["fileId"])
        sch = layouts.result("Gift", kind + "DownloadInfo")
        return xtsw.encode(sch, layouts.fill(sch, xtsw.decode(layouts.request("Gift", kind + "Create"), f["descriptor"])))


for _k in KINDS:
    _make_gift_create(_k)


@handler("Gifting", "GetFileGiftInfo")
@guarded
def file_gift_info(ctx, body):
    r = dec("Gifting", "GetFileGiftInfo", body)
    g = _gift(ctx, r["giftId"])
    f = _owned(ctx, g["file"])
    return enc("Gifting", "GetFileGiftInfo", {"fileInfo": {"FileId": f["id"], "Type": f["type"],
                                                           "PayloadSize": f["payload_size"], "PreviewPayload": f["preview"] or b""}})


@handler("Gifting", "MarkFileGiftRetrieved")
@guarded
def mark_gift(ctx, body):
    r = dec("Gifting", "MarkFileGiftRetrieved", body)
    g = _gift(ctx, r["giftId"])
    _db(ctx).execute("UPDATE gifts SET retrieved=1 WHERE id=?", (g["id"],))
    return b""


# --- shared club cars ---------------------------------------------------------------------------
@handler("SharedCar", "Create")
@guarded
def shared_create(ctx, body):
    r = dec("SharedCar", "Create", body)
    db, x = _db(ctx), xuid_of(ctx)
    now = ctx.state.store.clock()
    if r["overWriteExisting"]:
        db.execute("UPDATE shared_cars SET removed=1 WHERE owner=?", (x,))
    cid = db.execute("INSERT INTO shared_cars(owner,data,texture_id,shared,used) VALUES(?,?,?,?,?)",
                     (x, body, r["texturesId"], now, now)).lastrowid
    return enc("SharedCar", "Create", {"carId": cid})


@handler("SharedCar", "UploadCarTextures")
@guarded
def shared_textures(ctx, body):
    r = dec("SharedCar", "UploadCarTextures", body)
    tid = ctx.state.store.put_textures(xuid_of(ctx), r["liveryTexture"], r["LiveryThumbnail"])
    return enc("SharedCar", "UploadCarTextures", {"texturesId": tid})


def _club_member_xuids(ctx, x):
    db = _db(ctx)
    m = db.execute("SELECT clan FROM clan_members WHERE xuid=?", (x,)).fetchone()
    if not m:
        return [x]
    return [r["xuid"] for r in db.execute("SELECT xuid FROM clan_members WHERE clan=?", (m["clan"],))]


@handler("SharedCar", "GetClubCars")
@guarded
def club_cars(ctx, body):
    r = dec("SharedCar", "GetClubCars", body)
    db, x = _db(ctx), xuid_of(ctx)
    owners = _club_member_xuids(ctx, x)
    rows = db.execute(f"SELECT * FROM shared_cars WHERE removed=0 AND owner IN ({','.join('?' * len(owners))}) "
                      "ORDER BY used DESC LIMIT ?", (*owners, r["maxResults"] or 50)).fetchall()
    sch = layouts.request("SharedCar", "Create")
    out = []
    for c in rows:
        d = xtsw.decode(sch, c["data"])["completeCarData"]
        info = (ctx.state.gamedb.cars.get(d["modelId"], {}) if ctx.state.gamedb else {})
        out.append(layouts.fill(layouts.result("SharedCar", "GetClubCars")[0][1][1], d, {
            "owner": clan_service.user_data(ctx, c["owner"]), "sharedDate": xtsw.iso(c["shared"]),
            "lastUsedDate": xtsw.iso(c["used"]), "sharedCarId": c["id"], "raceCount": c["races"],
            "year": info.get("Year", 0), "makeId": info.get("MakeID", 0), "countryId": info.get("CountryID", 0),
            "MakeName": info.get("MakeName") or "", "ModelName": info.get("ModelShort") or ""}))
    return enc("SharedCar", "GetClubCars", {"carData": out})


@handler("SharedCar", "GetCarDetails")
@guarded
def car_details(ctx, body):
    r = dec("SharedCar", "GetCarDetails", body)
    db, s = _db(ctx), ctx.state.store
    sch = layouts.request("SharedCar", "Create")
    out = []
    for cid in r["carIds"][:r["carCount"] or len(r["carIds"])]:
        c = db.execute("SELECT * FROM shared_cars WHERE id=? AND removed=0", (cid,)).fetchone()
        if c:
            d = xtsw.decode(sch, c["data"])["completeCarData"]
            out.append({"sharedCarId": cid, "thumbnail": s.get_textures(c["texture_id"])[1],
                        "manufacturerColorIndex": d["manufacturerColorIndex"], "carColorRGB": d["carColorRGB"],
                        "isMetallic": d["isMetallic"], "specialColorIndex": d["specialColorIndex"]})
    return enc("SharedCar", "GetCarDetails", {"carData": out})


@handler("SharedCar", "DownloadSharedCar")
@guarded
def download_shared(ctx, body):
    r = dec("SharedCar", "DownloadSharedCar", body)
    c = _db(ctx).execute("SELECT * FROM shared_cars WHERE id=? AND removed=0", (r["carId"],)).fetchone()
    if not c:
        raise ServiceError(65, "no such shared car")
    d = xtsw.decode(layouts.request("SharedCar", "Create"), c["data"])
    return enc("SharedCar", "DownloadSharedCar", {"liveryDetails": d["liveryDetails"][:r["maxLiveryLength"] or None],
                                                 "tuningDetails": d["tuning"][:r["maxTuningLength"] or None]})


@handler("SharedCar", "RemoveSharedCar")
@guarded
def remove_shared(ctx, body):
    r = dec("SharedCar", "RemoveSharedCar", body)
    _db(ctx).execute("UPDATE shared_cars SET removed=1 WHERE id=? AND owner=?", (r["carId"], xuid_of(ctx)))
    return b""


@handler("SharedCar", "IncrementRaceCount")
@guarded
def shared_race(ctx, body):
    r = dec("SharedCar", "IncrementRaceCount", body)
    _db(ctx).execute("UPDATE shared_cars SET races=races+1, used=? WHERE id=?", (ctx.state.store.clock(), r["carId"]))
    return b""


# --- scoreboards, ghosts, time trials -----------------------------------------------------------
# Score direction is not in the descriptors: lower is better here (lap times). Unverified for drift boards.
def _flags(d):
    return sum(1 << i for i, k in enumerate(("usedSTM", "usedABS", "usedTCS", "usedAutoBrake", "usedAutoShifting",
                                              "usedClutch")) if d.get(k))


def _submit(ctx, method, body):
    r = dec("Scoreboard", method, body)
    d = r["data"].get("base", r["data"])
    db, x = _db(ctx), xuid_of(ctx)
    guid = uuid.uuid4().hex
    db.execute("INSERT INTO scores(guid,kind,board,track,owner,score,car,class,pi,flags,clean,submitted) "
               "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
               (guid, r["type"], d["gameScoreboardId"], d["trackId"], x, d["score"], d["carId"], d["carClassId"],
                d["carPerformanceIndex"], _flags(d), int(d["isClean"]), ctx.state.store.clock()))
    return enc("Scoreboard", method, {"scoreId": guid, "uploadFiles": True})   # ask the game for its ghost file


@handler("Scoreboard", "SubmitScoreEx")
@guarded
def submit_ex(ctx, body):
    return _submit(ctx, "SubmitScoreEx", body)


@handler("Scoreboard", "SubmitScore")
@guarded
def submit(ctx, body):
    return _submit(ctx, "SubmitScore", body)


@handler("Scoreboard", "UploadFile")
@guarded
def upload_ghost(ctx, body):
    r = dec("Scoreboard", "UploadFile", body)
    db, x = _db(ctx), xuid_of(ctx)
    s = db.execute("SELECT * FROM scores WHERE guid=? AND owner=?", (r["scoreId"], x)).fetchone()
    if not s:
        raise ServiceError(66, "no such score")
    fid = db.execute("INSERT INTO files(kind,type,owner,title,description,price,quantity,submitted,payload,payload_size,gift_for)"
                     " VALUES('Ghost',0,?,'','',0,0,?,?,?,'ghost')",
                     (x, ctx.state.store.clock(), r["data"], len(r["data"]))).lastrowid
    db.execute("UPDATE scores SET ghost=? WHERE id=?", (fid, s["id"]))
    return b""


@handler("Scoreboard", "DownloadFile")
@guarded
def download_ghost(ctx, body):
    r = dec("Scoreboard", "DownloadFile", body)
    f = _db(ctx).execute("SELECT payload FROM files WHERE id=? AND kind='Ghost'", (r["id"],)).fetchone()
    return enc("Scoreboard", "DownloadFile", {"data": bytes(f["payload"])[:r["maxSize"] or None] if f else b""})


def score_row(ctx, s, position, rank):
    cl = clan_service.user_data(ctx, s["owner"])
    name = _db(ctx).execute("SELECT name FROM clans WHERE id=?", (cl["clanId"],)).fetchone() if cl["clanId"] else None
    return {"position": position, "rank": rank, "user": cl, "score": s["score"],
            "submittedTime": xtsw.iso(s["submitted"]), "carId": s["car"], "carClassId": s["class"],
            "carPerformanceIndex": s["pi"], "usedSTM": bool(s["flags"] & 1), "usedABS": bool(s["flags"] & 2),
            "usedTCS": bool(s["flags"] & 4), "usedAutoBrake": bool(s["flags"] & 8),
            "usedAutoShifting": bool(s["flags"] & 16), "usedClutch": bool(s["flags"] & 32),
            "isClean": bool(s["clean"]), "ghostFileId": s["ghost"],
            "clanData": {"clanId": cl["clanId"], "clanTag": cl["clanTag"], "clanName": name["name"] if name else ""},
            "bounty": 0}


def _seed_board(ctx, board, track):
    """Invent rows for a board the first time it is asked for (FM4_SEED_LEADERBOARDS=<rows per board>)."""
    st = ctx.state
    if not (st.seed_scores and st.gamedb):
        return
    db = _db(ctx)
    if db.execute("INSERT OR IGNORE INTO seeded_boards VALUES(?,?)", (board, track)).rowcount == 0:
        return
    import seed
    rng = random.Random(board * 100003 + track)       # same board always gets the same rows
    length = (st.gamedb.tracks.get(track) or {}).get("Length") or 3000
    base = length / rng.uniform(34, 46)                # a lap at an average speed of 120-165 km/h
    cars = st.gamedb.sellable_cars()
    for i in range(st.seed_scores):
        xuid, tag = seed.Seeder.fake_user(i)
        st.store.user(xuid, tag)
        c = rng.choice(cars)
        db.execute("INSERT INTO scores(guid,kind,board,track,owner,score,car,class,pi,flags,clean,submitted,seeded) "
                   "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,1)",
                   (uuid.uuid4().hex, 0, board, track, xuid, round(base * rng.uniform(0.92, 1.35), 3), c["Id"],
                    c["ClassID"], c["PerformanceIndex"], 0, int(rng.random() < 0.8),
                    st.store.clock() - rng.uniform(3600, 86400 * 30)))


def _rows(ctx, board, track, start, count, club=False):
    """Best score per player on a board, best first, plus the caller's own row."""
    db, x = _db(ctx), xuid_of(ctx)
    where, args = "board=?", [board]
    if track is not None:
        where += " AND track=?"
        args.append(track)
    if club:
        mem = _club_member_xuids(ctx, x)
        where += f" AND owner IN ({','.join('?' * len(mem))})"
        args += mem
    best = {}
    for s in db.execute(f"SELECT * FROM scores WHERE {where} ORDER BY score ASC, id ASC", args):
        best.setdefault(s["owner"], s)
    ordered = sorted(best.values(), key=lambda s: (s["score"], s["id"]))
    rows = [score_row(ctx, s, i + 1, i + 1) for i, s in enumerate(ordered)]
    mine = next((r for r in rows if r["user"]["qwXuid"] == int(x)), None)
    empty = score_row(ctx, {"owner": x, "score": 0, "submitted": 0, "car": 0, "class": 0, "pi": 0, "flags": 0,
                            "clean": 0, "ghost": 0}, 0, 0)
    return rows[start:start + (count or 50)], (mine or empty), len(rows)


@handler("Scoreboard", "GetRows")
@guarded
def get_rows(ctx, body):
    r = dec("Scoreboard", "GetRows", body)
    track = unset(r["trackId"])
    _seed_board(ctx, r["gameScoreboardId"], track or 0)
    rows, mine, n = _rows(ctx, r["gameScoreboardId"], track, max(0, r["startAt"]), r["maxResults"], r["isClubBoard"])
    return enc("Scoreboard", "GetRows", {"rows": rows, "userRow": mine, "available": n})


@handler("Scoreboard", "GetRollupRows")
@guarded
def get_rollup(ctx, body):
    r = dec("Scoreboard", "GetRollupRows", body)
    board = r["pivotId"]
    _seed_board(ctx, board, 0)
    rows, mine, n = _rows(ctx, board, None, max(0, r["startAt"]), r["maxResults"], r["isClubBoard"])
    return enc("Scoreboard", "GetRollupRows", {"rows": rows, "userRow": mine, "available": n})


@handler("Scoreboard", "GetStorefrontRows")
@guarded
def get_storefront_rows(ctx, body):
    dec("Scoreboard", "GetStorefrontRows", body)
    return enc("Scoreboard", "GetStorefrontRows", {"rows": [], "available": 0})


@handler("Scoreboard", "GetBestScores")
@guarded
def best_scores(ctx, body):
    r = dec("Scoreboard", "GetBestScores", body)
    s = _db(ctx).execute("SELECT MIN(score) m FROM scores WHERE owner=? AND board=? AND track=?",
                         (xuid_of(ctx), r["gameScoreboardId"], r["trackId"])).fetchone()
    return enc("Scoreboard", "GetBestScores", {"timeScore": {"score": s["m"] or 0.0, "isClean": s["m"] is not None}})


@handler("Scoreboard", "GetRival")
@guarded
def get_rival(ctx, body):
    r = dec("Scoreboard", "GetRival", body)
    db = _db(ctx)
    s = db.execute("SELECT * FROM scores WHERE board=? AND owner=? ORDER BY score ASC LIMIT 1",
                   (r["gameScoreboardId"], str(r["rivalXuid"]))).fetchone()
    if not s:
        raise ServiceError(67, "no score from that rival")
    return enc("Scoreboard", "GetRival", {"rival": {"scoreData": score_row(ctx, s, 1, 1), "bounty": 0}})


@handler("Scoreboard", "NotifyRival")
@guarded
def notify_rival(ctx, body):
    dec("Scoreboard", "NotifyRival", body)
    return b""


@handler("Scoreboard", "GetTimeTrials")
@guarded
def time_trials(ctx, body):
    r = dec("Scoreboard", "GetTimeTrials", body)
    st = ctx.state
    if not (st.seed_scores and st.gamedb):
        return enc("Scoreboard", "GetTimeTrials", {})
    tracks = sorted((t for t in st.gamedb.tracks.values() if t["Active"]), key=lambda t: t["id"])
    tracks = tracks[:max(1, min(r["maxTimeTrialResults"] or 10, 10))]
    now = st.store.clock()
    entries = [{"id": t["id"] + 1, "scoreType": 0, "name": t["MediaName"], "description": f"Fastest lap at {t['MediaName']}",
                "notRegisteredMessage": "", "trackId": t["id"], "trackName": t["MediaName"], "hopperId": 0,
                "gameOptionsIndex": 0, "startDate": xtsw.iso(now - 86400), "endDate": xtsw.iso(now + 86400 * 365),
                "userRegistered": True, "featured": i == 0, "active": True} for i, t in enumerate(tracks)]
    return enc("Scoreboard", "GetTimeTrials", {"timeTrialCategories": [{"category": 0, "startIndex": 0}],
                                               "timeTrials": entries})
