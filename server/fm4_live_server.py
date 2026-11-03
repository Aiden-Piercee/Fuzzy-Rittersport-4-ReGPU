#!/usr/bin/env python3
"""FM4 title server: stand-in for the Turn 10 web services (Forza.WebServices.*).

The game POSTs /<service path>/o.xtsw with Content-Type bin/xtsw and the call
named in X-ClassName / X-Method. Bodies are big-endian packed records (see
xtsw.py). Replies start with a 12-byte header: u32 status (0 ok), u32 code,
u32 message length (+ UTF-8 message when status != 0).

Methods without a handler get an empty success reply and are written to the
call log (calls.jsonl, with the request body in hex), so one play session
shows exactly which methods and layouts a feature needs.

Licence: AGPL-3.0-or-later (see LICENSE and NOTICE.md).
"""
import argparse, json, os, struct, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from xtsw import Reader  # noqa: F401  (re-exported for handlers)

EMPTY_TAIL = bytes(64)  # trailing zeros read as empty result fields
HANDLERS = {}           # (short class name, method) -> fn(ctx, body) -> bytes
LOG_LOCK = threading.Lock()


def handler(cls, method):
    def deco(fn):
        HANDLERS[(cls, method)] = fn
        return fn
    return deco


def short_class(x_class):
    # "Forza.WebServices.Hopper, Forza.WebServices" -> "Hopper"
    return x_class.split(",")[0].strip().rsplit(".", 1)[-1]


def envelope(results=b"", status=0, code=0, message=""):
    msg = message.encode("utf-8") if status else b""
    return struct.pack(">III", status, code, len(msg)) + msg + results


class ServiceError(Exception):
    """Raise from a handler to answer the game with a service failure."""
    def __init__(self, code, message=""):
        super().__init__(message)
        self.code, self.message = code, message


class Ctx:
    def __init__(self, title_id, class_name, method, xuid, txn, state):
        self.title_id, self.class_name, self.method = title_id, class_name, method
        self.xuid, self.txn, self.state = xuid, txn, state


class State:
    """Everything handlers share. `store` is the Auction House database."""
    def __init__(self, store=None, seeder=None, gamedb=None, seed_scores=0):
        self.store, self.seeder = store, seeder
        self.gamedb = gamedb          # GameDb or None
        self.seed_scores = seed_scores  # leaderboard rows to invent per board (0 = none)


def default_reply(ctx):
    """Unhandled call: an empty record in the method's own reply layout when the game's
    descriptors know it (empty lists, zeros), otherwise a run of zeros."""
    import layouts, xtsw
    cls = short_class(ctx.class_name)
    sch = layouts.result_for(cls, ctx.method)
    if sch is not None:
        return xtsw.encode(sch, {})
    if layouts.has_request(cls, ctx.method):  # a known call with no reply fields: plain success
        return b""
    return EMPTY_TAIL


def _split_class(cls, method):
    """Some services are their own class: StorefrontTuning.Create is the record ForzaStorefrontTuningCreate,
    which is registered here as Storefront.TuningCreate. Try the base class with the suffix moved to the method."""
    for base in sorted({c for c, _ in HANDLERS}, key=len, reverse=True):
        if cls.startswith(base) and len(cls) > len(base):
            rest = cls[len(base):]
            fn = HANDLERS.get((base, rest + method)) or HANDLERS.get((base, method + rest))
            if fn:
                return fn
    return None


def _keep_blob(log_path, ctx, body):
    """Any request whose layout carries a bytes field is also written whole to blobs/ next to the log,
    so a play session leaves every upload (ghosts, liveries, tunes, photos) on disk for study."""
    try:
        import layouts
        if len(body) < 64 or "bytes" not in repr(layouts.request(short_class(ctx.class_name), ctx.method)):
            return None
        d = os.path.join(os.path.dirname(os.path.abspath(log_path)), "blobs")
        os.makedirs(d, exist_ok=True)
        name = f"{int(time.time() * 1000)}_{short_class(ctx.class_name)}_{ctx.method}.bin"
        with open(os.path.join(d, name), "wb") as f:
            f.write(body)
        return {"file": name, "size": len(body), "head": body[:64].hex()}
    except Exception:
        return None


