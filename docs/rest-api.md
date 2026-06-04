# Homebrew Leaderboard REST API (`/api/hb/*`)

Language-agnostic contract so homebrew on **any** toolchain (not just nxdk) can
integrate after approval. Base URL: `https://xb.live`. Auth for the device/login
flow is the existing Insignia auth service at `https://auth.insigniastats.live`.

All request/response bodies are JSON. The player's Insignia session key is sent in
the `X-Session-Key` header (obtained via the Insignia device-login flow; the SDK
does this for you).

## Signing (HMAC-SHA256)

Approved games get a `game_secret` (see [approval-workflow.md](approval-workflow.md)).
Signatures are lowercase-hex HMAC-SHA256 over a canonical string using that secret
as the key.

```
run/start:
    sig = HMAC_SHA256(game_secret, "start|" + game_id)

score:
    input_hash = SHA256_hex(input_log)        # hash of the exact input_log string
    msg = "score|" + game_id + "|" + run_id + "|" + nonce + "|"
        + board_id + "|" + score + "|" + seed + "|" + input_hash
    sig = HMAC_SHA256(game_secret, msg)

achievements:
    input_hash = SHA256_hex(input_log)
    msg = "ach|" + game_id + "|" + run_id + "|" + seed + "|" + input_hash
    sig = HMAC_SHA256(game_secret, msg)

multiplayer lobby (create / join / leave):
    sig = HMAC_SHA256(game_secret, "mp|" + game_id)

multiplayer round finish (same shape as score, board_id is the literal "mp"):
    input_hash = SHA256_hex(input_log)
    msg = "score|" + game_id + "|" + run_id + "|" + nonce + "|mp|"
        + score + "|" + seed + "|" + input_hash
    sig = HMAC_SHA256(game_secret, msg)
```

`score` and `seed` are formatted as plain base-10 integers in the message.

## Input log format

A sparse, ascending list of `frame:state` changes joined by commas, e.g.
`"0:0,40:2,95:1,210:0"`. `state` is a game-defined bitmask (for the dodger:
`1`=left, `2`=right). The state in effect at frame *f* is the latest entry with
`frame ≤ f` (default `0`). Rules (validated server-side):

- frames strictly ascending, each in `[0, MAX_FRAMES]`,
- states in the game's allowed range (`0..3` for the dodger),
- at most `MAX_FRAMES + 1` entries.

See [../sim/sim-spec.md](../sim/sim-spec.md) for the exact simulation the server
replays.

---

## Endpoints

### `POST /api/hb/run/start`
Open a server-tracked run. **Headers:** `X-Session-Key`. **Body:**
```json
{ "game_id": "og-testgame", "map_id": 0, "sig": "<hmac hex>" }
```
`map_id` is optional (default `0`; clamped to a valid map). **200:**
```json
{ "success": true, "run_id": "<hex>", "nonce": "<hex>",
  "seed": 123456789, "map_id": 0, "server_time": 1733250000, "ttl": 1800 }
```
You **must** play the returned `seed` on the returned `map_id`. The `nonce` is
single-use and expires after `ttl` seconds. Score verification replays the run on
the run's recorded map.

Errors: `401` invalid session / bad signature, `404` game not approved,
`429` too many runs.

### `POST /api/hb/score`
Submit a score for verification. **Headers:** `X-Session-Key`. **Body:**
```json
{
  "game_id": "og-testgame",
  "run_id": "<hex>",
  "nonce": "<hex>",
  "board_id": "survival-0",
  "score": 1234,
  "seed": 123456789,
  "input_hash": "<sha256 hex of input_log>",
  "sig": "<hmac hex>",
  "input_log": "0:0,40:2,95:1"
}
```

