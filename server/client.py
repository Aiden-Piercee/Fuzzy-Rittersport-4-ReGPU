#!/usr/bin/env python3
"""Test client for the FM4 title server: sends xtsw calls the way the game does.

  python client.py call Auction GetGlobalConfig                  # empty body
  python client.py call Auction PlaceBid --hex 0000000100000fa0  # raw body
  python client.py demo                                          # full auction walk-through

Licence: AGPL-3.0-or-later.
"""
import argparse, http.client, struct, sys
import xtsw
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
        """Encode `req` with the schema table, return (status, code, message, decoded)."""
        sch = services.SCHEMAS
        body = xtsw.encode(sch[(cls, method, "req")], req or {}) if (cls, method, "req") in sch else b""
        status, code, msg, tail = self.raw(cls, method, body)
        res = None
        if status == 0 and (cls, method, "res") in sch:
            res = xtsw.decode(sch[(cls, method, "res")], tail)
        return status, code, msg, res


def demo(host, port):
    seller, buyer = Client(host, port, 1001), Client(host, port, 2002)
    print("logon", seller.call("User", "Logon2", {"loginData": {"xuid": 1001, "gamertag": "Seller"}})[3]["responseData"]["userData"])
    print("logon", buyer.call("User", "Logon2", {"loginData": {"xuid": 2002, "gamertag": "Buyer"}})[3]["responseData"]["userData"])
    st, code, msg, r = seller.call("Auction", "Create", {
        "title": "Demo car", "car_ordinal": 1234, "class": 3, "pi": 640, "year": 1999,
        "power": 300, "start_price": 20000, "buyout": 60000, "hours": 24, "car_blob": b"car-data"})
    aid = r["id"]
    print("created", aid)
    print("bid", buyer.call("Auction", "PlaceBid", {"id": aid, "amount": 21000})[3])
    print("list", [a["id"] for a in buyer.call("Auction", "GetAuctions", {"limit": 10})[3]["auctions"]])
    print("self-bid refused", seller.call("Auction", "PlaceBid", {"id": aid, "amount": 30000})[:3])


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