def make_app(log_path, state=None):
    state = state or State()

    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        def log_message(self, *a): pass

        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(n) if n else b""
            ctx = Ctx(self.headers.get("X-TitleId", ""),
                      self.headers.get("X-ClassName", ""),
                      self.headers.get("X-Method", ""),
                      self.headers.get("X-UserId", ""),
                      self.headers.get("X-TransactionId"), state)
            if state.seeder:
                state.seeder.tick()
            key = (short_class(ctx.class_name), ctx.method)
            fn = HANDLERS.get(key) or _split_class(*key)
            handled = fn is not None
            note = ""
            try:
                out = envelope(fn(ctx, body) if fn else default_reply(ctx))
            except ServiceError as e:
                out, note = envelope(status=2, code=e.code, message=e.message), e.message
            except Exception as e:  # report to the game as a service failure
                out, note = envelope(status=2, code=1, message=str(e)), repr(e)
            saved = _keep_blob(log_path, ctx, body)
            with LOG_LOCK, open(log_path, "a", encoding="utf-8") as f:
                rec = {"t": time.time(), "class": ctx.class_name,
                       "method": ctx.method, "xuid": ctx.xuid,
                       "handled": handled, "len": len(body),
                       "error": note, "body": body[:4096].hex()}
                if saved:
                    rec["blob"] = saved
                f.write(json.dumps(rec) + "\n")
            self.send_response(200)
            self.send_header("Content-Type", "bin/xtsw")
            self.send_header("Content-Length", str(len(out)))
            self.end_headers()
            self.wfile.write(out)
    return H


def serve(host, port, log_path, state=None):
    import services  # noqa: F401  registers the handlers
    srv = ThreadingHTTPServer((host, port), make_app(log_path, state))
    srv.daemon_threads = True
    return srv


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default="127.0.0.1")  # local only unless asked
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--log", default="calls.jsonl")
    ap.add_argument("--db", default="fm4_live.sqlite")
    ap.add_argument("--gamedb", default=os.environ.get("FM4_GAMEDB"),
                    help="the game's gamedb.slt (read only), needed for seeded listings")
    ap.add_argument("--seed-leaderboards", type=int, default=int(os.environ.get("FM4_SEED_LEADERBOARDS", "0") or 0),
                    help="invent this many leaderboard rows per board and a list of time trials (0 = off)")
    ap.add_argument("--seed-auctions", type=int, default=int(os.environ.get("FM4_SEED_AUCTIONS", "0") or 0),
                    help="keep this many seeded listings open (0 = off)")
    a = ap.parse_args(argv)
    from auction_store import Store
    store = Store(a.db)
    seeder = None
    if a.seed_auctions:
        if not a.gamedb:
            ap.error("--seed-auctions needs --gamedb (or FM4_GAMEDB)")
        from seed import Seeder
        seeder = Seeder(store, a.gamedb, a.seed_auctions)
        print(f"seeding {a.seed_auctions} auction listings from {len(seeder.cars)} cars")
    gdb = seeder.db if seeder else None
    if a.seed_leaderboards:
        if not a.gamedb:
            ap.error("--seed-leaderboards needs --gamedb (or FM4_GAMEDB)")
        if gdb is None:
            from gamedb import GameDb
            gdb = GameDb(a.gamedb)
    srv = serve(a.host, a.port, a.log, State(store, seeder, gdb, a.seed_leaderboards))
    print(f"FM4 title server on {a.host}:{a.port}, calls logged to {a.log}, data in {a.db}")
    try: srv.serve_forever()
    except KeyboardInterrupt: pass


if __name__ == "__main__":
    # Run through the importable module: services.py imports handler from fm4_live_server, and under
    # __main__ that is a second copy with its own HANDLERS, so no handler would ever be found.
    import fm4_live_server
    fm4_live_server.main()
