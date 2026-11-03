# Online mode (`multiplayer` branch)

Experimental. This branch brings back the online screens of Forza Motorsport 4 (Auction House,
Community, Marketplace, Car Clubs, Rivals) by giving the game a title server to talk to. Nothing
here is on `main` yet. It merges once online is stable.

Status marks used below (updated after each test report; last: server `72ed7ee`). The dated history is in `docs/online-changelog.md`:

- **Works**: seen working in a hand test.
- **Broken** / **Untested**: as it says.


## How online works

The original game talks to Turn 10's web services over HTTPS through the Xbox Live layer. We replace
both ends:

1. **Game side (MIT, `src/kernel`, `src/hooks/fm4`).** Online is off unless `FM2_ONLINE=1`. With it
   on, the network calls the game makes (`NetDll_*`) run over Winsock, Live reports you as signed in,
   and the title-server discovery calls return one server: the address you give in `FM2_SERVER_ADDR`.
   Requests for port 80 go to `FM2_SERVER_PORT`.
2. **Title server (AGPL-3.0, `server/`).** A small Python program that answers the game's
   `Forza.WebServices.*` calls (HTTP POST, binary body, `X-ClassName` / `X-Method` headers, big-endian
   records). It keeps its data in a local SQLite file. Request and reply layouts come from the type
   descriptors in the running game, dumped to `server/xtsw-layouts.txt`.

There are no other players unless they point their game at your server. Today it is a single-player
stand-in: the auctions, leaderboards and rivals you see are seeded or come from your own uploads.

Credits stay in your profile, as on the console. The server never blocks a bid for lack of credits.

## What works

All hand-tested in the game unless noted.

| Area | State |
|---|---|
| Live sign-in, title-server discovery, Logon2 (permissions table) | Works |
| Auction House: browse, search, list a car, bid on seeded listings, My Auctions, My Bids | Works |
| Marketplace opens and finishes its content check | Works |
| Community menu, leaderboards (load and scroll), all Rival tiles open | Works |
| Car Club: create, leave, create again | Works |
| Online credit grant (`FM4_GRANT_CREDITS`, paid once per profile) | Works |
| Profile save loads | Works |
| Tune upload to the Storefront | **Works** (server `35ef5eb`). Run 32 confirmed uploaded tunes are stored on the server. Run 31 failed because the game sends the upload under the service name `StorefrontTuning`, which the server did not handle. |
| Rival race: finishing and placing | **Works** (server `35ef5eb`). Beating the rival now scores 1st. Run 31 scored a loss because rival times were seeded for the wrong track and for one lap, not the event's real distance. Rival times are now realistic for the real track, and the made-up rival is a little slower than your best. |
| Rivals tabs (all categories incl. Spec, Track Day, Autocross) | **Works** (v2 server rework; all seven tabs: `6a9fc29`). 12 events per tab rotating weekly (`72ed7ee`) is built but not yet tested in the game. Rivals is marked complete for now. Earlier, Spec, Track Day and Autocross did not. |
| Rival rewards | **Unverified.** Before the bounty change, winning a Rival race paid 0 CR. The server now sends a class-based bounty (see "How Rivals works"), **not yet tested in the game**. Run 32 confirmed Winnings is computed in the game, not by the server, so it may still be 0 for Rivals races. |
| Racing your own ghost on a repeat Rival | **Untested** (not yet checked since the Rival fix). Your ghost is saved when you finish a lap and later rivals race it. |
| Storefront liveries, photos, replays, buying, rating, seller earnings; Gifting; club garage; Scoreboard ghost files; Time Trials | Server answers them and passes its tests. **Untested in the game** except the screens listed above. |
| Race Online (matchmaking) | **Broken.** Crashes on a null object in the game's session transport. Not fixed. |
| LAN / direct-IP racing | **Not started.** |

Other known problems:

- A save softlock was seen once (run 28) and not again in run 29. The game froze once in run 30; the
  cause is unknown.
- The boot videos can't be skipped.
- Because Rivals normally needs a ghost, rival 0 has a target time and no ghost (`ghostFileId` -1).
  If you have driven that board before, the target is within about 4% of your best, otherwise it is a
  fixed estimate. The game does start a no-ghost rival race.

## How Rivals works

1. The Rivals menu calls `Scoreboard.GetTimeTrials`. The server decides every tile: name, track, laps, class.
2. Picking a trial calls `Scoreboard.GetRival`. The server answers with one rival row: gamertag, time, car, class, ghost id and bounty.
3. `Hopper.GetGameOptions` gives the race set-up (track, laps, scoring and the locked car class).
4. If the rival has a ghost, the game downloads it and races it. Without one (ghost id -1) there is only a target time.
5. Win or lose is decided **in the game** by comparing your time with the rival's. The server is not asked.
6. Afterwards the game sends `Scoreboard.SubmitScoreEx` and, for a good run, `Scoreboard.UploadFile` with your ghost. The server stores both and later rivals race your ghost.
7. The payout screen's Bounty Bonus comes from the server's bounty field. Winnings, Difficulty, Race Feats and Damage are worked out in the game.

