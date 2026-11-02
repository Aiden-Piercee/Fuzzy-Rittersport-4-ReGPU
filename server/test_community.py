import os, sqlite3, tempfile, unittest
import fm4_live_server as s
import xtsw, layouts, seed
from gamedb import GameDb
from test_server import Base, CARW, Clock


def mini_gamedb():
    p = os.path.join(tempfile.mkdtemp(), "gamedb.slt")
    c = sqlite3.connect(p)
    c.executescript("""
    CREATE TABLE Data_Car(Id INT, Year INT, MakeID INT, ClassID INT, CountryID INT, PerformanceIndex REAL,
      BaseCost INT, IsDrivable INT, IsArcade INT, IsUnicorn INT, SimPeakPower REAL, SimPeakTorque REAL,
      CurbWeight REAL, WeightDistribution REAL, PowertrainID INT, AspirationTypeId INT, SpeedRating REAL,
      AccelerationRating REAL, LaunchRating REAL, BrakingRating REAL, HandlingRating REAL);
    CREATE TABLE CarClasses(Id INT, MaxPerformanceIndex REAL, MaxDisplayPerformanceIndex REAL);
    CREATE TABLE Tracks(id INT, ShortDisplayName TEXT, MediaName TEXT, Length INT, Active INT, IsReverse INT, EnvironmentId INT);
    INSERT INTO CarClasses VALUES(0,0.157,200),(1,0.2224,275),(2,0.2825,350),(3,0.347,425);
    INSERT INTO Tracks VALUES(0,'a','LagunaSeca',3602,1,0,1),(1,'b','Tsukuba',2045,1,0,2);
    """)
    for i in range(30):
        c.execute("INSERT INTO Data_Car VALUES(?,?,?,?,?,?,?,1,0,0,300,300,12,0.5,1,1,5,5,5,5,5)",
                  (1000 + i, 1990 + i % 20, 1 + i % 5, i % 4, 3, 0.05 + i * 0.01, 10000 + i * 1000))
    c.commit(); c.close()
    return p


HDR = {"Title": "My livery", "Description": "nice", "Price": 500, "Quantity": 0, "Type": 1, "PayloadSize": 6,
       "KeywordOne": 2, "KeywordTwo": 3}


