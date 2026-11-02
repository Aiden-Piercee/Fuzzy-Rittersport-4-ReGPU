#!/usr/bin/env python3
"""Test client for the FM4 title server: sends xtsw calls the way the game does.

  python client.py call Auction GetGlobalConfig                  # empty body
  python client.py call Auction PlaceBid --hex 0000000100000fa0  # raw body
  python client.py demo                                          # full auction walk-through

Licence: AGPL-3.0-or-later.
"""
import argparse, http.client, struct, sys
import xtsw
import layouts
import services


class Client:
    def __init__(self, host="127.0.0.1", port=8080, xuid=1, path="/Services.F4Release/o.xtsw"):
        self.host, self.port, self.xuid, self.path = host, port, xuid, path

    def raw(self, cls, method, body=b""):
        c = http.client.HTTPConnection(self.host, self.port, timeout=10)
        c.request("POST", self.path, body, {
            "Content-Type": "bin/xtsw", "User-Agent": "Xls Client",
            "X-TitleId": "1297287625", "X-UserId": str(self.xuid),
            "X-ClassName": f"Forza.WebServices.{cls}, Forza.WebServices", "X-Method": method})
        r = c.getresponse()
        data = r.read()
        c.close()
        if r.status != 200:
            raise RuntimeError(f"HTTP {r.status}")
        status, code, mlen = struct.unpack(">III", data[:12])
        msg = data[12:12 + mlen].decode("utf-8", "replace") if status else ""
        return status, code, msg, data[12 + (mlen if status else 0):]

    def call(self, cls, method, req=None):
        """Encode `req` with the game's layout, return (status, code, message, decoded reply)."""
        sch = services.SCHEMAS
        qs = sch.get((cls, method, "req")) or layouts.request(cls, method)
        rs = sch.get((cls, method, "res")) or layouts.result(cls, method)
        body = xtsw.encode(qs, layouts.fill(qs, req or {}))
        status, code, msg, tail = self.raw(cls, method, body)
        res = xtsw.decode(rs, tail) if status == 0 and rs else None
        return status, code, msg, res


CAR = {"make": 1, "carId": 1234, "carClass": 3, "perfIndex": 640.0, "year": 1999, "peakPower": 300.0}


def demo(host, port):
    seller, buyer = Client(host, port, 1001), Client(host, port, 2002)
    for c, tag in ((seller, "Seller"), (buyer, "Buyer")):
        c.call("User", "Logon2", {"loginData": {"xuid": c.xuid, "gamertag": tag}})
    r = seller.call("Auction", "Create", {"openingPrice": 20000, "buyoutPrice": 60000,
                                          "durationMs": 24 * 3600_000, "car": CAR})
    aid = r[3]["auctionId"]
    print("created", aid, r[:3])
    print("bid", buyer.call("Auction", "PlaceBid", {"auctionId": aid, "amount": 21000})[:3])
    s = buyer.call("Auction", "SearchForAuction", {"maxResultIds": 10, "maxResults": 10})[3]
    print("search ids", s["auctionIds"], [a["base"]["status"] for a in s["auctionResults"]])
    print("self-bid refused", seller.call("Auction", "PlaceBid", {"auctionId": aid, "amount": 30000})[:3])


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--xuid", type=int, default=1)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("call"); c.add_argument("cls"); c.add_argument("method")
    c.add_argument("--hex", default="")
    sub.add_parser("demo")
    a = ap.parse_args(argv)
    if a.cmd == "demo":
        return demo(a.host, a.port)
    st, code, msg, tail = Client(a.host, a.port, a.xuid).raw(a.cls, a.method, bytes.fromhex(a.hex))
    print(f"status={st} code={code} message={msg!r} result={tail[:128].hex()}")


if __name__ == "__main__":
    sys.exit(main())
