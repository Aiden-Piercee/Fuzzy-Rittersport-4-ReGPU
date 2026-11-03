# FM4 online mode: changelog

## 2026-11-03

Working (tested by hand in the game):
- Tune upload to the Storefront works end to end (`35ef5eb`). The game sends it under the service name `StorefrontTuning`,
  which the server now routes; the tune-name check (`632d482`, 2026-11-02) is answered by the game side.
- Rivals: all seven tabs open, built from the game's own race modes (`6a9fc29`), with the class and car lock, rivals matched to the
  event's class, and a bounty (`e8ed584`). Each tab lists 12 events and the list rotates weekly (`72ed7ee`). Rival times are seeded on
  the event's real track and a first rival you can beat (`35ef5eb`).
- Car Club: create, leave and create again (see 2026-11-02: `7e3b5b9`, `c7bb78a`, `f7933f9`).
- Server tests run on Windows: timestamps before 1970 no longer fail to format (`a0a6c61`).
- Online guide: `docs/online.md` (`a4d3aab`).
- Diagnosis: the winnings formula is logged under `FM4_TRACE_XAM` (`015a3c2`).

Not verified yet:
- Winnings and the Bounty Bonus after beating a Rival: a race with the new trace has not been finished yet, so the payout is unconfirmed.
- Ghost upload after a win: the game has not been seen to send its ghost file yet, so racing your own ghost is untested.

Still broken:
- Race Online (matchmaking) crashes on a null object in the game's session transport.
- Boot videos cannot be skipped.

Earlier notes follow.

# 2026-11-02 and earlier

Branch `multiplayer`, not merged to `main`. Online behaviour is behind `FM2_ONLINE=1`; offline behaviour is unchanged except
for the two items marked (always).

## Working (tested by hand in the game)
- Sign-in reads as signed in to Live, the permission table from Logon2 loads, and the Auction House opens.
- Auction House: browse, search, list a car, bid (seeded listings), My Auctions and My Bids.
- Marketplace opens and finishes its content check.
- Community menu: leaderboards load and scroll, Rival tiles open, Car Club hub opens.
- Car Club: create, leave, and create again.
- Credits: a test grant of 100,000 arrived through the online-credit call.
- The profile save loads.

## Fixed today
Kernel side (src/kernel, src/hooks/fm4):
- Network over Winsock: `NetDll_*` calls, `XNetXnAddrToMachineId`, `__WSAFDIsSet` argument order (`11641d0`), the title server's
  port 80 redirected to its real port (`2d44bdd`, `77bb92c`), `NetOnline()` (`8bc92e3`).
- Title server discovery: enumerators return valid handles and counts (`fe30d3b`, `87ad7f7`, `1930e6d`, `a14f598`); the entry carries
  the address, flags and the `Turn10.F4Release` description.
- Logon: sign-in state 2 and Live-enabled info (`8bc92e3`), Live app messages answered (`10fbf27`), session handle objects (`e16e7f4`).
- Achievement enumerator returns an empty list and a buffer size (`13ef615`, always), which removed a heap failure and the Marketplace
  "not enough space" dialog.
- Device name filled in `XamContentGetDeviceData` (`0a5362d`, always).
- On-screen keyboard: club names and tags are generated and unique (`7e3b5b9`, `c7bb78a`).
- Tune name check (XStringVerify) answered clean and the thread's last error cleared for synchronous Live requests (`632d482`).
- Tracing for diagnosis: guest registers on an indirect-call miss (`a14f598`); the Live feature gates (`fab64ab`, `75eef04`).
Server side (server/):
- Layouts read from the running game's own type descriptors, `server/xtsw-layouts.txt` (`82236a8`, `7da0d12`).
- Logon2 reply with the real layout (`82236a8`); handler lookup fix (`1c2cf4b`).
- Auction House on the game's real records, search treats -1 and 0xFFFF as no filter, seeded listings (`cee5f74`, `250eaf9`).
- Marketplace metadata answers "nothing to download" with file Id -1 (`62e5a71`), `DLC.GetFeaturedOfferIds` (`45e5153`).
- Leaderboards: 1-based start index, rival lookup, time trials in up to 7 categories (`ca8feda`, `08404e2`, `7159012`).
- Car Club: create, leave, disband, details (`250eaf9`, `f7933f9`).
- Community services: Storefront, content upload and download, Gifting, shared club cars, Scoreboard with ghost files (`f7d79de`),
  uploaded blobs stored and ghosts served (`0c69a37`), `ghostFileId = -1` when none, `Hopper.GetGameOptions` for time trials (`7c0e7eb`).
- Tune storage (Storefront tuning files and content upload handlers, `f7d79de`).
- Credits: `FM4_GRANT_CREDITS` once per profile through the online-credit call (`96a95ef`, `fda433e`).

## Still open
- Tune upload: the gate passes and the name check is answered, but the upload has not been seen end to end.
- Rival ghosts: the game needs a ghost file recorded from a real lap; none exists until a first lap is uploaded.
- A save softlock was seen once (run 28) and not reproduced in run 29.
- The game froze in run 30; the log shows no stall or long-wait lines, so the cause is unknown.
- Race Online (matchmaking) crashes on a null object in the game's session transport.
- Boot videos cannot be skipped yet (the existing skip hook is bound to another title's addresses).

## How to run
1. Title server: `python server/fm4_live_server.py --log calls.jsonl --db fm4_live.sqlite` (listens on 127.0.0.1:8080).
   Options by environment: `FM4_GAMEDB=<path to gamedb.slt>`, `FM4_SEED_AUCTIONS=30`, `FM4_SEED_LEADERBOARDS=15`,
   `FM4_ALLOW_SELF_BID=1`, `FM4_GRANT_CREDITS=<amount>` (paid once per profile).
2. Game: run the built `fm4.exe` with the game folder as its argument, directly and not through the launcher, with `FM2_ONLINE=1`,
   `FM2_NO_LAUNCHER=1` and `FM2_SERVER_ADDR=127.0.0.1`. `FM2_SERVER_PORT` (default 8080) is the server's real port.
3. For diagnosis: `FM4_TRACE_XAM=1 FM4_TRACE_XAM_LIMIT=200000 FM4_LONG_WAITS=3000 FM4_STALL_DUMP=1`; the trace is in the game log.
4. Back up the profile folder before a session.