class Storefront(Base):
    def upload(self, c, kind="Livery", desc=None, **hdr):
        key = {"Livery": "liveryDesc", "Tuning": "tuningDesc"}[kind]
        fid = c.call("Storefront", kind + "Create", {
            "fileHeader": dict(HDR, **hdr), key: {"base": desc or {"MakeId": 7, "ModelId": 9}},
            "payloadSize": 6, "previewData": b"PV"})[3]["contentFileId"]
        self.assertEqual(c.call("Content", "UploadContent", {"contentId": fid, "payloadOffset": 0, "payloadTotalSize": 6,
                                                            "uploadData": b"abc"})[3]["payloadUploadedSize"], 3)
        self.assertEqual(c.call("Content", "UploadContent", {"contentId": fid, "payloadOffset": 3, "payloadTotalSize": 6,
                                                            "uploadData": b"def"})[3]["payloadUploadedSize"], 6)
        return fid

    def test_upload_search_purchase_credit(self):
        seller, buyer = self.cl(1), self.cl(2)
        fid = self.upload(seller)
        sp = {"PriceMax": -1, "MakeId": 7, "ModelId": -1, "ClassId": -1, "KeywordOneId": -1, "KeywordTwoId": -1,
              "EnvironmentId": -1, "TrackId": -1, "NumLayers": -1}
        r = buyer.call("Storefront", "SearchLiveryFiles", {"searchParams": sp, "maxFiles": 10})[3]["searchResults"]
        self.assertEqual([x["Id"] for x in r], [fid])
        sp["MakeId"] = 8
        self.assertEqual(buyer.call("Storefront", "SearchLiveryFiles", {"searchParams": sp, "maxFiles": 10})[3]["searchResults"], [])
        self.assertEqual(buyer.call("Storefront", "SearchTuningFiles", {"searchParams": dict(sp, MakeId=-1), "maxFiles": 5})[3]["searchResults"], [])
        h = buyer.call("Storefront", "GetFileHeavyWeightData", {"fileCount": 1, "FileIds": [fid]})[3]["fileData"][0]
        self.assertEqual((h["Title"], h["Price"], h["PayloadSize"], h["PreviewPayload"]), ("My livery", 500, 6, b"PV"))
        p = buyer.call("Storefront", "Purchase", {"fileId": fid, "price": 500, "maxPayloadSize": 100, "maxPreviewSize": 100})[3]
        self.assertEqual((p["payload"], p["purchasedFileInfo"]["Title"]), (b"abcdef", "My livery"))
        self.assertEqual(seller.call("Storefront", "QueryOnlineCredit")[3]["credit"], 500)
        self.assertEqual(seller.call("Storefront", "GetTotalSales", {"xuid": 1})[3], {"count": 1, "earnings": 500})
        self.assertEqual(seller.call("Storefront", "RetrieveOnlineCredit")[3]["credit"], 500)
        self.assertEqual(seller.call("Storefront", "QueryOnlineCredit")[3]["credit"], 0)
        self.assertEqual(buyer.call("Storefront", "GetMostDownloadedFiles", {"maxFiles": 5})[3]["MostDownloadedFiles"][0]["DownloadCount"], 1)

    def test_ratings_pages_favorites_remove(self):
        a, b = self.cl(1), self.cl(2)
        fid = self.upload(a)
        b.call("Storefront", "SubmitFileRating", {"fileId": fid, "rating": 4})
        self.cl(3).call("Storefront", "SubmitFileRating", {"fileId": fid, "rating": 2})
        top = b.call("Storefront", "GetTopRatedFiles", {"maxFiles": 5})[3]["TopRatedFiles"]
        self.assertAlmostEqual(top[0]["Rating"], 3.0)
        pg = b.call("Storefront", "GetPageList", {"ownerXuid": 1, "maxPages": 5})[3]["Pages"]
        self.assertEqual(pg[0]["UsedSlots"], 1)
        lst = b.call("Storefront", "GetFileListForPage", {"ownerXuid": 1, "page": 0, "maxFiles": 10})[3]
        self.assertEqual((lst["totalFiles"], lst["Files"][0]["Owner"]["qwXuid"]), (1, 1))
        b.call("Storefront", "AddFavoriteUser", {"favoriteXuid": 1})
        self.assertEqual(b.call("Storefront", "GetStorefrontFavorites", {"storefrontXuid": 2, "maxResults": 5})[3]["results"][0]["qwXuid"], 1)
        self.assertEqual(b.call("Storefront", "RemoveFile", {"fileId": fid})[1], 61)   # not the owner
        a.call("Storefront", "RemoveFile", {"fileId": fid})
        self.assertEqual(b.call("Storefront", "GetFeaturedFiles", {"maxFiles": 5})[3]["FeaturedFiles"], [])

    def test_content_download_chunks(self):
        a = self.cl(1)
        fid = self.upload(a)
        r1 = a.call("Content", "DownloadContent", {"contentId": fid, "payloadOffset": 0, "requestedSize": 4})[3]
        r2 = a.call("Content", "DownloadContent", {"contentId": fid, "payloadOffset": 4, "requestedSize": 4})[3]
        self.assertEqual((r1["downloadData"], r1["isLastBuffer"], r1["totalSize"]), (b"abcd", False, 6))
        self.assertEqual((r2["downloadData"], r2["isLastBuffer"]), (b"ef", True))