**Leaderboards are split per game mode and map.** Board ids follow the scheme
`"<mode>-<mapId>"` — e.g. `survival-0` (Classic), `survival-3` (Blizzard),
`battle-2`, `sprint-4`, `threed-1` (3D Mode), `shooter-0` (Shooter Test).
Single-player runs post to a solo mode's board for their own map —
`survival-<mapId>`, `sprint-<mapId>`, `threed-<mapId>` or `shooter-<mapId>` where
`<mapId>` equals the run's map (Shooter Test has a single map, so only
`shooter-0`). A board for a different map, or a `battle-*`/`livebattle-*` board
(those modes are inherently multiplayer and are written server-side by the lobby
finish handlers, never by a `/score` call), is rejected with `400`. (The legacy
combined `highscore` board still accepts scores but is no longer used by the
game.)

Most boards are ranked by **frames** (longer = better); Shooter Test's
`shooter-0` is ranked by **points** (higher = better). The `score` field carries
whichever the board uses.

**Verifier selection.** A run is sim-neutral on the wire (`run/start` records only
`seed + map_id`); the **board id selects how it is verified**. `threed-*` scores
are replayed with the Test Game's separate 3D-Mode sim
(`homebrew/dodge3d-sim.js`) and all `survival/sprint/battle/livebattle` boards use
the 2D dodger (`homebrew/dodge-sim.js`). **`shooter-*` is the exception:** the
free-roam FPS isn't deterministically replayable, so it is **trust-based** — the
server applies plausibility caps (max score + minimum wall-clock for the fixed
60 s round) and stores the score **unverified** (it still ranks/displays). See
[single-player.md](single-player.md) and [`../sim/shooter-spec.md`](../sim/shooter-spec.md).
**200 (accepted):**
```json
{ "success": true, "accepted": true, "verified": true, "improved": true,
  "score": 1234, "rank": 1, "message": "Verified! New best: 1234" }
```
Rejections: `400` nonce/seed/hash mismatch or malformed log, `401` bad
signature/session, `403` run belongs to another user, `409` run already submitted,
`410` run expired, `422` **score failed replay verification** or implausible timing,
`501` no server verifier registered for a `replay`-mode game.

### `POST /api/hb/achievements`
Report a finished run so the server can **re-derive and grant** achievements (it
never trusts a client-supplied list). **Headers:** `X-Session-Key`. **Body:**
```json
{ "game_id":"og-testgame", "run_id":"<hex>", "seed":123456789,
  "input_hash":"<sha256 hex of input_log>", "sig":"<hmac hex>",
  "input_log":"0:0,40:2,95:1" }
```
**200:**
```json
{ "success": true,
  "stats": { "score": 1653, "obstaclesPassed": 42 },
  "unlocked": [ { "id":"survivor_10s", "name":"Getting the Hang of It",
                  "description":"Survive 10 seconds in a single run.", "kind":"run" } ],
  "all": [ "survivor_10s", "survivor_20s", "dodger_25", "first_run" ] }
```
The server replays `seed`+`input_log`, evaluates run-derived achievements from the
recomputed `{score,obstaclesPassed}` and cumulative ones from its own counters,
and grants idempotently. `unlocked` is the newly-granted set (toast these). Errors:
`400` hash/seed mismatch or malformed log, `401` bad signature/session, `403` run
belongs to another user. Full design: [achievements.md](achievements.md).

### `GET /api/hb/games/:gameId/achievements`
A game's achievement catalog. With `X-Session-Key`, each entry also reports
whether the caller has `unlocked` it (plus a global `unlocked_count`).
```json
{ "game_id":"og-testgame","game_name":"Test Game",
  "achievements":[ { "id":"survivor_10s","name":"Getting the Hang of It",
                     "description":"Survive 10 seconds in a single run.",
                     "kind":"run","unlocked":true,"unlocked_count":3 } ] }
```

### `GET /api/me/hb-achievements`
The signed-in player's unlocked achievements across all games (header
`X-Session-Key`): `{ "success": true, "achievements": [ { game_id, game_name,
achievement_id, name, description, kind, unlocked_at } ] }`.

### `GET /api/hb/games/:gameId/maps`
A game's deterministic map catalog (public):
```json
{ "game_id":"og-testgame","maps":[ {"map_id":0,"name":"Classic"},
  {"map_id":1,"name":"Rush"}, {"map_id":2,"name":"Wideload"},
  {"map_id":3,"name":"Blizzard"}, {"map_id":4,"name":"Zen"} ] }
```

