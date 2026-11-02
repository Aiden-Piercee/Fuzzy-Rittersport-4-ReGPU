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
import argparse, json, struct, threading, time
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
    def __init__(self, store=None):
        self.store = store


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
            key = (short_class(ctx.class_name), ctx.method)
            fn = HANDLERS.get(key)
            handled = fn is not None
            note = ""
            try:
                out = envelope(fn(ctx, body) if fn else EMPTY_TAIL)
            except ServiceError as e:
                out, note = envelope(status=2, code=e.code, message=e.message), e.message
            except Exception as e:  # report to the game as a service failure
                out, note = envelope(status=2, code=1, message=str(e)), repr(e)
            with LOG_LOCK, open(log_path, "a", encoding="utf-8") as f:
                f.write(json.dumps({"t": time.time(), "class": ctx.class_name,
                                    "method": ctx.method, "xuid": ctx.xuid,
                                    "handled": handled, "len": len(body),
                                    "error": note, "body": body[:4096].hex()}) + "\n")
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
    a = ap.parse_args(argv)
    from auction_store import Store
    srv = serve(a.host, a.port, a.log, State(Store(a.db)))
    print(f"FM4 title server on {a.host}:{a.port}, calls logged to {a.log}, data in {a.db}")
    try: srv.serve_forever()
    except KeyboardInterrupt: pass


if __name__ == "__main__":
    main()