class Dlc(Base):
    def test_no_marketplace_downloads(self):
        c = self.cl(1)
        for m, k in (("GetStringTableInfo", "stringTableInfo"), ("GetDBPatchFileInfo", "patchInfo")):
            r = c.call("DLCMetadata", m, {"language": "EN"})[3][k]
            self.assertEqual((r["base"]["Id"], r["base"]["Size"], r["Digest"]), (-1, 0, b""))
        self.assertEqual(c.call("DLCMetadata", "GetThumbnailInfo", {"maxThumbnails": 5})[3], {"thumbnailInfo": []})
        self.assertEqual(c.raw("DLC", "GetFeaturedOfferIds", bytes(4))[3], bytes(4))
        self.assertEqual(c.raw("DLC", "GetUpsellOfferId")[3], bytes(8))
        st, code, _, _ = c.raw("Content", "DownloadContent", bytes(13))
        self.assertEqual((st, code), (2, 6))


class Gifts(Base):
    def test_credits_and_file_gift(self):
        a, b = self.cl(1), self.cl(2)
        gid = a.call("Gifting", "SendCreditsGift", {"recipientXuid": 2, "credits": 5000, "reason": "hi"})[3]["giftId"]
        g = b.call("Gifting", "GetGiftsForUser", {"startAt": 0, "maxResults": 10})[3]
        self.assertEqual((g["nAvailable"], g["results"][0]["senderXuid"]), (1, 1))
        self.assertEqual(b.call("Gifting", "RetrieveCreditsGift", {"giftId": gid})[3]["creditAmount"], 5000)
        self.assertEqual(b.call("Gifting", "GetGiftsForUser", {"startAt": 0, "maxResults": 10})[3]["nAvailable"], 0)
        self.assertEqual(a.call("Gifting", "RetrieveCreditsGift", {"giftId": gid})[1], 64)
        r = a.call("Gift", "LiveryCreate", {"fileHeader": {"RecipientXuid": 2, "Type": 1, "PayloadSize": 3},
                                            "liveryDesc": {"base": {"MakeId": 4, "ModelId": 5}}, "payloadSize": 3,
                                            "previewData": b"p"})[3]
        a.call("Content", "UploadContent", {"contentId": r["contentFileId"], "payloadOffset": 0, "payloadTotalSize": 3, "uploadData": b"xyz"})
        info = b.call("Gifting", "GetFileGiftInfo", {"giftId": r["giftId"]})[3]["fileInfo"]
        self.assertEqual((info["FileId"], info["PayloadSize"]), (r["contentFileId"], 3))
        self.assertEqual(b.call("Gift", "LiveryDownloadInfo", {"fileId": info["FileId"]})[3]["liveryDesc"]["base"]["MakeId"], 4)
        # gift files stay out of the public storefront
        self.assertEqual(b.call("Storefront", "GetMostDownloadedFiles", {"maxFiles": 5})[3]["MostDownloadedFiles"], [])

    def test_car_gift_round_trip(self):
        a, b = self.cl(1), self.cl(2)
        gid = a.call("Gifting", "SendCarGift", {"recipientXuid": 2, "car": CARW, "partsInstalled": b"PP", "tuning": b"TT"})[3]["giftId"]
        r = b.call("Gifting", "RetrieveCarGift", {"giftId": gid})[3]
        self.assertEqual((r["car"]["carId"], r["partsInstalled"], r["tuning"], r["giftListing"]["senderXuid"]), (1234, b"PP", b"TT", 1))


