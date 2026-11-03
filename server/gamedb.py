"""Read-only view of the game's database (gamedb.slt): cars, classes and tracks.

The file is copied to a temporary location first, so the game's own copy is never opened by SQLite.
Licence: AGPL-3.0-or-later.
"""
import os, shutil, sqlite3, tempfile


class GameDb:
    def __init__(self, path):
        tmp = tempfile.mkdtemp()
        copy = os.path.join(tmp, "g.db")
        shutil.copy(path, copy)
        g = sqlite3.connect(copy)
        g.row_factory = sqlite3.Row
        self.cars = {r["Id"]: dict(r) for r in g.execute("SELECT * FROM Data_Car")}
        self.classes = [(r[0], r[1]) for r in g.execute(
            "SELECT MaxPerformanceIndex, MaxDisplayPerformanceIndex FROM CarClasses ORDER BY Id")]
        self.tracks = {r["id"]: dict(r) for r in g.execute(
            "SELECT id, ShortDisplayName, MediaName, Length, Active, IsReverse, EnvironmentId FROM Tracks")}
        # Career data reused for Rivals: autocross cone layouts with their target times, showroom spec cars,
        # and the Track Day circuits. Missing tables (small test databases) just leave these empty.
        def rows(q):
            try:
                return [dict(r) for r in g.execute(q)]
            except sqlite3.Error:
                return []
        self.autocross = rows(
            "SELECT e.TargetClass AS cls, r.TrackId AS track, r.NumLaps AS laps, s.CollObjLayoutId AS layout,"
            " s.TargetScore_Easy AS easy, s.TargetScore_Pro AS pro FROM Events e JOIN Races r ON r.EventId=e.Id"
            " JOIN RacesSpecial s ON s.RaceId=r.Id WHERE e.CareerTypeId=7 ORDER BY e.Id, r.Id")
        self.spec_cars = [r["CarID"] for r in rows("SELECT CarID FROM EventShowroomChallenges ORDER BY EventID")]
        self.trackday_tracks = sorted({r["TrackId"] for r in rows(
            "SELECT r.TrackId FROM Races r JOIN Events e ON e.Id=r.EventId WHERE e.CareerTypeId=11")})
        g.close()
        shutil.rmtree(tmp, ignore_errors=True)

    def sellable_cars(self):
        return [c for c in self.cars.values() if c["IsDrivable"] and not c["IsArcade"] and c["BaseCost"] > 0]
