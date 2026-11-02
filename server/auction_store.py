"""Auction House rules and storage (SQLite). Independent of the wire layouts.

Money model, matching the original design where credits are held by the
server: placing a bid takes credits from the bidder at once; the previous top
bidder's credits become a pending payment (outbid refund) collected through
RetrievePaymentForAuctionLost; the seller's proceeds (price minus fee) become
a pending payment collected through RetrievePaymentForAuctionSold. The car
goes to the winner via RetrieveCarForAuction, or back to the seller when it
did not sell (same call, "failed" side).

Licence: AGPL-3.0-or-later.
"""
import sqlite3, threading, time

ACTIVE, EXPIRED, SOLD, CANCELLED = "active", "expired", "sold", "cancelled"

DEFAULT_CONFIG = {
    "min_start_price": 1000,
    "max_start_price": 50_000_000,
    "min_increment_pct": 5,       # a bid must beat the top by this percent
    "fee_pct": 5,                 # taken from the seller's proceeds
    "durations_hours": [24, 72, 168],
    "starting_credits": 1_000_000,
    "max_active_per_seller": 20,
}


class AuctionError(Exception):
    """Business-rule failure; `code` is returned to the game."""
    def __init__(self, code, msg):
        super().__init__(msg)
        self.code = code


class Store:
    def __init__(self, path=":memory:", config=None, clock=time.time):
        self.cfg = dict(DEFAULT_CONFIG, **(config or {}))
        self.clock = clock
        self.db = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        self.db.row_factory = sqlite3.Row
        self.lock = threading.RLock()
        self.db.executescript("""
        CREATE TABLE IF NOT EXISTS users(xuid TEXT PRIMARY KEY, gamertag TEXT, credits INTEGER);
        CREATE TABLE IF NOT EXISTS auctions(
          id INTEGER PRIMARY KEY AUTOINCREMENT, seller TEXT, seller_tag TEXT,
          title TEXT, car_ordinal INTEGER, class INTEGER, pi INTEGER, year INTEGER,
          make TEXT, country INTEGER, drive INTEGER, power INTEGER,
          car_blob BLOB, textures BLOB,
          start_price INTEGER, buyout INTEGER, created REAL, ends REAL,
          status TEXT, top_bid INTEGER, top_bidder TEXT, top_tag TEXT,
          car_collected INTEGER DEFAULT 0, seller_paid INTEGER DEFAULT 0,
          banned INTEGER DEFAULT 0);
        CREATE TABLE IF NOT EXISTS bids(
          id INTEGER PRIMARY KEY AUTOINCREMENT, auction INTEGER, bidder TEXT,
          amount INTEGER, at REAL);
        CREATE TABLE IF NOT EXISTS payments(
          id INTEGER PRIMARY KEY AUTOINCREMENT, xuid TEXT, auction INTEGER,
          kind TEXT, amount INTEGER, collected INTEGER DEFAULT 0);
        """)

    # -- users ---------------------------------------------------------------
    def user(self, xuid, gamertag=None):
        with self.lock:
            r = self.db.execute("SELECT * FROM users WHERE xuid=?", (xuid,)).fetchone()
            if r is None:
                self.db.execute("INSERT INTO users VALUES(?,?,?)",
                                (xuid, gamertag or f"Player{xuid[-4:]}", self.cfg["starting_credits"]))
            elif gamertag and gamertag != r["gamertag"]:
                self.db.execute("UPDATE users SET gamertag=? WHERE xuid=?", (gamertag, xuid))
            return dict(self.db.execute("SELECT * FROM users WHERE xuid=?", (xuid,)).fetchone())

    def credits(self, xuid):
        return self.user(xuid)["credits"]

    def _add_credits(self, xuid, n):
        self.user(xuid)
        self.db.execute("UPDATE users SET credits=credits+? WHERE xuid=?", (n, xuid))

    # -- lifecycle -----------------------------------------------------------
    def settle(self):
        """Close auctions whose time is up. Safe to call often."""
        now = self.clock()
        with self.lock:
            for a in self.db.execute("SELECT * FROM auctions WHERE status=? AND ends<=?",
                                     (ACTIVE, now)).fetchall():
                self._close(a)

    def _close(self, a):
        if a["top_bidder"]:
            net = a["top_bid"] - a["top_bid"] * self.cfg["fee_pct"] // 100
            self.db.execute("UPDATE auctions SET status=? WHERE id=?", (SOLD, a["id"]))
            self.db.execute("INSERT INTO payments(xuid,auction,kind,amount) VALUES(?,?,?,?)",
                            (a["seller"], a["id"], "sold", net))
        else:
            self.db.execute("UPDATE auctions SET status=? WHERE id=?", (EXPIRED, a["id"]))

    def create(self, seller, gamertag, car, start_price, buyout, hours):
        """`car` is a dict of searchable fields plus car_blob / textures bytes."""
        c = self.cfg
        with self.lock:
            self.settle()
            self.user(seller, gamertag)
            if not c["min_start_price"] <= start_price <= c["max_start_price"]:
                raise AuctionError(10, "start price out of range")
            if buyout and buyout < start_price:
                raise AuctionError(11, "buyout below start price")
            if hours not in c["durations_hours"]:
                raise AuctionError(12, "duration not allowed")
            n = self.db.execute("SELECT COUNT(*) FROM auctions WHERE seller=? AND status=?",
                                (seller, ACTIVE)).fetchone()[0]
            if n >= c["max_active_per_seller"]:
                raise AuctionError(13, "too many active auctions")
            now = self.clock()
            cur = self.db.execute(
                "INSERT INTO auctions(seller,seller_tag,title,car_ordinal,class,pi,year,make,"
                "country,drive,power,car_blob,textures,start_price,buyout,created,ends,status,top_bid)"
                " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0)",
                (seller, gamertag, car.get("title", ""), car.get("car_ordinal", 0),
                 car.get("class", 0), car.get("pi", 0), car.get("year", 0), car.get("make", ""),
                 car.get("country", 0), car.get("drive", 0), car.get("power", 0),
                 car.get("car_blob", b""), car.get("textures", b""), start_price,
                 buyout or 0, now, now + hours * 3600, ACTIVE))
            return cur.lastrowid

    def get(self, auction_id):
        self.settle()
        r = self.db.execute("SELECT * FROM auctions WHERE id=?", (auction_id,)).fetchone()
        if r is None:
            raise AuctionError(1, "no such auction")
        return dict(r)

    def min_next_bid(self, a):
        if not a["top_bidder"]:
            return a["start_price"]
        return a["top_bid"] + max(1, a["top_bid"] * self.cfg["min_increment_pct"] // 100)

    def bid(self, auction_id, bidder, gamertag, amount):
        with self.lock:
            a = self.get(auction_id)
            self.user(bidder, gamertag)
            if a["status"] != ACTIVE or a["banned"]:
                raise AuctionError(20, "auction is not open")
            if a["seller"] == bidder:
                raise AuctionError(21, "cannot bid on your own auction")
            if a["top_bidder"] == bidder:
                raise AuctionError(22, "already the top bidder")
            buyout = a["buyout"] and amount >= a["buyout"]
            if buyout:
                amount = a["buyout"]
            elif amount < self.min_next_bid(a):
                raise AuctionError(23, "bid too low")
            if self.credits(bidder) < amount:
                raise AuctionError(24, "not enough credits")
            self._add_credits(bidder, -amount)
            if a["top_bidder"]:  # refund the previous top bidder as a pending payment
                self.db.execute("INSERT INTO payments(xuid,auction,kind,amount) VALUES(?,?,?,?)",
                                (a["top_bidder"], a["id"], "lost", a["top_bid"]))
            self.db.execute("INSERT INTO bids(auction,bidder,amount,at) VALUES(?,?,?,?)",
                            (a["id"], bidder, amount, self.clock()))
            self.db.execute("UPDATE auctions SET top_bid=?, top_bidder=?, top_tag=? WHERE id=?",
                            (amount, bidder, gamertag, a["id"]))
            if buyout:
                self._close(dict(self.db.execute(
                    "SELECT * FROM auctions WHERE id=?", (a["id"],)).fetchone()))
            return {"top_bid": amount, "bought_out": bool(buyout),
                    "credits": self.credits(bidder)}

    def cancel(self, auction_id, seller):
        with self.lock:
            a = self.get(auction_id)
            if a["seller"] != seller:
                raise AuctionError(30, "not your auction")
            if a["status"] != ACTIVE or a["top_bidder"]:
                raise AuctionError(31, "cannot cancel after a bid")
            self.db.execute("UPDATE auctions SET status=? WHERE id=?", (CANCELLED, a["id"]))

    def restart(self, auction_id, seller, hours):
        """Relist an unsold (expired or cancelled) auction."""
        with self.lock:
            a = self.get(auction_id)
            if a["seller"] != seller:
                raise AuctionError(30, "not your auction")
            if a["status"] not in (EXPIRED, CANCELLED):
                raise AuctionError(32, "only unsold auctions can restart")
            if hours not in self.cfg["durations_hours"]:
                raise AuctionError(12, "duration not allowed")
            now = self.clock()
            self.db.execute("UPDATE auctions SET status=?, created=?, ends=?, car_collected=0 WHERE id=?",
                            (ACTIVE, now, now + hours * 3600, a["id"]))

    def ban(self, auction_id):
        """Moderation: pulls an auction. Bidder is refunded, car returns to seller."""
        with self.lock:
            a = self.get(auction_id)
            if a["status"] == ACTIVE and a["top_bidder"]:
                self.db.execute("INSERT INTO payments(xuid,auction,kind,amount) VALUES(?,?,?,?)",
                                (a["top_bidder"], a["id"], "lost", a["top_bid"]))
            self.db.execute("UPDATE auctions SET banned=1, status=?, top_bidder=NULL WHERE id=?",
                            (CANCELLED, a["id"]))

    # -- collecting -----------------------------------------------------------
    def retrieve_car(self, auction_id, xuid):
        """Winner takes the car; an unsold car goes back to the seller. Once."""
        with self.lock:
            a = self.get(auction_id)
            if a["car_collected"]:
                raise AuctionError(40, "car already collected")
            if a["status"] == SOLD and a["top_bidder"] == xuid:
                pass
            elif a["status"] in (EXPIRED, CANCELLED) and a["seller"] == xuid:
                pass
            else:
                raise AuctionError(41, "no car to collect")
            self.db.execute("UPDATE auctions SET car_collected=1 WHERE id=?", (a["id"],))
            return a["car_blob"]

    def retrieve_payment(self, auction_id, xuid, kind):
        """kind 'sold' (seller proceeds) or 'lost' (outbid refund). Returns credits paid."""
        with self.lock:
            self.settle()
            rows = self.db.execute(
                "SELECT * FROM payments WHERE auction=? AND xuid=? AND kind=? AND collected=0",
                (auction_id, xuid, kind)).fetchall()
            if not rows:
                raise AuctionError(42, "no payment waiting")
            total = sum(r["amount"] for r in rows)
            self.db.execute("UPDATE payments SET collected=1 WHERE auction=? AND xuid=? AND kind=?",
                            (auction_id, xuid, kind))
            self._add_credits(xuid, total)
            return total

    # -- queries --------------------------------------------------------------
    def search(self, status=ACTIVE, **f):
        self.settle()
        q, args = "SELECT * FROM auctions WHERE banned=0 AND status=?", [status]
        for col, op in (("price_min", ">="), ("price_max", "<="), ):
            if f.get(col):
                q += f" AND COALESCE(NULLIF(top_bid,0),start_price){op}?"
                args.append(f[col])
        for col in ("class", "country", "drive", "make", "car_ordinal", "year"):
            if f.get(col):
                q += f" AND {col}=?"
                args.append(f[col])
        for col, op in (("power_min", ">="), ("power_max", "<=")):
            if f.get(col):
                q += f" AND power{op}?"
                args.append(f[col])
        q += " ORDER BY ends ASC LIMIT ? OFFSET ?"
        args += [f.get("limit", 50), f.get("offset", 0)]
        return [dict(r) for r in self.db.execute(q, args).fetchall()]

    def mine(self, xuid):
        self.settle()
        return [dict(r) for r in self.db.execute(
            "SELECT * FROM auctions WHERE seller=? ORDER BY created DESC", (xuid,)).fetchall()]

    def my_bids(self, xuid):
        self.settle()
        return [dict(r) for r in self.db.execute(
            "SELECT a.* FROM auctions a WHERE a.id IN (SELECT auction FROM bids WHERE bidder=?)"
            " ORDER BY a.ends DESC", (xuid,)).fetchall()]

    def viewer_status(self, a, xuid):
        """Per-viewer state: active, expired, sold, cancelled, top bidder, won, outbid, lost."""
        if a["status"] == ACTIVE:
            if a["top_bidder"] == xuid:
                return "top_bidder"
            if self.db.execute("SELECT 1 FROM bids WHERE auction=? AND bidder=?", (a["id"], xuid)).fetchone():
                return "outbid"
            return "active"
        if a["status"] == SOLD:
            if a["top_bidder"] == xuid:
                return "won"
            if self.db.execute("SELECT 1 FROM bids WHERE auction=? AND bidder=?", (a["id"], xuid)).fetchone():
                return "lost"
        return a["status"]