class SharedCars(Base):
    def test_club_garage(self):
        a, b, c = self.cl(1), self.cl(2), self.cl(3)
        for x, t in ((a, "A"), (b, "B"), (c, "C")):
            self.logon_(x, t)
        cid = a.call("Clan", "CreateClan", {"clanTag": "CL", "clanName": "Club"})[3]["clanId"]
        b.call("Clan", "RequestJoiningClan", {"clanId": cid})
        tid = a.call("SharedCar", "UploadCarTextures", {"liveryTexture": b"L", "LiveryThumbnail": b"TH"})[3]["texturesId"]
        sid = a.call("SharedCar", "Create", {"completeCarData": {"modelId": 77, "perfIndex": 0.3, "classId": 3},
                                             "texturesId": tid, "liveryDetails": b"LD", "tuning": b"TU"})[3]["carId"]
        cars = b.call("SharedCar", "GetClubCars", {"maxResults": 10})[3]["carData"]
        self.assertEqual((len(cars), cars[0]["modelId"], cars[0]["owner"]["qwXuid"]), (1, 77, 1))
        self.assertEqual(c.call("SharedCar", "GetClubCars", {"maxResults": 10})[3]["carData"], [])   # not in the club
        self.assertEqual(b.call("SharedCar", "GetCarDetails", {"carCount": 1, "carIds": [sid]})[3]["carData"][0]["thumbnail"], b"TH")
        d = b.call("SharedCar", "DownloadSharedCar", {"carId": sid, "maxLiveryLength": 99, "maxTuningLength": 99})[3]
        self.assertEqual((d["liveryDetails"], d["tuningDetails"]), (b"LD", b"TU"))
        b.call("SharedCar", "IncrementRaceCount", {"carId": sid})
        self.assertEqual(b.call("SharedCar", "GetClubCars", {"maxResults": 10})[3]["carData"][0]["raceCount"], 1)
        a.call("SharedCar", "RemoveSharedCar", {"carId": sid})
        self.assertEqual(b.call("SharedCar", "GetClubCars", {"maxResults": 10})[3]["carData"], [])

    def logon_(self, c, tag):
        c.call("User", "Logon2", {"loginData": {"xuid": c.xuid, "gamertag": tag}})


