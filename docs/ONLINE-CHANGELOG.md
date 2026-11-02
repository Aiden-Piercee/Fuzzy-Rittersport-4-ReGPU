# FM4 online mode: changelog and state (branch `multiplayer`)

Everything below is behind `FM2_ONLINE=1` unless it says otherwise. Offline behaviour is unchanged apart from the
two always-on fixes marked (always). Nothing here is pushed.

## How to run a test session
- Title server: `python server\fm4_live_server.py --log <calls.jsonl> --db <fm4_live.sqlite>` (127.0.0.1:8080). Options by
  environment: `FM4_GAMEDB=<gamedb.slt>`, `FM4_SEED_AUCTIONS=30`, `FM4_SEED_LEADERBOARDS=15`, `FM4_ALLOW_SELF_BID=1`,
  `FM4_GRANT_CREDITS=<amount>` (pays once per XUID through `User.RetrieveOnlineCredits`; the daily bonus is display-only).
- Game: `build\fm4-release\fm4.exe <game folder>` directly (not the launcher), with `FM2_ONLINE=1`, `FM2_NO_LAUNCHER=1`,
  `FM2_SERVER_ADDR=127.0.0.1`, and for diagnosis `FM4_TRACE_XAM=1 FM4_TRACE_XAM_LIMIT=200000 FM4_LONG_WAITS=3000 FM4_STALL_DUMP=1`.
  `FM2_SERVER_PORT` (default 8080) is the title server's real port; the game's port 80 is redirected to it.
- Back up `Documents\fm4\profiles\Player` before each session.

## Kernel (src/kernel, src/hooks/fm4)
- Sign-in reads as signed in to Live: `XamUserGetSigninState` 2, `XamUserGetSigninInfo` Live-enabled, user index 0xFF resolves to the
  signed-in user; failures for other users return HRESULT 0x80070525; `XamGetSystemVersion` past the legacy privilege gate.
- Live apps answer success: XMsg apps 0xFB (XGI) and 0xFE (XLive init) and 0xFC messages; 0xFC/0x58004 and 0x58006 write a 1.
- Enumerators: `XamCreateEnumeratorHandle` returns its handle through the eighth argument; an overlapped `XamEnumerate`
  returns its item count in the overlapped length and the NO_MORE_FILES extended error 0x80070012; `XamGetPrivateEnumStructureFromHandle`
  returns a zeroed block; the title-server entry (`0x58039`) carries address, flags and `Turn10.F4Release`.
- Sessions: `XamSessionRefObjByHandle` returns an object with a readable vtable.
- Network (`imports_net.cpp`): `NetOnline()`; `__WSAFDIsSet(socket, set)` argument order fixed; `NetDll_XNetXnAddrToMachineId`;
  connect/sendto redirect of the title server's port 80; `NetDll_` calls traced under `FM4_TRACE_XAM`.
- Achievements: `XamUserCreateAchievementEnumerator` returns an empty enumerator and its buffer size (always). This removed a
  "guest virtual heap exhausted" and a Marketplace "not enough space" dialog.
- `XamContentGetDeviceData` fills the device friendly name (always).
- `XamShowKeyboardUI`: an empty default or a previously generated name is answered with a fresh `Club<4 base-36 chars>` that fits
  the field, and the prompt is logged. Create Club, leave, create again works.
- Synchronous Live requests clear the thread's last error; `XMsgStartIORequest(0xFC, 0x5000C)` (XStringVerify, used by the tune
  name check) answers every string clean (`632d482`).
- Tracing: indirect-call miss logs guest registers; `src/hooks/fm4/live_trace.cpp` logs the Live feature gates and popup builders.

## Server (server/)
- Descriptor-driven layouts for every web call (`server/xtsw-layouts.txt`, read from the running game's type descriptors).
- Services: Logon2, OnlineProfile sync, Notifications, DailyCreditBonus, User (GetUserData, RetrieveOnlineCredits), Auction House
  (browse, search, list, bid, seeded listings), Clan / car clubs (create, leave, details), Storefront, Scoreboard (leaderboards,
  time trials in up to 7 categories, rivals, ghost upload/download), Marketplace metadata (`Id = -1` means nothing to download), Gifting.

## Status after the last session (run 30; the game froze and was closed)
Working: Auction House (browse, search, list a car, seeded cars, bids), Marketplace opens, leaderboards load and scroll, Rival tiles open,
Create Club / leave / create again, credits (+100,000 via `RetrieveOnlineCredits`), the save loads.
Open:
1. **Tune upload**: share gate passes; the cause was an XStringVerify reply we left empty. Fix is in (`632d482`) but run 30 did not show
   whether it worked (no `string verify` line and no Storefront upload request in the log before the freeze).
2. **Rival race**: needs a real ghost uploaded by the game, `ghostFileId = -1` when none, and a `Hopper.GetGameOptions` XML reply
   (server commit `7c0e7eb`); the game froze at or near the "download your rival's ghost" wait, unverified.
3. Saving softlock seen once (run 28), not reproduced in run 29.
4. Race Online (matchmaking) crashed on a null object in the game's session transport (`sub_82CD0808`, `this+164`); not solved.
5. Boot videos cannot be skipped in FM4 yet (the FM2 skip hook is address-bound).

## Commit history
See `git log 26d8fac..HEAD` on branch `multiplayer`: kernel changes `8bc92e3` to `632d482`, server changes `1c2cf4b` to `7c0e7eb`.