---

## Online presence (`/api/hb/presence`)

A lightweight heartbeat so the site (and the game) can show who/how many players
are currently in a game. A player counts as **online** while their last heartbeat
is within the presence window (`window_sec`, currently 90s). Stale rows are purged
server-side. See [presence.md](presence.md).

### `POST /api/hb/presence`
Heartbeat. Call every ~30s while the game is open and the user is signed in.
**Headers:** `X-Session-Key`. **Body:**
`{ "game_id":"og-testgame", "status":"online", "instance":"<token>", "claim":false, "sig":"<presence sig>" }`
where `sig = hmac_sha256_hex(secret, "presence|" + game_id)` and `status` is one of
`online` (in menus), `playing` (in a run) or `lobby` (in a multiplayer room) —
unknown values are treated as `online`. **200:**
`{ "success":true, "superseded":false, "online_count":3, "window_sec":90 }`.
Errors: `401` bad sig/session, `404` game not approved.

**Single online session.** A player may be signed in on several consoles/games at
once (the session key is shared — see [sdk-reference.md](sdk-reference.md) → the
shared sign-in store), but only **one** can be *online* at a time. `instance` is a
per-launch token identifying this run:

- `claim:true` — this launch is going online (e.g. at startup, or deliberately
  taking over). It becomes the online session for the account and any other place
  is backed out. If the previous owner was online in a different game, it's removed
  from that game's count immediately.
- `claim:false` — a periodic refresh. If a newer instance has taken over,
  the response has `"superseded":true` and this client should stop online play
  (the Test Game shows "Signed in elsewhere"). Otherwise it refreshes ownership.

Clients without an `instance` keep the legacy per-game behavior (no enforcement).

> The signature + session requirement means a client can only mark **itself**
> online, never another user. `run/start`, `mp/progress` and `mp/finish` also
> refresh presence, so a player mid-game stays online without a separate ping.

### `GET /api/hb/games/:gameId/presence`
Who is online for a game (public; usernames are already public on the board):
```json
{ "game_id":"og-testgame","window_sec":90,"online_count":2,
  "players":[ {"name":"alice","status":"playing","idle_sec":3},
              {"name":"bob","status":"online","idle_sec":31} ] }
```
`players` is capped at 100, most-recent heartbeat first.

---

## Friends & invites (`/api/hb/friends`, `/api/hb/invites`)

Friends mirror the caller's Insignia friends (read-only) annotated with live
homebrew presence; invites point a friend at a lobby. See [friends.md](friends.md).

