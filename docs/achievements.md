# Achievements (verifiable, replay-derived)

Achievements use the **same trust model as scores**: *prove, don't trust.* The
client never gets to tell the server "I earned achievement X" and have it
believed. Instead the server **re-derives** which achievements a run earned by
re-simulating that run (run-derived) or by reading its own account counters
(cumulative). A bare forged API call unlocks nothing.

This document covers the design, the verification, the Test Game's catalog, and
how to add achievements to your own game.

---

## Two kinds of achievement

| Kind | Earned from | Verified because… |
|------|-------------|-------------------|
| **run** | a single run's deterministic summary `{ score, obstaclesPassed }` | the server recomputes that summary by replaying the submitted `seed` + `input_log`. The same replay already verifies the score, so there is nothing extra to trust. |
| **cumulative** | server-owned account counters (e.g. number of verified runs) | the server owns those numbers outright; the client never supplies them. |

The deterministic run summary is produced identically by the C client
(`dodgeReplayStats` in `sim/dodge_sim.c`) and the JS server
(`replayStats` in `homebrew/dodge-sim.js`). The lockstep is pinned by
`sim/test-vectors.json` (now including `obstaclesPassed`) and checked by
`sim/verify_vectors.js`.

---

## End-to-end flow

```
                 ┌─────────────────────── on the console ───────────────────────┐
   play a run →  evaluate conditions live → xbl_ach_award(id) → toast at bottom
                 └───────────────────────────────────────────────────────────────┘
                                          │ (run ends, score submitted)
                                          ▼
   xbl_ach_report(run, seed, input_log)  ──HMAC──►  POST /api/hb/achievements
                                                         │
                                                         ▼
                          server RE-SIMULATES seed+input_log → { score, obstaclesPassed }
                          ├─ evaluateRun(stats)              → earned run-derived ids
                          ├─ evaluateCumulative(counters)    → earned cumulative ids
                          └─ grant (idempotent) → return the NEWLY unlocked set
                                                         │
                                                         ▼
                          client toasts any newly-unlocked ones it hadn't shown
                          (this is how cumulative achievements get their toast)
```

**Why the toast fires before the server confirms.** The client evaluates the
exact same conditions during play, so it can pop the toast the instant you cross
a threshold — that is purely cosmetic, instant feedback. The *grant of record*
only happens server-side after the run is replayed. If the two ever disagreed
(e.g. a hacked client), the server's decision wins and only it affects your
account.

**Why we report once at end-of-run, not per-achievement mid-run.** A run can
only be verified once it's complete (the server needs the whole input log to
replay it). So the client toasts immediately for feel, then sends a single signed
report when the run ends.

---

## The request

`POST /api/hb/achievements` (requires `X-Session-Key`):

```json
{
  "game_id":   "og-testgame",
  "run_id":    "<the run_id from /api/hb/run/start>",
  "seed":      123456789,
  "input_hash":"<sha256 hex of input_log>",
  "sig":       "<hmac, see below>",
  "input_log": "0:0,40:2,95:1,..."
}
```

Signature (lowercase-hex HMAC-SHA256 with the game secret):

```
sig = HMAC_SHA256(game_secret, "ach|" + game_id + "|" + run_id + "|" + seed + "|" + input_hash)
```

Server checks, in order:

1. **Session** → resolves the player.
2. **HMAC + `input_hash`** → proves the request came from the real game binary
   and the input log is intact (Layer 1, identical to score submit).
3. **Run binding** → the `run_id` must be a real, server-issued run *for this
   player*, and `seed` must match the run's issued seed. (Unlike score submit,
   the run may already be *consumed* — achievements are reported right after the
   score, and granting is idempotent.)
4. **Replay** → `replayStats(seed, parse(input_log))` recovers `{ score,
   obstaclesPassed }`. The client's claimed list is **never read.**
5. **Derive + grant** → `evaluateRun(stats)` ∪ `evaluateCumulative({verifiedRuns})`,
   filtered to the catalog, upserted idempotently.

Response:

```json
{
  "success": true,
  "stats": { "score": 1653, "obstaclesPassed": 42 },
  "unlocked": [ { "id":"survivor_10s", "name":"Getting the Hang of It",
                  "description":"Survive 10 seconds in a single run.", "kind":"run" } ],
  "all": [ "survivor_10s", "survivor_20s", "dodger_25", "first_run" ]
}
```

`unlocked` = achievements newly granted by *this* call (the client toasts these).
`all` = the player's full unlocked set for the game.

Other read endpoints:

- `GET /api/hb/games/:gameId/achievements` — catalog; includes `unlocked` per
  entry when `X-Session-Key` is supplied, plus a global `unlocked_count`.
- `GET /api/me/hb-achievements` — the caller's unlocks across all games (dashboard).

---

## Test Game catalog

Defined in **both** `game/game_achievements.c` (client) and
`homebrew/testgame-achievements.js` (server, authoritative). Ids/thresholds must
match.

