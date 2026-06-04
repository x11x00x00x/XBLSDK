# Anti-cheat: how scores are verified (and what this can't do)

The original Xbox runs fully open homebrew. Anything shipped on the disc — including
an embedded secret — can eventually be extracted, and the public REST API can be
called directly with `curl`. So the design does **not** "trust the score." It
requires the client to **prove** the score is the product of real gameplay, and
makes the cheap attacks fail. This document is deliberately honest about where the
guarantees end.

## The core idea: prove, don't trust (deterministic replay)

The whole game is a **pure, deterministic simulation**: given a `seed` and the
exact sequence of inputs, it always produces the same outcome and the same score.
The simulation has two byte-identical implementations:

- `sim/dodge_sim.c` — runs on the console (and is the source of truth).
- `homebrew/dodge-sim.js` — runs on the xb.live server.

`sim/test-vectors.json` pins agreed `(seed, inputs) → score` cases that both
implementations must reproduce, so they can never silently drift apart.

When a score is submitted, the client sends the run's **seed** and a compact
**input log**. The server **re-runs the same simulation** and accepts the score
only if the recomputed value matches the claim. A fabricated number with no valid
input log fails. To pass, you must supply inputs that genuinely produce that score
under the real rules — i.e. you must actually (or programmatically) play the game.

## The layers

1. **App identity — HMAC-SHA256.**
   Each approved game has a `game_secret`. Every `run/start` and `score` request is
   signed with it. Requests without a valid signature are rejected, so a casual
   `curl` against the API (without the secret) cannot submit anything.

2. **Server-issued, single-use run nonce + server-chosen seed.**
   A score must cite a `run_id`/`nonce` returned by `run/start`. The nonce is
   single-use (atomically consumed), short-lived, and bound to the requesting user.
   This blocks replays, duplicate submissions, and pre-baked runs. Crucially the
   **seed is chosen by the server** at `run/start`, so a player cannot grind seeds
   offline to find a lucky one and then submit it.

3. **Deterministic replay (the real gate).**
   The server recomputes the score from `seed` + `input_log`. Mismatch ⇒ reject.
   This is what actually ties a score to real play. It catches forged scores **even
   when the attacker possesses the game secret and signs the request correctly.**
   (The integration test `homebrew/test-hb.js` demonstrates exactly this case.)

4. **Plausibility, timing & rate limits.**
   - Score must be in range; the input log must be well-formed (ascending frames in
     range, valid states, bounded length).
   - The wall-clock time between `run/start` and `score` must be at least roughly
     half of the simulated duration — you can't "play" a 3-minute run in 2 seconds.
   - Runs per minute per user/game are capped.

5. **Verified-only ranking + moderation.**
   Only replay-verified scores are ranked on the public board (a "Verified" badge).
   Admins can review and delete games/boards/scores.

## Achievements ride the same proof

Achievements are not a separate trust surface. When the game reports a run to
`POST /api/hb/achievements`, the server **re-derives** which achievements it
earned from the *same* deterministic replay (run-derived: e.g. survived N
seconds, dodged N blocks) or from its *own* account counters (cumulative: e.g.
number of verified runs). The client's claimed list is ignored. So a bare forged
"I unlocked X" request unlocks nothing, exactly like a forged score is rejected —
the unlock has the same guarantees (and the same limitations) as the score. The
client only evaluates conditions locally to pop the on-screen toast instantly;
the grant of record is always the server's. See [achievements.md](achievements.md).

## Multiplayer rides the same proof

Multiplayer doesn't weaken any of this. A lobby round issues each player a normal
server-tracked run bound to the **shared seed** and the lobby's **map**, and the
round **finish** goes through the *identical* replay-verification pipeline as a
single-player score (it is even signed the same way, with board id `mp`). Live
progress (`/progress`) is cosmetic — it only drives the on-screen standings and
is never used to award anything. Final placement is computed from the
replay-verified final scores, so a player cannot win a match by forging a finish
or by lying in heartbeats. See [multiplayer.md](multiplayer.md).

## Threat model — what each attack hits

| Attack | Outcome |
|--------|---------|
| `curl` a made-up score, no secret | Rejected (no/invalid HMAC). |
| Replay a previously captured valid submission | Rejected (nonce already consumed / expired). |
| Submit a huge score with the real secret but no/garbage input log | Rejected (replay doesn't reproduce it). |
| Pick a favourable seed offline, then submit | Can't — the server chooses the seed at `run/start`. |
| Submit instantly after `run/start` for a long run | Rejected (timing implausible). |
| Flood submissions | Rate-limited. |

## Honest limitations

No purely client-side scheme is unbreakable on an open, offline console:

- **Secret extraction.** A determined attacker can dump the XBE and recover the
  embedded `game_secret`. Then they can sign requests. They still cannot submit a
  score that fails replay — but they *can* run the deterministic core themselves.
- **Bot / synthesized input logs.** Because the simulation is deterministic and the
  attacker can run it, they can search for an input sequence that yields a high
  score for the server-issued seed and submit that valid log. This is exactly a
  very good (or superhuman) bot. Replay verification confirms the inputs *do*
  produce the score; it cannot prove a human pressed the buttons.
- **Timing only bounds the lower edge.** It stops instant submissions, not an
  attacker who waits.

What this design **does** achieve: it defeats trivial API forgery, makes cheating
require real reverse-engineering effort, keeps every ranked score backed by an
input log that reproduces it, and leaves cheating **detectable and removable**.

### Why not something stronger?

Truly stronger options are infeasible here:

- **Fully server-authoritative gameplay** (server simulates while you play) needs a
  constant low-latency connection and a server tick per player — impractical for
  casual OG-Xbox homebrew, and the console would still send the inputs.
- **Hardware attestation / secure enclave** does not exist on the original Xbox.

Mitigations that *do* help and are supported or easy to add: rotating per-game
secrets, anomaly flagging (e.g. statistical outliers, superhuman input regularity),
trust tiers, and manual moderation. The `verify_mode` field also lets a game opt
into `replay` (full verification), `hmac` (signature only — scores land unverified),
or `none`.
