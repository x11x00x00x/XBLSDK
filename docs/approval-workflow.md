# Approval workflow — get your homebrew game on the leaderboards

A game can only submit scores **after it's approved** and has been issued a signing
secret. This is the "linked to upon approval" flow.

## 1. Register the game (developer)

Signed in with your Insignia account, call:

```
POST /api/hb/dev/games
X-Session-Key: <your session key>
{ "game_id": "cool-shmup", "name": "Cool Shmup",
  "description": "A bullet-hell scorer.", "verify_mode": "replay" }
```

- `game_id`: 3–40 chars, lowercase `a–z`, `0–9`, and dashes. This is permanent.
- `verify_mode`:
  - `replay` (recommended) — scores are verified by server-side re-simulation.
    Requires a server verifier (see step 4).
  - `hmac` — only the HMAC signature is checked; scores are stored **unverified**
    and are not ranked on the public board by default.
  - `none` — no app signature required (open submission; unverified).

The game starts **pending**.

## 2. Approve + receive the secret (admin)

An admin opens **Admin → Homebrew leaderboards** (`/admin-homebrew`), finds the
pending game, and clicks **Approve & issue secret**. The server generates a random
`game_secret`, stores it **encrypted at rest** (AES-256-GCM), and shows the
plaintext **once**.

> Copy the secret immediately — it is never shown again. Only the last 4 characters
> remain visible afterward. Re-approving issues a new secret.

A default `highscore` board is auto-created if the game has none. Admins can add
more boards (descending or ascending sort, custom unit) from the same page.

(Equivalent API: `POST /api/hb/admin/games/:id/approve` → `{ game_secret }`.)

## 3. Bake the secret into your game

Embed `game_id` and `game_secret` in your build. For the SDK, set them on
`XblConfig`:

```c
cfg.game_id = "cool-shmup";
cfg.game_secret = "<the issued secret>";
```

Understand the trade-off in [anti-cheat.md](anti-cheat.md): an embedded secret can
be extracted from an open console. Replay verification — not the secret — is what
keeps fabricated scores off the board.

## 4. (replay mode) Provide a server verifier

For `verify_mode: "replay"`, the server must be able to re-simulate your game. Add a
JS module mirroring your deterministic core (like `homebrew/dodge-sim.js`) and
register it in `homebrew/hb-routes.js`:

```js
const SIMS = {
    'og-testgame': require('./dodge-sim'),
    'cool-shmup':  require('./cool-shmup-sim'),   // exports replay(seed, events), parseInputLog, validateInputLog, SIM_VERSION, TICK_RATE
};
```

Your verifier must reproduce scores byte-for-byte from your console core. Pin the
agreement with test vectors (see `sim/test-vectors.json` and `sim/verify_vectors.js`
for the pattern). Set the game's `sim_version` to match.

Until a verifier is registered, `replay`-mode score submissions return `501`.

## 5. Ship

Players sign in on-console, play, and submit. Verified scores appear on
`/homebrew?game=cool-shmup` and in each player's dashboard **Homebrew** tab.
