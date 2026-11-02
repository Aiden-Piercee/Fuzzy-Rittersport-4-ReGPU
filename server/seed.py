"""Seeded Auction House listings (off unless FM4_SEED_AUCTIONS=<count> and a game database).

Cars come from the game's own database (Data_Car in gamedb.slt, read only). The record layout is
the one the game itself sends in Auction.Create (see xtsw-layouts.txt); every field of its `car`
struct maps to a Data_Car column. All seeded cars are stock: part slots follow the pattern seen in
real Create requests (stock part ids are carId*1000, (carId-540)*1000 and (carId-596)*1000 in
fixed slots; -1 where a slot is empty), tuning is left at its "unset" value (-1.0).

Listings belong to a few fake sellers, have random prices and expiry times, and are topped up
as they expire. Fake bidders sometimes hold the top bid. Licence: AGPL-3.0-or-later.
"""
import os, random, sqlite3, struct, tempfile, shutil, threading
import layouts, xtsw

FAKE_TAGS = ["TurboTessa", "DriftKing99", "GarageGuru", "Apex Alley", "NurburgNick", "LeMansLou",
             "BoostedBen", "ClutchKicker", "RoadsterRita", "V8Vince", "HairpinHal", "SlipstreamSam"]
FAKE_BASE = 0xE0000000_00001000   # fake XUIDs: well away from real profiles
# (carId*1000, (carId-540)*1000 or (carId-596)*1000, -1) per slot, 0/1/2 -> family, None -> empty
SLOTS = [None, None, 0, 0, 0, None] + [0] * 8 + [1] * 10 + [None] + [1] + [None] * 7 + [2] * 4 + [0] * 8 + [None]
assert len(SLOTS) == 45


def parts_for(car_id):
    ids = (car_id * 1000, (car_id - 540) * 1000, (car_id - 596) * 1000)
    return struct.pack(">45i", *[-1 if s is None else ids[s] for s in SLOTS])


def display_pi(pi, classes):
    """perfIndexFriendly: linear between class limits (PerformanceIndex -> shown 100..999)."""
    lo_pi, lo_d = 0.0, 100.0
    for max_pi, max_d in classes:
        if pi <= max_pi:
            return int(round(lo_d + (pi - lo_pi) / (max_pi - lo_pi) * (max_d - lo_d)))
        lo_pi, lo_d = max_pi, max_d
    return int(lo_d)


class Seeder:
    def __init__(self, store, gamedb, count, rng=None, interval=15.0):
        self.store, self.count, self.rng = store, count, rng or random.Random()
        self.interval, self.last = interval, -1e9
        self.lock = threading.Lock()
        tmp = tempfile.mkdtemp()  # a copy: the game's file stays untouched, even by SQLite
        path = os.path.join(tmp, "g.db")
        shutil.copy(gamedb, path)
        g = sqlite3.connect(path)
        g.row_factory = sqlite3.Row
        self.cars = [dict(r) for r in g.execute(
            "SELECT * FROM Data_Car WHERE IsDrivable=1 AND IsArcade=0 AND BaseCost>0")]
        self.classes = [(r[0], r[1]) for r in g.execute(
            "SELECT MaxPerformanceIndex, MaxDisplayPerformanceIndex FROM CarClasses ORDER BY Id")]
        g.close()
        shutil.rmtree(tmp, ignore_errors=True)
        self.sellers = [(str(FAKE_BASE + i), t) for i, t in enumerate(FAKE_TAGS)]
        for x, t in self.sellers:
            store.user(x, t)
            store._add_credits(x, 100_000_000)

    def _is_fake(self, xuid):
        return FAKE_BASE <= int(xuid) < FAKE_BASE + len(FAKE_TAGS)

    def car_record(self, row, tag, now_epoch):
        cid = row["Id"]
        return {
            "make": row["MakeID"], "carId": cid, "garageId": self.rng.randint(1, 200), "manColorIndex": 1,
            "countryId": row["CountryID"], "displacementId": (cid - 540) * 1000,
            "aspirationTypeId": row["AspirationTypeId"], "perfIndex": row["PerformanceIndex"],
            "perfIndexFriendly": display_pi(row["PerformanceIndex"], self.classes),
            "carClass": row["ClassID"], "year": row["Year"], "peakPower": row["SimPeakPower"],
            "peakTorque": row["SimPeakTorque"], "curbWeight": row["CurbWeight"],
            "weightDistribution": row["WeightDistribution"], "powertrainId": row["PowertrainID"],
            "isLiveryLocked": True, "isUnicorn": bool(row["IsUnicorn"]),
            "speedRating": row["SpeedRating"], "accelerationRating": row["AccelerationRating"],
            "launchRating": row["LaunchRating"], "brakingRating": row["BrakingRating"],
            "handlingRating": row["HandlingRating"]}

    def one(self):
        r = self.rng
        row = r.choice(self.cars)
        xuid, tag = r.choice(self.sellers)
        hours = r.choice(self.store.cfg["durations_hours"])
        start = max(1000, int(row["BaseCost"] * r.uniform(0.3, 1.3)) // 100 * 100)
        buyout = int(start * r.uniform(1.4, 2.2)) // 100 * 100 if r.random() < 0.55 else 0
        create = {
            "openingPrice": start, "buyoutPrice": buyout, "listingFeePaid": self.store.cfg["fees"].get(hours, 0),
            "durationMs": hours * 3_600_000, "car": self.car_record(row, tag, 0), "texturesId": 0,
            "liveryCreatorXuid": int(xuid), "wzItemGuid": "", "unencryptedSize": 0, "liveryDetails": b"",
            "partsInstalled": parts_for(row["Id"]), "partsInTrunk": bytes(4),
            "tuning": struct.pack(">44f", *([-1.0] * 44)),
            "carHistory": {"base": {"numOwners": 1, "originalOwner": tag}}, "wasLiveryLocked": False}
        sch = layouts.request("Auction", "Create")
        body = xtsw.encode(sch, layouts.fill(sch, create))
        c = create["car"]
        aid = self.store.create(xuid, tag, {
            "make": c["make"], "car_ordinal": c["carId"], "class": c["carClass"], "pi": c["perfIndex"],
            "year": c["year"], "power": c["peakPower"], "country": c["countryId"],
            "drive": c["powertrainId"], "car_blob": body}, start, buyout, hours)
        now = self.store.clock()
        self.store.db.execute("UPDATE auctions SET ends=? WHERE id=?",
                              (now + r.uniform(1800, hours * 3600), aid))
        if r.random() < 0.4:  # a fake bidder already holds the top bid
            bx, bt = r.choice([s for s in self.sellers if s[0] != xuid])
            try:
                self.store.bid(aid, bx, bt, int(start * r.uniform(1.0, 1.4)))
            except Exception:
                pass
        return aid

    def tick(self, force=False):
        """Top the seeded listings up to `count`. Cheap to call on every request."""
        now = self.store.clock()
        with self.lock:
            if not force and now - self.last < self.interval:
                return 0
            self.last = now
            self.store.settle()
            n = self.store.db.execute(
                "SELECT COUNT(*) FROM auctions WHERE status='active' AND CAST(seller AS INTEGER) BETWEEN ? AND ?",
                (FAKE_BASE, FAKE_BASE + len(FAKE_TAGS))).fetchone()[0]
            made = 0
            for _ in range(max(0, self.count - n)):
                self.one()
                made += 1
            return made
