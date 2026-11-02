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

## Options (all off by default)

| Option | Effect |
|---|---|
| `--gamedb PATH` / `FM4_GAMEDB` | the game's `gamedb.slt`, read from a temporary copy |
| `--seed-auctions N` / `FM4_SEED_AUCTIONS` | keep N invented Auction House listings open (stock cars from the game database, 12 fake sellers, random prices and expiry, topped up as they end) |
| `--seed-leaderboards N` / `FM4_SEED_LEADERBOARDS` | invent N rows the first time a leaderboard is opened, and list time trials from the game's tracks (no ghost files) |
| `FM4_ALLOW_SELF_BID=1` | one profile may bid on its own auction (solo testing) |
| `FM4_GRANT_CREDITS=N` | one-off credit grant per profile through the daily bonus |

Credits live in the player's profile, as on the console: the server reports amounts and never
refuses a bid for lack of credits (`track_credits` in `auction_store.DEFAULT_CONFIG` turns server-side
balances on). Sale proceeds and outbid refunds are collected through the auction payment calls or
`Storefront.RetrieveOnlineCredit`, whichever the game calls first.

## Status of the byte layouts

Every request and reply layout is read from `xtsw-layouts.txt`, the game's own type descriptors
(`layouts.py` turns them into schemas). A call with no handler but a known reply record gets an
empty record in the right layout; a request that does not decode gets a service error and is
logged with its body in hex. Still unconfirmed against the game: systemtime fields are sent as
ISO text (forza-live convention), and the numbers the descriptors do not explain (file types,
club roles, gift types, scoreboard types, score direction).

The server listens on localhost by default. Do not expose it to the internet as it is: there is no
authentication beyond the user id header, and no rate or upload limits.