class Scores(Base):
    def submit(self, c, score, track=0, car=5):
        r = c.call("Scoreboard", "SubmitScoreEx", {"type": 0, "data": {"base": {
            "gameScoreboardId": 3, "score": score, "trackId": track, "carId": car, "carClassId": 2,
            "carPerformanceIndex": 0.25, "isClean": True}}})[3]
        return r

    def test_submit_ghost_rows_rival(self):
        a, b = self.cl(1), self.cl(2)
        r = self.submit(a, 95.5)
        self.assertTrue(r["uploadFiles"])
        a.call("Scoreboard", "UploadFile", {"scoreId": r["scoreId"], "data": b"GHOST"})
        self.submit(b, 99.0); self.submit(b, 101.0)
        rows = a.call("Scoreboard", "GetRows", {"type": 0, "gameScoreboardId": 3, "trackId": 0, "pivotId": -1, "maxResults": 10})[3]
        self.assertEqual([x["user"]["qwXuid"] for x in rows["rows"]], [1, 2])
        self.assertEqual(rows["available"], 2)
        self.assertEqual((rows["userRow"]["position"], rows["userRow"]["score"]), (1, 95.5))
        gid = rows["rows"][0]["ghostFileId"]
        self.assertTrue(gid)
        self.assertEqual(b.call("Scoreboard", "DownloadFile", {"id": gid, "maxSize": 100})[3]["data"], b"GHOST")
        rv = b.call("Scoreboard", "GetRival", {"gameScoreboardId": 3, "rivalXuid": 1})[3]["rival"]["scoreData"]
        self.assertEqual((rv["score"], rv["ghostFileId"]), (95.5, gid))
        self.assertEqual(b.call("Scoreboard", "GetRival", {"gameScoreboardId": 3, "rivalXuid": 9})[1], 67)
        r1 = a.call("Scoreboard", "GetRows", {"type": 0, "gameScoreboardId": 3, "trackId": 0, "startAt": 1, "maxResults": 50})[3]
        self.assertEqual(len(r1["rows"]), 2)   # startAt is 1-based: position 1 is the first row
        self.assertEqual(a.call("Scoreboard", "GetBestScores", {"gameScoreboardId": 3, "trackId": 0})[3]["timeScore"]["score"], 95.5)
        self.assertEqual(a.call("Scoreboard", "GetTimeTrials", {"maxTimeTrialCategories": 4, "maxTimeTrialResults": 10})[3]["timeTrials"], [])

    def test_seeded_boards_and_time_trials(self):
        self.srv.RequestHandlerClass  # server already running with default state; build a second one with seeding
        state = s.State(self.store, None, GameDb(mini_gamedb()), 6)
        srv = s.serve("127.0.0.1", 0, self.log, state)
        import threading, client
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close); self.addCleanup(srv.shutdown)
        c = client.Client("127.0.0.1", srv.server_address[1], 1)
        r = c.call("Scoreboard", "GetRows", {"type": 0, "gameScoreboardId": 3, "trackId": 0, "pivotId": -1, "maxResults": 10})[3]
        self.assertEqual(r["available"], 6)
        scores = [x["score"] for x in r["rows"]]
        self.assertEqual(scores, sorted(scores)); self.assertTrue(60 < scores[0] < 200)
        again = c.call("Scoreboard", "GetRows", {"type": 0, "gameScoreboardId": 3, "trackId": 0, "pivotId": -1, "maxResults": 10})[3]
        self.assertEqual([x["score"] for x in again["rows"]], scores)    # stable, not re-invented
        tt = c.call("Scoreboard", "GetTimeTrials", {"maxTimeTrialCategories": 2, "maxTimeTrialResults": 10})[3]
        self.assertEqual([c["category"] for c in tt["timeTrialCategories"]], [0, 1])
        self.assertEqual(len(tt["timeTrials"]), 10)
        self.assertEqual([c["startIndex"] for c in tt["timeTrialCategories"]], [0, 5])
        self.assertTrue(all(x["active"] and x["userRegistered"] for x in tt["timeTrials"]))
        self.assertEqual(len({x["id"] for x in tt["timeTrials"]}), 10)
        tt7 = c.call("Scoreboard", "GetTimeTrials", {"maxTimeTrialCategories": 7, "maxTimeTrialResults": 150})[3]
        self.assertEqual(len(tt7["timeTrialCategories"]), 7)
        rv = c.call("Scoreboard", "GetRival", {"gameScoreboardId": 7, "rivalXuid": 0})
        self.assertEqual((rv[0], rv[3]["rival"]["scoreData"]["ghostFileId"]), (0, 0))
        self.assertTrue(rv[3]["rival"]["scoreData"]["score"] > 0)


class SeededAuctions(unittest.TestCase):
    def test_seeded_listings_decode_and_top_up(self):
        from auction_store import Store
        clock = Clock()
        store = Store(clock=clock)
        sd = seed.Seeder(store, mini_gamedb(), 8)
        self.assertEqual(sd.tick(force=True), 8)
        rows = store.search(limit=50)
        self.assertEqual(len(rows), 8)
        sch = layouts.request("Auction", "Create")
        for a in rows:
            r = xtsw.decode(sch, a["car_blob"])
            c = r["car"]
            self.assertEqual(r["partsInstalled"], seed.parts_for(c["carId"]))
            self.assertEqual(a["car_ordinal"], c["carId"])
            self.assertGreaterEqual(a["start_price"], 1000)
            self.assertTrue(a["buyout"] == 0 or a["buyout"] > a["start_price"])
            self.assertIn(a["seller"], [x for x, _ in sd.sellers])
        clock.t += 8 * 24 * 3600
        self.assertEqual(sd.tick(force=True), 8)          # all expired, so topped up again
        self.assertEqual(len(store.search(limit=50)), 8)

    def test_display_pi_matches_real_requests(self):
        classes = [(0.157, 200), (0.2224, 275), (0.282493, 350), (0.347, 425)]
        self.assertEqual(seed.display_pi(0.3020515, classes), 373)
        self.assertEqual(seed.display_pi(0.2365294, classes), 293)


if __name__ == "__main__":
    unittest.main()
