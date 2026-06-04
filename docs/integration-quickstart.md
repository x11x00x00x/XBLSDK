# Integration quickstart

Stand up the whole loop: build the Test Game, run the server pieces, and submit a
verified score.

## Prerequisites

- **nxdk** toolchain (clang/LLVM, lld) — https://github.com/XboxDev/nxdk
- **mbed TLS** cloned (TLS + HMAC). The Makefile reuses an existing clone by default.
- **XEMU** (to run without hardware) or a modded Xbox with Ethernet.
- Node.js 18+ for the server side (already used by xb.live).

## 1. Build the Test Game

```sh
cd testgame
# Defaults reuse the sibling project's nxdk + mbedtls; override if needed:
#   make NXDK_DIR=/path/to/nxdk MBEDTLS_DIR=/path/to/mbedtls
source "$NXDK_DIR/bin/activate"   # or set NXDK_DIR + PATH manually
make
```

Produces `bin/default.xbe` and a bootable `TestGame.iso`. On macOS the build needs
Homebrew LLVM on PATH, e.g.:

```sh
export NXDK_DIR=/path/to/nxdk
export PATH="/opt/homebrew/opt/llvm/bin:$NXDK_DIR/bin:$PATH"
make NXDK_DIR=$NXDK_DIR
```

## 2. Verify the deterministic core (C ⇄ JS lockstep)

This guarantees the console and server compute identical scores:

```sh
cd sim
clang -O2 -I. gen_vectors.c dodge_sim.c -o /tmp/gen && /tmp/gen > test-vectors.json
node verify_vectors.js     # checks homebrew/dodge-sim.js against the C vectors
```

All cases must report `PASS`.

## 3. Server side (xb.live / "insignia stats")

The homebrew code is additive and self-contained:

- `database.js` creates the `homebrew_*` tables on boot (idempotent) and seeds the
  Test Game + a `highscore` board.
- `server.js` registers the routes:
  ```js
  const { registerHomebrewRoutes } = require('./homebrew/hb-routes');
  registerHomebrewRoutes(app, { db, express, authApiGetUser });
  ```
- The Test Game's HMAC secret is provisioned automatically on first boot to match
  the value baked into the game binary. Override it with `HB_TESTGAME_SECRET`, and
  set `HB_MASTER_KEY` (64 hex chars) to control secret encryption at rest.

Run the route + anti-cheat integration test (no production DB needed — uses an
in-memory SQLite):

```sh
node homebrew/test-hb.js
```

Expect `107 passed, 0 failed`, including: honest score verified, forged score
rejected by replay (even with a valid HMAC), double-submit blocked, wrong-seed
blocked, tampered-hash blocked, admin gating, achievement re-derivation, the
full multiplayer lobby lifecycle (create/join/map/mode/start/progress/replay-verified
finish), game modes (survival/battle/sprint with finish-line clears and winner),
**Live Battle** (real-time 1v1: mode caps the lobby at 2, UDP-endpoint signaling,
and a server replay of the two-player sim from both input logs that decides the
verified winner and records the `livebattle-<mapId>` board), per-(mode x map)
leaderboards (single-player Survival posts to `survival-<mapId>` and is rejected
if the board does not match the run's map; verified multiplayer rounds are
recorded on their `<mode>-<mapId>` board), online-presence heartbeats
(signed, deduped, aging out of the window), and **friends & invites** (the
friends roster reflects live homebrew presence; invites require lobby membership,
are addressed to the right account, can be accepted from the website session-only,
and are dropped when their lobby is disbanded).

## 4. Run it end to end

1. Boot `TestGame.iso` in XEMU (with networking) or `default.xbe` on a console.
2. Sign in. If the console already remembers an Insignia account (from this or
   any other SDK app — accounts live in the shared `E:\Insignia\accounts.txt`
   store), pick it from the launch list to log straight in, or choose "Add
   another account" to scan the QR code for a new one. See
   [sdk-reference.md](sdk-reference.md) → "Multiple accounts & the shared sign-in
   store".
3. Play a run (D-pad / left stick to dodge). On game over the app opens a run,
   submits the score + input log, and shows "Verified!".
4. Press BACK to see the in-game leaderboard, or visit
   `https://xb.live/homebrew?game=og-testgame`.

> Note: the console talks to the live `xb.live` host by default
> (`game/main.c` → `HOST`). Point it at a dev server by changing `HOST`/`PORT`
> (and the auth host) and rebuilding.

## Where things are

- SDK API: [sdk-reference.md](sdk-reference.md)
- REST + HMAC: [rest-api.md](rest-api.md)
- Verification & limits: [anti-cheat.md](anti-cheat.md)
- Simulation rules: [../sim/sim-spec.md](../sim/sim-spec.md)
- Approval: [approval-workflow.md](approval-workflow.md)