| id | name | condition | kind |
|----|------|-----------|------|
| `survivor_10s`    | Getting the Hang of It | survive 10 s (600 frames) | run |
| `survivor_20s`    | Survivor               | survive 20 s (1200 frames) | run |
| `untouchable_30s` | Untouchable            | survive 30 s (1800 frames) | run |
| `dodger_25`       | Crowd Control          | dodge 25 blocks in a run | run |
| `dodger_75`       | Block Party            | dodge 75 blocks in a run | run |
| `first_run`       | Welcome to the Board   | submit your first verified score | cumulative |
| `persistent_10`   | Dedicated              | submit 10 verified runs | cumulative |

A block counts as **dodged** when it falls off the bottom of the field without
hitting you — the simulation counts this in `obstaclesPassed`, so it's derived
purely from the replay.

### Gamerscore on xb.live

On the website, the Test Game is surfaced exactly like a retail Insignia title:
it has a `games` row (synthetic title id **`fe000001`**), its leaderboard and
online count, and these achievements appear on the site-wide **Achievements** page
with a Gamerscore. A small bridge (`syncTestGameToRetail` in `database.js`) mirrors
each verified homebrew unlock into the retail `user_achievements` table, so a
player's site Gamerscore and profile reflect Test Game progress. The mapping
(also pinned in `database.js` → `TESTGAME_ACH_MAP`) is:

| id | retail achievement id | Gamerscore |
|----|-----------------------|-----------:|
| `first_run`       | 9001 | 10G |
| `survivor_10s`    | 9002 | 15G |
| `dodger_25`       | 9003 | 20G |
| `survivor_20s`    | 9004 | 30G |
| `persistent_10`   | 9005 | 35G |
| `dodger_75`       | 9006 | 40G |
| `untouchable_30s` | 9007 | 50G |

Total: **200G**. Because the bridge only mirrors unlocks the server already
granted by replay, Gamerscore is just as unforgeable as the underlying
achievements — an API call alone earns nothing.

---

## SDK surface (`sdk/xblsdk.h`)

```c
typedef struct { const char *id, *name, *description; } XblAchievementDef;
typedef void (*XblAchievementToast)(const XblAchievementDef *def, void *ud);

int  xbl_ach_init(const XblAchievementDef *defs, int count, XblAchievementToast on_toast, void *ud);
int  xbl_ach_sync(const XblSession *s);    // after sign-in: seed already-unlocked (silent)
void xbl_ach_reset(void);                  // ONLY for account switch (then re-sync)
void xbl_ach_award(const char *id);        // earned → toast once (skips already-earned)
int  xbl_ach_was_awarded(const char *id);
int  xbl_ach_report(const XblSession *s, const XblRun *run, long long score,
                    uint32_t seed, const char *input_log, char *msg, size_t msgsz);
```

The SDK is generic plumbing; **your game owns the catalog and the conditions**
(they are game-specific). The SDK handles: the toast dedupe, the HMAC-signed
report, and toasting any server-confirmed (cumulative) unlocks from the response.

**Don't re-pop earned achievements.** The awarded set is seeded once right after
sign-in via `xbl_ach_sync()` (from the server's unlocked list) and persists for
the whole session — `xbl_ach_report()` also keeps it in sync from the response's
`all` array. Never call `xbl_ach_reset()` between runs, or achievements earned on
a previous run/launch will toast again.

---

## Adding achievements to your own game

1. **Decide the conditions.** Run-derived conditions must be a pure function of
   your run's deterministic summary (whatever stats your sim exposes). Cumulative
   conditions must be a pure function of server counters.
2. **Server registry.** Add a module like `homebrew/<yourgame>-achievements.js`
   exporting `CATALOG`, `evaluateRun(stats)`, and `evaluateCumulative(counts)`,
   and register it in `homebrew/achievements.js` under your `game_id`. Make sure
   your game also has a replay verifier registered in `homebrew/hb-routes.js`
   (`SIMS`) so `stats` can be recomputed.
3. **Client catalog.** Mirror the ids/names/descriptions in C, register them with
   `xbl_ach_init`, then call `xbl_ach_sync(&session)` once right after sign-in so
   already-unlocked achievements don't re-toast. `xbl_ach_award(id)` the moment a
   condition is met during play. Call `xbl_ach_report(...)` after
   `xbl_score_submit(...)` succeeds (while the input log is still alive).
4. **Keep them in lockstep.** The ids and thresholds in the C catalog must match
   the JS registry. If the threshold depends on a new stat, expose that stat in
   *both* sim cores and pin it in `sim/test-vectors.json`.

---

## Limitations (honest)

- Achievements are only as trustworthy as the run that produces them. The
  guarantees and residual risks are exactly those of score verification — see
  [anti-cheat.md](anti-cheat.md). In particular, a game whose `verify_mode` is
  not `replay` has no run summary to derive from, so it gets **no run-derived
  achievements** (cumulative ones still work).
- Cumulative counters are deliberately simple (e.g. verified-run count). They are
  trustworthy because the server owns them, but they only measure what the server
  can see (verified submissions), not raw play time on the console.
- A modified client can suppress or fake *its own* toasts. That only changes what
  that player sees locally; it never changes what is granted, because the server
  re-derives every unlock.