What the server sends now **(not yet tested in the game)**:

| Category | Trials | Laps | Class |
|---|---|---|---|
| Hot Lap | 5 real circuits | 3 | D, C, B, A, S |
| Top Gear | 5 | 1 | D, C, B, A, S |
| Weekly | 3, rotating each week | 3 | rotating |

- Bounty by class: D 1,500, C 2,500, B 4,000, A 6,000, S 9,000 CR.
- The first rival on a new trial has an easy target. After you have raced, the rival is 0 to 4% slower than your best.
- The v2 rework lists all Rivals categories, and B confirmed every tab works. The table above describes the first version. Playing them has not been checked event by event.
- Switches: `FM4_TRIAL_CLASS_LOCK=0` turns off the class lock if the game rejects it. `FM4_ALL_TRIAL_CATEGORIES=1` brings back all 7 categories as plain hot laps.
- Open questions for the next run: does the game pay the bounty, why is Winnings 0, does the class lock work, does a win upload its ghost.

Weekly rotation: built, with up to 12 events per tab that rotate each week. **Not yet tested in the game.** Showing the events in the in-game messages/news is **not done**. Bounty and Winnings are **untested**.

## Running it

You need Python 3.8 or newer. The server uses only the standard library.

### 1. Start the title server

```
python server/fm4_live_server.py --log calls.jsonl --db fm4_live.sqlite
```

It listens on `127.0.0.1:8080`. Keep it open while you play.

Optional environment settings, set before starting it:

| Variable | Effect |
|---|---|
| `FM4_GAMEDB=<path to gamedb.slt>` | Real cars and tracks for seeded content |
| `FM4_SEED_AUCTIONS=30` | Number of seeded Auction House listings (off if unset) |
| `FM4_SEED_LEADERBOARDS=15` | Invented leaderboard rows per board (off if unset) |
| `FM4_ALLOW_SELF_BID=1` | Let you bid on your own listings, for testing |
| `FM4_GRANT_CREDITS=<amount>` | Pay credits once per profile |

### 2. Start the game

Run the built `fm4.exe` directly, not through the launcher, with the game folder as its argument, and
set:

```
FM2_ONLINE=1
FM2_NO_LAUNCHER=1
FM2_SERVER_ADDR=127.0.0.1
```

`FM2_SERVER_PORT` is the server's real port (default 8080). If you run the server on another machine,
set `FM2_SERVER_ADDR` to its address.

Back up your profile folder before each session.

### 3. Diagnosis switches (game side)

```
FM4_TRACE_XAM=1 FM4_TRACE_XAM_LIMIT=200000 FM4_LONG_WAITS=3000 FM4_STALL_DUMP=1
```

The trace goes into the game log.

### Tests

```
python -m unittest test_server      # in server/, no game needed
python client.py demo               # auction walk-through against a running server
```

## Logs and data

- **`calls.jsonl`**: one line per call the game makes: class, method, request body in hex, and what
  the server replied. A method with no handler gets an empty success, so one play session lists every
  method the game needs. This is the first place to look when something fails.
- **`blobs/`**: files uploaded through the game (tunes, liveries, photos, replays, ghost files). Served
  back to the game when needed.
- **`fm4_live.sqlite`**: auctions, bids, clubs, scores, storefront items. Delete it (with `blobs/`) for
  a clean start.

`blobs/` is created in a folder named `blobs` next to the file you pass as `--log` (so next to `calls.jsonl` in the example above), not in the working directory.

## Troubleshooting

| Symptom | Check |
|---|---|
| Online screens say you can't connect | Is the server running? Is `FM2_ONLINE=1` set? Did you start `fm4.exe` directly with `FM2_SERVER_ADDR`? |
| A screen is empty or errors | Open `calls.jsonl`, find the last calls. A reply with an empty success and no handler means that method is not implemented yet. A decode error means the layout is wrong. Send both the line and the screen. |
| Auction House is empty | Set `FM4_SEED_AUCTIONS`, or list a car yourself. |
| Rival win pays 0 CR | Known, being investigated. |
| Race Online crashes | Known, not fixed. |
| Server must be restarted | After updating `server/`, always restart it. It does not reload its code. |

## Reporting a problem

Please send: what you were doing, the screen, the last 20 lines of `calls.jsonl`, and the game log.
Do not send your profile folder.

## Safety

The server has no authentication beyond the user id header, and no rate or upload limits. Run it on
localhost or a trusted network only. Do not expose it to the internet.

## Credits and licences

The server is licensed AGPL-3.0-or-later (see `server/LICENSE` and `server/NOTICE.md`). It follows the
approach of, and adapts the wire format documented by, forza-live
(https://github.com/FH1-ReImagined/forza-live) and FH2MP (https://github.com/Charelene/FH2MP).
The game-side code stays under the repository's MIT licence. The two only talk over HTTP.
