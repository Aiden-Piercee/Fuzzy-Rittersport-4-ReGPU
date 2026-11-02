# FM4 title server

Stand-in for the Turn 10 web services (`Forza.WebServices.*`) that Forza Motorsport 4's
online screens talk to. Licence: AGPL-3.0-or-later (see `LICENSE`, `NOTICE.md`).

    python fm4_live_server.py            # 127.0.0.1:8080, calls.jsonl, fm4_live.sqlite
    python -m unittest test_server       # tests, no game needed
    python client.py demo                # auction walk-through against a running server

Python 3.8+, standard library only.

- `fm4_live_server.py`: HTTP framing, dispatch on `X-ClassName`/`X-Method`, reply header, call log.
  A method with no handler gets an empty success and a line in `calls.jsonl` with the request
  body in hex. One play session therefore lists every method and layout the game needs.
- `xtsw.py`: big-endian record codec driven by schemas.
- `services.py`: handlers (User.Logon2, Auction.*) and the table of wire schemas.
- `auction_store.py`: Auction House rules and SQLite storage (credits, bids, fees, expiry,
  payments, relisting). No wire details in it.
- `client.py`: test client.

## Status of the byte layouts

Auction House rules are tested. The **wire layouts are not yet verified against the game**:
no FM4 server source exists, so `SCHEMAS` in `services.py` follows forza-live's conventions and
the method names found in the game. A request that does not decode is answered with a service
error and logged. Fix a layout by editing its line in `SCHEMAS`.

The server listens on localhost by default. Do not expose it to the internet as it is: there is no
authentication beyond the user id header, and no rate or upload limits.