### `GET /api/hb/friends?game_id=`
Session required (`X-Session-Key`). Returns the caller's **full** Insignia friends
list (online and offline), most-actionable first (in this game → online in
homebrew → online on Insignia → offline). The live `/auth/friends` list is fetched
and persisted to a stored roster so the full list is returned even if the auth API
is briefly down. `online_any` unifies homebrew and Insignia presence into one
"online", and `online_count` is how many friends are online by that measure:
```json
{ "game_id":"og-testgame","count":3,"online_count":2,
  "friends":[
    {"name":"bob","invite_to":"bob","online":true,"hb_status":"lobby",
     "hb_game_id":"og-testgame","in_this_game":true,"lobby_code":"ABCD12",
     "xbox_online":true,"xbox_game":null,"playing":"Test Game","online_any":true},
    {"name":"dave","invite_to":"","online":false,"in_this_game":false,
     "lobby_code":"","xbox_online":true,"xbox_game":"Halo 2","playing":"Halo 2","online_any":true},
    {"name":"carol","invite_to":"","online":false,"in_this_game":false,
     "lobby_code":"","xbox_online":false,"xbox_game":null,"playing":"","online_any":false} ] }
```
`invite_to` is the account name to address an invite to (set only when the friend
is online in homebrew). `lobby_code` is a joinable lobby of *this* game, if any.
`playing` is the human-readable game the friend is in right now (the homebrew
game's name when in one, else the Insignia game; `""` when offline). `online_any`
is the field to drive a single online indicator — Insignia and homebrew presence
are treated as one and the same.

### `POST /api/hb/invites/send`
Session + app signature `sig = hmac_sha256_hex(secret, "invite|" + game_id)`.
Body `{ "game_id","to","lobby_code","sig" }`. You must be a member of `lobby_code`
and may not invite yourself. Returns `{ "success":true, "invite":{ "id",...} }`.

### `GET /api/hb/invites?game_id=`
Session required. Your live invites (pending or accepted-elsewhere, within a
5-minute TTL); invites to a disbanded lobby are dropped:
```json
{ "game_id":"og-testgame","count":1,
  "invites":[ {"id":7,"from":"alice","lobby_code":"ABCD12","status":"pending",
    "age_sec":4,"map_id":0,"map_name":"Classic","mode":"survival","host":"alice",
    "player_count":1,"max_players":32} ] }
```

### `POST /api/hb/invites/:id/accept`
**Session only** (no app secret — so the website can accept too). Marks the invite
accepted and returns the lobby to join:
`{ "success":true, "lobby_code":"ABCD12", "lobby":{...} }`. A `410` means the lobby
is gone. The client then `POST /api/hb/mp/lobby/:code/join`s as usual.

### `POST /api/hb/invites/:id/decline`
**Session only.** Dismisses the invite. `{ "success":true }`.

---

## Multiplayer lobbies (`/api/hb/mp/*`)

Shared-seed competitive rooms for up to 32 players. See
[multiplayer.md](multiplayer.md) for the model and **game modes**. Lobby objects
look like:
```json
{ "code":"K7P2QM","game_id":"og-testgame","host":"alice","name":"alice's lobby",
  "map_id":0,"map_name":"Classic","mode":"battle","target":0,
  "max_players":32,"status":"playing","seed":123,"round_started_at":1700000000,
  "player_count":2,"alive_count":1,"winner":null,
  "players":[ {"name":"alice","score":120,"frame":120,"px":148,"alive":true,
               "finished":false,"verified":false} ] }
```
`status` is `waiting` → `playing` → `finished`. `seed` is non-null once a round
starts. `mode` is `survival` | `battle` | `sprint`; `target` is the sprint
finish-line frame (`0` otherwise); `alive_count` is players still running;
`winner` is filled once `finished`. Each player carries `px` (live x, for
opponent ghosts) and, in sprint, `cleared` (passed the finish line). `players`
is sorted by score. `run_id`/`nonce` are **per-member secrets** and are never in
the public lobby object.

### `POST /api/hb/mp/lobby/create`
Open a lobby (you become host). **Headers:** `X-Session-Key`. **Body:**
`{ "game_id":"og-testgame", "map_id":0, "mode":"survival", "name":"optional", "sig":"<mp sig>" }`.
`mode` is optional (`survival` | `battle` | `sprint`; unknown → `survival`).
**200:** `{ "success":true, "lobby": { ... } }`. Errors: `401` bad sig/session,
`404` game not approved.

### `GET /api/hb/mp/lobbies?game_id=`
List lobbies (public). **Query:** `status` = `waiting` (default, joinable),
`playing` (spectatable), or `all` (both — used by the website's live-match list):
`{ "game_id":"...","lobbies":[ {code,host,name,map_id,map_name,mode,player_count,max_players,status} ] }`.

### `GET /api/hb/mp/lobby/:code`
Poll a lobby's full state + live standings (public): `{ "success":true, "lobby": {...} }`.
`404` if not found.

### `POST /api/hb/mp/lobby/:code/join`
Join. **Headers:** `X-Session-Key`. **Body:** `{ "sig":"<mp sig>" }`. **200:**
`{ "success":true, "lobby": {...}, "spectating":<bool> }`. Joining is allowed even
while a round is in progress: the new member sits that round out as a spectator
(`spectating:true`, not counted alive or toward round completion) and is dealt in
on the next round. If the lobby is `friends_only`, only the host's friends and
invited players may join. Errors: `401` bad sig, `403` not allowed (friends-only),
`409` lobby full.

### `POST /api/hb/mp/lobby/:code/leave`
Leave. **Body:** `{ "sig":"<mp sig>" }`. The leaving member is removed from the
roster. If the **host** leaves and other members remain, the host role migrates
to the longest-present remaining member and the response is
`{ "success":true, "host":"<new host>" }`. The lobby is only disbanded when the
**last** member leaves (`{ "success":true, "disbanded":true }`). Clients should
poll `GET /api/hb/mp/lobby/:code`; a `404` means the lobby is gone.

### `POST /api/hb/mp/lobby/:code/return`
Re-arm a played-through lobby back to the `waiting` room so the same group can
play again (clients call this after viewing the round results, instead of being
sent back to the menu). **Headers:** `X-Session-Key`. Resets the round seed and
every member's run state; members stay. Idempotent (no-op if already `waiting`).
**200:** `{ "success":true, "lobby": {...} }`. Errors: `401` session, `403` not a
member, `404` lobby not found.

### `POST /api/hb/mp/lobby/:code/map`
Host changes the map (waiting only). **Body:** `{ "map_id":3 }`. Errors: `403`
not host, `409` round already started.

### `POST /api/hb/mp/lobby/:code/mode`
Host changes the game mode (waiting only). **Body:** `{ "mode":"battle" }`
(`survival` | `battle` | `sprint` | `livebattle`). Switching to `livebattle`
(**Live Mode**) defaults the size to **2** (1v1) but keeps room for any players
already present, up to **8**. Errors: `403` not host, `409` round already started.

### `POST /api/hb/mp/lobby/:code/option`
Host toggles lobby game options (waiting only). **Body:** `{ "win_stays":true }`
and/or `{ "friends_only":true }`. `win_stays` is the Live Mode **"winner stays
on"** option: once enabled, after a decided round the losers are removed and the
winner becomes host and keeps the lobby for the next challengers (on a draw all
stay). `friends_only` restricts `join` to the host's friends + invited players.
**200:** `{ "success":true, "lobby": {...} }` (the lobby object reports
`win_stays` and `friends_only`). Errors: `403` not host, `409` round already
started.

### `POST /api/hb/mp/lobby/:code/maxplayers`
Host sets the lobby size (waiting only). **Body:** `{ "max_players":8 }`, clamped to
**2..8** (and never below the players already present). Applies to every mode,
including **Live Mode** (`livebattle`), which now supports up to 8 real-time peers.
**200:** `{ "success":true, "lobby": {...} }`. Errors: `403` not host, `409` round
already started.

### `POST /api/hb/mp/lobby/:code/kick`
Host kicks a player (waiting only). **Body:** `{ "username":"bob" }`. The player is
removed from the roster — this is a plain **disconnect, not a ban**, so they may
rejoin later. The kicked client detects it's no longer a member and returns to the
browser. **200:** `{ "success":true, "lobby": {...} }`. Errors: `403` not host or
target is the host, `404` not in the lobby, `409` round already started.

### `POST /api/hb/mp/lobby/:code/start`
Host starts the round. Server picks a shared `seed`, flips to `playing`, and
issues one run per member. **200:**
`{ "success":true, "seed":123, "map_id":0, "run_id":"<hex>", "nonce":"<hex>",
   "server_time":..., "lobby": {...} }` (run_id/nonce are the **caller's**).
Errors: `403` not host, `409` already started, `409` for a `livebattle` lobby with
fewer than 2 players (1v1 needs an opponent before it can start).

### `POST /api/hb/mp/lobby/:code/me`
Fetch **your** run for the current round (run_id/nonce are per-member secrets, so
this is authenticated). **200:**
`{ "success":true, "run_id":"<hex>", "nonce":"<hex>", "seed":123, "map_id":0,
   "status":"playing", "lobby": {...} }`. Non-host members call this after seeing
`status == "playing"`.

### `POST /api/hb/mp/lobby/:code/progress`
Push your live state; returns current standings. **Body:**
`{ "frame":120, "score":120, "px":148, "alive":true }`. Call ~1×/sec while
playing. `px` (your player x, 0–320) lets other clients render your live ghost.

### `POST /api/hb/mp/lobby/:code/finish`
Submit your final round score (**replay-verified**, same pipeline as `/score`).
**Body:** `{ "score":1234, "seed":123, "input_hash":"<sha256>", "sig":"<mp finish sig>",
"input_log":"0:0,40:2" }`. The server replays your run on the lobby's map and
records the result; when all members finish the lobby becomes `finished`. A
verified result is also written to the lobby's **`<mode>-<mapId>` leaderboard**
(server-derived from the lobby, e.g. `battle-2`), so multiplayer rounds build the
same per-(mode x map) boards as single-player Survival. **200:**
`{ "success":true, "verified":true, "lobby": {...} }`. Errors: `400` seed/hash
mismatch or wrong round seed, `401` bad sig, `403` not in the round, `422` failed
replay verification.

### Live Mode (real-time, up to 8 players) endpoints

These only apply to a lobby whose `mode` is `livebattle`. The lobby brokers the
peer-to-peer UDP links; the live gameplay never touches the server. With more than
2 players it's a **host-relay star** (guests talk only to the host). See
[multiplayer.md](multiplayer.md).

#### `GET` / `POST /api/hb/mp/lobby/:code/net`
Signaling. `POST` **registers** this console's UDP endpoint, `GET` just polls.
Both require the caller to be in the live round. **POST body:**
`{ "ip":"192.168.1.10", "port":41000 }` (dotted-quad IPv4 + UDP port). **200
(both):**
```json
{ "success":true, "role":0, "player_count":3, "seed":123, "map_id":0,
  "peer_ready":true, "peer_name":"bob",
  "peer_ip":"192.168.1.11", "peer_port":41001, "peer_wan_ip":"203.0.113.7",
  "peers":[
    { "index":0, "name":"alice", "host":true,  "ready":true, "ip":"", "wan_ip":"203.0.113.7", "port":3074 },
    { "index":1, "name":"bob",   "host":false, "ready":true, "ip":"192.168.1.11", "wan_ip":"203.0.113.7", "port":41001 },
    { "index":2, "name":"carol", "host":false, "ready":false, "ip":"", "wan_ip":"", "port":0 }
  ] }
```
`role` is the caller's **index in the canonical player order** (host = `0`, then
guests by join time); `player_count` is the round size. `peers[]` lists every
participant's endpoint — guests dial the host (index 0); the host learns each
guest from this list + their incoming hellos. The legacy `peer_*` fields are kept
for the 2-player case (for the host they point at the first guest). Errors: `403`
not in the round, `409` not a live lobby, `400` missing/invalid ip/port.

#### `POST /api/hb/mp/lobby/:code/battle`
Upload your input log for the finished live round. **Body:**
`{ "seed":123, "input_hash":"<sha256>", "sig":"<sig>", "input_log":"0:0,40:2" }`.
Signed like a score with board id `livebattle` and score `0`
(`score|game_id|run_id|nonce|livebattle|0|seed|input_hash`). The server stores
your log; once **all** participants' logs are in it **replays the N-player sim**
(`replayBattle`) from the shared seed + every log and records the verified
winner on the `livebattle-<mapId>` leaderboard. **200:**
`{ "success":true, "decided":true, "result":1, "winner":"alice", "lobby":{...} }`
where `result` is `+1` (you won), `0` (you lost), or `-1` (draw / not yet
decided — `decided:false` until everyone submits). Errors: `400` seed/hash
mismatch or malformed log, `401` bad sig, `403` not in the round, `409` not a
live lobby.

When the round is decided the server also updates each player's **head-to-head
record** (win for the survivor, a loss for each other player, or a draw) in
`homebrew_pvp_record`. The record is returned on each player in the lobby's
`players` array as `wins` / `losses` / `draws` / `streak`. If the lobby has
`win_stays` enabled, the losers are dropped from `players` and the winner is set
as `host` in the returned lobby.

### `GET /api/hb/leaderboard/:gameId/:boardId`
Public top-N. **Query:** `limit` (1–1000, default 100), `offset` (default 0),
`unverified=1` to include unverified scores.
```json
{ "game_id":"og-testgame","board_id":"highscore","name":"Survival Time",
  "sort":"desc","unit":"frames",
  "entries":[{"rank":1,"name":"Player","score":1234,"verified":true}],
  "total":1,"limit":100,"offset":0,"hasMore":false }
```

### `GET /api/hb/games`
List approved games: `{ "games": [ { game_id, name, description, ... } ] }`.

### `GET /api/hb/games/:gameId`
Game info + boards: `{ "game": {...}, "boards": [ { board_id, name, sort, unit } ] }`.

### `GET /api/me/hb-scores`
The signed-in player's scores (header `X-Session-Key`):
`{ "success": true, "scores": [ { game_id, game_name, board_id, board_name, score, unit, verified, updated_at } ] }`.

### Developer / admin
- `POST /api/hb/dev/games` — register a game (session). Body `{ game_id, name, description?, verify_mode? }`.
- `POST /api/hb/admin/games/:id/approve` — *admin*; returns one-time `game_secret`.
- `POST /api/hb/admin/games/:id/boards` — *admin*; body `{ board_id, name, sort, unit }`.
- `DELETE /api/hb/admin/games/:id` — *admin*; remove a game and all its data.
- `GET /api/hb/admin/games` — *admin*; list all games incl. pending.

---

## Minimal worked example (pseudocode)

```text
session_key = insignia_device_login()          # QR flow → session key

# 1. start a run
sig   = hmac_sha256_hex(secret, "start|" + game_id)
run   = POST /api/hb/run/start  {game_id, sig}   with X-Session-Key

# 2. play `run.seed` deterministically, recording inputs → input_log, score

# 3. submit
input_hash = sha256_hex(input_log)
msg = "score|"+game_id+"|"+run.run_id+"|"+run.nonce+"|"+board_id+"|"+score+"|"+run.seed+"|"+input_hash
sig = hmac_sha256_hex(secret, msg)
POST /api/hb/score {game_id, run_id:run.run_id, nonce:run.nonce, board_id,
                    score, seed:run.seed, input_hash, sig, input_log}  with X-Session-Key

# 4. (optional) report the run for achievements — server re-derives the unlocks
asig = hmac_sha256_hex(secret, "ach|"+game_id+"|"+run.run_id+"|"+run.seed+"|"+input_hash)
POST /api/hb/achievements {game_id, run_id:run.run_id, seed:run.seed,
                           input_hash, sig:asig, input_log}  with X-Session-Key

# --- multiplayer variant (shared-seed lobby) ---
mp_sig = hmac_sha256_hex(secret, "mp|" + game_id)
lobby  = POST /api/hb/mp/lobby/create {game_id, map_id, sig:mp_sig}   # host
# ...others POST /api/hb/mp/lobby/<code>/join {sig:mp_sig}...
start  = POST /api/hb/mp/lobby/<code>/start {}                        # host → shared seed
mine   = POST /api/hb/mp/lobby/<code>/me {}                           # each member → run_id/nonce/seed/map
# play `mine.seed` on `mine.map_id`, recording inputs; every ~1s:
POST /api/hb/mp/lobby/<code>/progress {frame, score, alive:true}
# on death, replay-verified finish (board id is literally "mp"):
fhash = sha256_hex(input_log)
fsig  = hmac_sha256_hex(secret, "score|"+game_id+"|"+mine.run_id+"|"+mine.nonce+"|mp|"+score+"|"+mine.seed+"|"+fhash)
POST /api/hb/mp/lobby/<code>/finish {score, seed:mine.seed, input_hash:fhash, sig:fsig, input_log}
```

To verify scores for a **replay**-mode game written outside nxdk, the server needs
a JS verifier for your game registered in `homebrew/hb-routes.js` (the `SIMS` map),
analogous to `dodge-sim.js`. Games that only sign with HMAC (`verify_mode:"hmac"`)
can submit, but their scores are stored unverified and are not ranked by default.
