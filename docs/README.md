# xb.live Homebrew Leaderboards — Documentation

This repository contains the **Test Game** (an original-Xbox "block-dodger"), the
reusable **xb.live Homebrew SDK** it is built on, the shared **deterministic
simulation core**, and the server-side pieces that make scores **verifiable**.

It lets any approved homebrew game submit player scores to xb.live, tied to a
player's Insignia account, and have those scores ranked on public leaderboards —
while making it hard to forge scores through the API.

## Documents

| Doc | What it covers |
|-----|----------------|
| [anti-cheat.md](anti-cheat.md) | **Read this first.** How scores are verified, the threat model, and honest limitations. |
| [single-player.md](single-player.md) | Single-player modes: Survival, Sprint, and the new **3D Mode** (a forward-runner with its own deterministic sim + leaderboards). |
| [story-mode.md](story-mode.md) | Single-player **campaign**: 10 escalating levels (two acts) with a story, climbing frame goals, per-level unlocks, and per-map leaderboards. |
| [achievements.md](achievements.md) | Replay-derived, verifiable achievements (catalog, API, how to add your own). |
| [multiplayer.md](multiplayer.md) | Online multiplayer (up to 32 players): lobbies, maps, **game modes** (survival/battle/sprint), shared-seed rounds, live opponent ghosts + standings, website spectator. |
| [saves.md](saves.md) | Native-format Xbox game saves (`E:\UDATA` / `SaveMeta.xbx`) via the SDK. |
| [presence.md](presence.md) | Online presence: heartbeat API + how the site/game show who's online. |
| [friends.md](friends.md) | Friends list (with live homebrew presence) + lobby **invites** (accept in-game or on the web). |
| [rest-api.md](rest-api.md) | Language-agnostic REST + HMAC spec for `/api/hb/*` (integrate from any toolchain). |
| [sdk-reference.md](sdk-reference.md) | The C/nxdk SDK API (`xblsdk.h`). |
| [integration-quickstart.md](integration-quickstart.md) | Build the Test Game and stand up the loop end to end. |
| [approval-workflow.md](approval-workflow.md) | Register a game, get approved, receive your signing secret. |
| [../sim/sim-spec.md](../sim/sim-spec.md) | The deterministic simulation contract (the lockstep rules). |

## Repository layout

```
testgame/
  sdk/            xblsdk: login + session + run + HMAC score submit + leaderboard +
                  achievements + multiplayer lobbies (xblsdk.c) + Xbox saves (xbl_save.c)
  game/           Test Game nxdk app (block-dodger): menu, story mode, maps, multiplayer, saves
  sim/            dodge_sim.c/.h 2D deterministic core (+ maps) + sim-spec.md + test-vectors.json
                  dodge3d_sim.c/.h 3D-Mode forward-runner sim + sim3d-spec.md
  third_party/    https_client (lwIP + mbed TLS), qrcodegen, mbed TLS shims
  lwip_override/  lwipopts shim (keeps lwIP debug off the framebuffer)
  docs/           this folder
  Makefile, README.md
```

The server side lives in the xb.live (`insignia stats`) repo under `homebrew/`:

```
homebrew/
  dodge-sim.js              JS mirror of sim/dodge_sim.c (incl. maps) — re-simulates runs to verify
  dodge3d-sim.js            JS mirror of sim/dodge3d_sim.c — verifies 3D-Mode (threed-*) scores
  hb-db.js                  schema + data access (games/leaderboards/runs/scores/stats/achievements/lobbies)
  hb-crypto.js              HMAC/SHA-256 + secret encryption at rest
  hb-routes.js              the /api/hb/* Express routes incl. /api/hb/mp/* lobbies (from server.js)
  achievements.js           per-game achievement registry (game_id → catalog + evaluators)
  testgame-achievements.js  the Test Game's catalog + run/cumulative evaluators
  test-hb.js                integration test (submit loop + anti-cheat + achievements + multiplayer)
```

## The 10-second mental model

1. Player signs in on the console via QR (Insignia device flow) → session key.
2. Game calls `POST /api/hb/run/start` (HMAC-signed) → gets a **server-chosen seed**
   and a single-use **nonce**.
3. Game plays that seed with the shared deterministic engine, recording every input.
4. Game calls `POST /api/hb/score` with the score + seed + input log (HMAC-signed).
5. Server **re-runs the identical simulation** and accepts the score only if it
   matches. Verified scores rank on the public board.
6. Game reports the run to `POST /api/hb/achievements`; the server **re-derives**
   which achievements the run earned (from the same replay) and grants them. See
   [achievements.md](achievements.md).

For **multiplayer**, a host opens a lobby (`/api/hb/mp/lobby/create`); up to 32
players join, pick a **map**, and the host starts a round. The server issues one
**shared seed** + a per-player verifiable run; everyone plays the same falling
blocks, pushes live progress, and submits a **replay-verified** final score — so
the leaderboard-grade anti-cheat applies to multiplayer too. See
[multiplayer.md](multiplayer.md). Progress (best score, last map) can be stored in
the **native Xbox save format** so it shows up in the dashboard — see
[saves.md](saves.md).
