# Online Multiplayer (up to 32 players)

The Test Game supports **online multiplayer for up to 32 players** built on the
same xb.live Homebrew platform that powers the leaderboards. This document
explains the model, why it is designed this way, the lobby + map system, the wire
protocol, and the SDK surface.

## The model: shared-seed competitive rounds

Real-time, 60 fps peer-to-peer collision netcode is **not** realistic on an
original Xbox talking to a cloud server over HTTPS — latency and the cost of TLS
requests make per-frame state exchange impossible. So multiplayer uses the design
that is both feasible on the hardware **and** preserves the leaderboard-grade
anti-cheat:

> Every player in a lobby plays the **same server-issued seed** on the **same
> map** at the same time. The falling-block world is therefore identical for
> everyone. Players push their **live progress** (current frame / score / alive)
> a couple of times per second so everyone sees a live scoreboard, and each
> player submits a **replay-verified** final score when they die. Highest
> survival time wins.

Because the simulation is deterministic (see [`../sim/sim-spec.md`](../sim/sim-spec.md)),
each console runs its own copy of the identical world. A network hitch only pauses
the *local* view — it can never desync the world, and it cannot help a player
cheat, because the final score is re-simulated server-side exactly like a
single-player score (see [anti-cheat.md](anti-cheat.md)).

This is "shared-world competitive racing", not "players colliding with each
other". It scales cleanly to 32 players because the per-player traffic is a small
JSON heartbeat, not a physics stream.

## Lobbies

A **lobby** is a room identified by a short, easy-to-type **code** (6 chars from
an unambiguous alphabet, e.g. `K7P2QM`).

- The **host** creates the lobby and chooses the **map**. They can change the map
  while the lobby is `waiting`.
- Up to **32 players** join from the lobby browser (or by code).
- The host **starts** the round. The server then:
  1. picks one random **seed**,
  2. flips the lobby to `playing` and stamps `round_started_at`,
  3. creates one server-tracked **run** (run_id + single-use nonce) per member,
     bound to the shared seed and the lobby's map.
- Each member fetches their own run (`/me`), plays, pushes progress, and finishes.
- When **every** member has finished (or the lobby goes stale), the lobby becomes
  `finished` and the final standings are frozen.

Lobby lifecycle: `waiting → playing → finished`. If the host leaves while
`waiting` the host role migrates (or the lobby disbands once empty).

**Abandoned-lobby reaping.** A player who quits to the dashboard, powers off, or
pulls the plug never sends a clean `/leave`, so without help their lobby would
sit stuck — a `playing` round stuck "in progress", or a `waiting` room stuck
"hosted by me, 1/2 players". Two reapers clear these:

- **`waiting` rooms** rely on each member's per-lobby liveness: the client polls
  the room with `xbl_mp_poll` (authenticated), which refreshes that member's
  `last_seen` ~1–2×/sec. A member who stops polling (quit / powered off / backed
  out to the browser) goes stale after **100 seconds** and is dropped; if the
  host is dropped the role migrates to the freshest remaining member, and a room
  with no live members left is disbanded. Online *presence* is deliberately
  **not** used here — a host idling in the multiplayer menu is still "online",
  which would keep their own ghost lobby alive forever.
- **`playing` rounds** that go silent for **5 minutes** are reaped. That window is
  safely longer than the longest possible round (a peer-to-peer Live Battle runs
  up to `DODGE_MAX_FRAMES` ≈ 3 minutes talking only to its peer), and a genuinely
  live round is never reaped because shared-seed modes keep members fresh via
  progress heartbeats.

Both run on `GET /api/hb/mp/lobbies` and `POST /api/hb/mp/lobby/create`, so the
list self-heals — the ghost disappears the moment anyone reopens the multiplayer
screen. (A 30-minute `updated_at` sweep still purges anything else as a backstop.)

## Maps

A **map** is a deterministic **parameter preset** for the dodger — it changes how
the world behaves (block speed, spawn rate, block sizes, difficulty ramp) but
**not** the field geometry, so the same renderer works for every map. Maps are
part of the verification contract: a run records its `map_id`, and the server
replays it on that exact map.

The built-in maps (mirror of `DODGE_MAPS[]` in `sim/dodge_sim.c` /
`MAPS` in `homebrew/dodge-sim.js`):

| id | Name      | Feel | Theme (falling shape / player / floor) |
|----|-----------|------|----------------------------------------|
| 0  | Classic   | The original tuning (unchanged; pre-map scores still replay identically). | blue/orange — rectangles / block / block floor |
| 1  | Rush      | Faster blocks, tighter spawns. | hot red — falling spikes / chevron / spiked floor |
| 2  | Wideload  | Big, wide blocks — more weaving. | earthy green/gold — rounded blobs / blob / brick floor |
| 3  | Blizzard  | Fast + dense; hardest. | icy cyan — diamond shards / diamond / jagged-ice floor |
| 4  | Zen       | Slow and sparse; easiest. | calm violet/teal — hexes / hex / soft-dot floor |

Map 0 is byte-for-byte the original constants, so **all existing single-player
scores and test vectors remain valid**.

### Per-map themes (visuals only)

Each map has its own **look**: background gradient, the **shape of the falling
obstacles**, the **shape of the player** (the piece you slide along the bottom),
and a **themed scrolling floor strip**. These are purely cosmetic — they live in
the renderer (`game/game_ui.c`, the `G_THEMES[]` table, indexed by `map_id`) and
do **not** touch the deterministic simulation, so they never affect scoring or
verification. The same `map_id` that drives the sim also selects the theme.

### Per-(mode × map) leaderboards

Because difficulty and rules differ by both mode and map, **each (mode × map) has
its own leaderboard**, with board id `"<mode>-<mapId>"` (e.g. `survival-0`,
`battle-3`, `sprint-2`). Single-player Survival posts to `survival-<mapId>` (the
server rejects a board that doesn't match the run's map); verified multiplayer
rounds are recorded on their lobby's `<mode>-<mapId>` board. On the website the
board picker groups these as a mode × map matrix, and on xb.live each board is
mirrored as its own game-page leaderboard.

Fetch the catalog at runtime with `GET /api/hb/games/<game_id>/maps`, or use the
local table via `DODGE_MAPS` / `xbl_maps_fetch()`.

## Game modes

A lobby also has a **mode** (chosen by the host while `waiting`, alongside the
map). All three modes use the **same deterministic run and the exact same
replay verification** — they are *not* separate physics. What differs is the
**win condition**, the **HUD/overlay**, and how a spectator sees the match. This
is deliberate: within a deterministic shared world the only honest competitive
axis is *how far you survive*, so modes reframe that axis rather than faking
interaction (which determinism forbids — see Honest limitations).

In **every** mode each client also transmits its **player x (`px`)** in the
progress heartbeat, so opponents are drawn as live **ghosts** on your own
gauntlet ("see the other players while you play"). Each ghost is a **grayed-out
hollow outline** of the player block (just its edges, never a filled block) so
it's never confused with your own solid block. Ghosts lag by the heartbeat
cadence (~0.75 s); they can never desync or affect your world.

**Lobby size.** The host can size any lobby from **2 to 8** players via
`POST /api/hb/mp/lobby/:code/maxplayers` (SDK `xbl_mp_set_max_players`, the
in-game lobby **Settings** overlay). Shared-seed lobbies (survival/battle/sprint)
all run the same field and see each other live; **Live Mode** (`livebattle`) is
the real-time peer-to-peer mode and now supports up to 8 players too (it just
defaults to 1v1 — see the dedicated section).

| Mode | Win condition | What's special |
|------|---------------|----------------|
| `survival` | Longest run wins. | Open-ended endurance; the classic scoreboard. |
| `battle` | **Last player alive** wins. | Elimination spotlight: live ghosts of every player, an `ALIVE n/m` counter, and a crash feed. |
| `sprint` | **Clear the gauntlet** (survive past the finish line, `target` frames). | A finish-line progress bar; multiple players can "clear"; non-clearers ranked by distance. |
| `livebattle` (**Live Mode**) | **Sudden death — last player alive wins** (2..8). | **True real-time peer-to-peer**: every console runs one shared N-player sim in deterministic **host-relay** lockstep over direct UDP, so you see the other players move *live* (not lagged ghosts). See the dedicated section below. |

- `target` is reported on the lobby object: `0` for survival/battle, `900`
  frames (15 s) for sprint. Sprint's "cleared" flag is simply `score >= target`,
  so it is verified by the same replay — no special server path.
- The `winner` field is filled once the round is `finished`: the top verified
  score (for sprint, the best score among clearers). Because survival time is the
  shared metric, battle's "last standing" is exactly the highest frame count.

Change the mode with `POST /api/hb/mp/lobby/:code/mode` (host, waiting only) or
`xbl_mp_set_mode()`. In the Test Game the host cycles modes with **Up/Down** in
the lobby room.

## Live Mode (real-time, up to 8 players, peer-to-peer)

The other modes share a world but each console only renders *lagged ghosts* of
its opponents. **Live Mode** (mode id `livebattle`) is different: it is genuine
**real-time** play for **2 to 8 players** where everyone dodges the same falling
blocks at the same time and you see the other players move with no perceptible
lag. It is the one mode that is **not** mediated by the server in the data path.
(It defaults to 1v1; the host raises the size in **Settings**.)

It works *because* the sim is deterministic. Players dodging the **same seeded
field** never need to exchange world state — only their **per-frame inputs**.
For N players that is **host-relay deterministic lockstep** (a star topology, not
a full mesh): each guest sends its input to the host, the host aggregates all N
inputs for a frame and broadcasts the combined frame to everyone, and every
console runs the identical N-player sim.

1. **Signaling (via the lobby).** When the host (mode `livebattle`) starts the
   round, each console opens a UDP socket — bound to the well-known **Xbox Live
   port 3074** (`XBL_NET_LIVE_PORT`), falling back to an ephemeral port if it's
   taken — and registers its endpoint with the lobby (`/net`). The server returns
   a **canonical player order** (host first, then guests by join time) as your
   `role`/index, the `player_count`, and a **`peers[]`** array of every
   participant's LAN + public endpoint. Guests dial the **host**; the host learns
   each guest's address as their hellos arrive (it also gets their endpoints from
   `peers[]`). The host is sim **player 0**; guests are players 1..N-1, identical
   on every machine, so the result is symmetric.
2. **Lockstep.** An input sampled at frame *f* is **applied at frame f + delay**
   (delay = 3 frames ≈ 50 ms). Each guest sends its scheduled input to the host
   (with redundancy, since UDP is lossy); the host, once it holds **all N**
   inputs for *f*, broadcasts the aggregated `{frame, input[N]}` packet to every
   guest. A console only advances frame *f* once it has the full input set, then
   runs the identical N-player sim (`dodgeBattleStep`), so the field and all
   avatars stay perfectly in sync — a desync is impossible. (The classic 1v1 is
   simply the N=2 case.)
3. **Sudden death.** The round ends the instant only one player is left alive (or
   the frame cap is reached); the **last player standing wins**, ties broken by
   blocks dodged. Because the sim is deterministic, *every* console computes the
   same end frame and winner at the same moment — no "you died" message is needed
   over the wire.
4. **Verified result (anti-cheat).** The live outcome is only **provisional**.
   After the round each console uploads its **input log** (`/battle`). Once all
   logs are in, the **server replays the N-player sim** (`replayBattle`) from the
   shared seed + every log and records the **authoritative** winner. A forged log
   cannot win: it must replay to a real run that out-survives the (equally
   replayed) field. The verified result lands on the `livebattle-<mapId>`
   leaderboard and mirrors to the website like every other board.

**Transport / NAT — honest limits.** The UDP channels are direct
console-to-console (true P2P), with the lobby acting only as a
matchmaking/signaling broker. Binding **port 3074** is deliberate: it's the port
most home routers already open/forward for the Xbox, so dialing the host on its
*public address:3074* is the same approach retail Xbox Live uses and connects out
of the box for **Open NAT / port-forwarded / UPnP** setups — as well as on a
**LAN** and under **xemu**. It is **not** a full ICE/TURN stack: a strict
symmetric-NAT host with no forwarding can still fail to accept guests (a relay
would be follow-up work). With N players the **host** must be reachable on its UDP
port by up to 7 guests, and lockstep advances at the speed of the slowest link, so
a laggy player stalls the round more (the existing `LB_STALL_MS` abort still
applies). Because the data path is peer-to-peer, the website does **not** spectate
a live match frame-by-frame; it shows the match in the live list and the verified
result on the leaderboard.

The N-player sim lives in `sim/dodge_sim.c` (`dodgeBattleStep` /
`dodgeBattleReplay`, `DODGE_BATTLE_MAX_PLAYERS = 8`) and is mirrored byte-for-byte
in `homebrew/dodge-sim.js` (`replayBattle`). The reusable netcode is **all in the
SDK** (`sdk/xbl_net.c`): the UDP transport (`xbl_net_*`, incl. `xbl_net_send_to` /
`xbl_net_recv_from` for the host's multi-peer socket), the signaling helpers
(`xbl_mp_net_*`, `xbl_mp_battle_finish`), the 2-peer lockstep (`xbl_lockstep_*`),
and the **N-player host-relay lockstep** layer (`xbl_lockstep_n_*` — hello
handshake, per-frame aggregation, loss-tolerant retransmits, ready-check).
**No-code matchmaking** is in the SDK too (`xbl_mp_find` / `xbl_mp_create_mode` /
`xbl_mp_quick_match`): list joinable rooms by mode, host one already set to a mode,
or "quick match" into the first open room (hosting if none) — so players never type
a lobby code. A game only supplies its own deterministic sim step and render.

In the Test Game, **Multiplayer** opens a small submenu with two options:
**Online Mode** (the shared-seed lobby browser) and **Live Mode** (the real-time
peer-to-peer mode, which takes the homepage's old 1v1 slot). Live Mode is a
matchmaking-style browser that lists open rooms so you can **join one with a single
button — no code to type** — or host your own (which creates a lobby already
switched to `livebattle`). The host **cannot start until a second player has
joined** (the server rejects a solo `livebattle` start with `409`). Once players
are in, every console shows "connecting… / waiting…", then the live match.

### Lobby controls, settings, kick & friends-only

The Live Mode lobby room uses these controls:

- **Start** — start the round (host).
- **Up/Down** — move a cursor over the player roster.
- **A** — (host) **kick** the highlighted player (with an "Are you sure?"
  confirm). A kick just disconnects them — no ban — so they can rejoin later. The
  kicked client notices it's no longer in the roster and returns to the browser.
- **X** — (host) open the **Settings** overlay.
- **Y** — invite a friend (any member).
- **B/Back** — leave the lobby, via an "Are you sure?" popup (**A** = leave,
  **B** = stay).

The host **Settings** overlay edits, in one place: **Mode**, **Map**, **Lobby
size** (2..8), **Winner stays**, and **Friends only**. Each row calls the matching
endpoint (`/mode`, `/map`, `/maxplayers`, `/option`).

**Friends only.** When the host enables it
(`POST /api/hb/mp/lobby/:code/option {"friends_only":true}`, reported as
`friends_only` on the lobby), `join` admits only the **host's friends**
(`getProfileFriendsList(host)`) plus anyone who was **invited**; everyone else
gets `403`.

### Win/loss record + winner stays

Live Mode keeps a **head-to-head record** per player: every decided round records
a win for the survivor and a loss for each other player (a true draw counts as a
draw). The record (`wins`/`losses`/`draws`/`streak`) rides along on each player in
the lobby roster (`lobbyPublic`), so the lobby and result screens can show
"`alice 3W-1L`". Records are stored server-side in `homebrew_pvp_record`, keyed by
`(game_id, mode, username)`, and are authoritative — they're updated only when the
server replays and decides the match, so they can't be inflated by API calls. The
Test Game also mirrors *your own* tally into the local save (`lw=`/`ll=`).

With **"winner stays on"** enabled (Settings, host + waiting only), after a decided
round the **losers are dropped from the lobby** and the **winner becomes host** and
keeps the lobby for the next challengers — so a hot seat forms automatically.
Knocked-out clients notice they're no longer in the roster and return to
matchmaking. On a draw everyone stays for a rematch. The option is reported on the
lobby object as `win_stays`.

> ⚠️ Changing a map's numbers changes scores, so it is a **sim-contract change**:
> bump it in *both* `dodge_sim.c` and `dodge-sim.js`, keep map 0 as Classic, and
> regenerate test vectors. See [`../sim/sim-spec.md`](../sim/sim-spec.md).

## Wire protocol (`/api/hb/mp/*`)

All lobby-mutating calls are authenticated with the player's `X-Session-Key` and
HMAC-signed with the app secret (`sig = HMAC(secret, "mp|<game_id>")`), exactly
like `run/start`. The round **finish** is signed like a score (board id `mp`).
Full details are in [rest-api.md](rest-api.md).

| Method & path | Auth | Purpose |
|---------------|------|---------|
| `POST /api/hb/mp/lobby/create` | session + `mp` sig | Open a lobby (you become host). |
| `GET  /api/hb/mp/lobbies?game_id=&status=` | none | List lobbies. `status=all` includes in-progress ones. |
| `GET  /api/hb/mp/lobby/:code` | none (session optional) | Poll lobby state + live standings. With a session (`xbl_mp_poll`) it also refreshes the caller's per-lobby liveness so an occupied `waiting` room isn't reaped. |
| `POST /api/hb/mp/lobby/:code/join` | session + `mp` sig | Join a lobby. Allowed mid-round (you spectate, then play next round). |
| `POST /api/hb/mp/lobby/:code/leave` | session + `mp` sig | Leave (host leaving disbands). |
| `POST /api/hb/mp/lobby/:code/return` | session | After a round, re-arm the lobby to `waiting` so the group plays again (idempotent). |
| `POST /api/hb/mp/lobby/:code/map` | host session | Change the map (waiting only). |
| `POST /api/hb/mp/lobby/:code/mode` | host session | Change the mode (waiting only). |
| `POST /api/hb/mp/lobby/:code/option` | host session | Toggle game options (waiting only), e.g. `{"win_stays":true}` or `{"friends_only":true}`. |
| `POST /api/hb/mp/lobby/:code/maxplayers` | host session | Set the lobby size 2..8 (waiting only), including Live Mode. |
| `POST /api/hb/mp/lobby/:code/kick` | host session | Kick a player `{username}` (disconnect, no ban — they can rejoin). |
| `POST /api/hb/mp/lobby/:code/start` | host session | Start the round; returns shared seed + your run. |
| `POST /api/hb/mp/lobby/:code/me` | session | Fetch *your* run (run_id/nonce) + seed/map for the round. |
| `POST /api/hb/mp/lobby/:code/progress` | session | Push your `frame/score/px/alive`; returns standings. |
| `POST /api/hb/mp/lobby/:code/finish` | session + score sig | Submit your final score (replay-verified). |
| `GET  /api/hb/mp/lobby/:code/net` | session | **Live Mode:** poll your role/index, `player_count`, and the `peers[]` UDP endpoints. |
| `POST /api/hb/mp/lobby/:code/net` | session | **Live Mode:** register your UDP endpoint `{ip,port}`; returns the canonical peer list. |
| `POST /api/hb/mp/lobby/:code/battle` | session + score sig (board `livebattle`) | **Live Mode:** upload your input log; once all are in the server replays the N-player sim and decides the verified winner. |

`run_id` and `nonce` are **per-member secrets** — the public `GET` never returns
them; members fetch their own via the authenticated `/me`.

### Typical flow

```
host:  create  ─────────────► lobby (waiting)
all:   join / poll GET ──────► see roster + map
host:  (optional) map ───────► change map
host:  start ────────────────► server picks SEED, status=playing, runs issued
all:   me ───────────────────► get my run_id+nonce, SEED, map_id
all:   play SEED on map (deterministic), record inputs
all:   progress (×N) ────────► live standings for everyone
each:  finish (score+log) ───► server REPLAYS the run, verifies, records
                               when all finished → status=finished
all:   GET results ──────────► final standings
all:   return ───────────────► lobby re-armed to waiting → host can start again
```

After a round the client **stays in the lobby**: it shows the results, calls
`/return` (which re-arms the lobby to `waiting`), and drops back into the lobby
room so the same group can play again. Nobody is kicked out to make a new lobby.
A player who **joins while a round is in progress** spectates the standings for
that round and is dealt in on the next one; the lobby room shows a brief
"X joined" banner at the top whenever someone new arrives.

## SDK surface (C / nxdk)

See [sdk-reference.md](sdk-reference.md) for full signatures. The essentials:

```c
XblLobby lobby; XblRun run; uint32_t seed; int mapId;

// Host
xbl_mp_create(&session, /*map_id*/0, "My lobby", &lobby);
xbl_mp_set_map(&session, lobby.code, 3, &lobby);          // change map while waiting
xbl_mp_set_mode(&session, lobby.code, XBL_MODE_BATTLE, &lobby); // change mode
xbl_mp_start(&session, lobby.code, &run, &seed, &mapId, &lobby);

// Guest
XblLobbyInfo list[16]; int n;
xbl_mp_list(list, 16, &n);                          // browse open lobbies
xbl_mp_join(&session, list[0].code, &lobby);
// ...poll xbl_mp_get(code,&lobby) until lobby.status == "playing"...
xbl_mp_round(&session, lobby.code, &run, &seed, &mapId, &lobby); // get my run

// Both: play the shared seed on the map, then
//   every ~0.75s:  xbl_mp_progress(&session, code, frame, score, 1, px, &lobby);
//   on death:      xbl_mp_finish(&session, code, &run, score, seed, log, &ok, &lobby);
// lobby.players[i].px / .alive / .cleared drive the live opponent ghosts.

// After the round, go back to the lobby instead of the menu:
xbl_mp_return(&session, lobby.code, &lobby);             // re-arm to waiting (idempotent)
```

`XblLobby.players[]` is sorted by score and is what the in-round standings
sidebar (`gui_renderGameMp`) and the results screen (`gui_mpResults`) render.

## In the Test Game

From the main menu: **Multiplayer** → **Online Mode** or **Live Mode** → lobby
browser (create or join) → lobby room. In the room the host opens **Settings**
(**X**) to change the **mode/map/size/winner-stays/friends-only**, moves the roster
cursor with **Up/Down** (and **A** to kick), and presses **Start** to begin; any
member invites a friend with **Y**, and **B/Back** leaves via a confirm popup. The
round runs with opponent ghosts (Online Mode) or live peers (Live Mode) on the
field + a live standings sidebar (and a finish-line bar in sprint) → mode-aware
final results. The Test Game polls lobby state about once per second and pushes
progress (with `px`) every ~45 frames.

## On the website

The Homebrew Leaderboards page has a **Live matches** panel that lists open and
in-progress lobbies for the selected game (`GET /api/hb/mp/lobbies?status=all`).
Click one to **spectate**: it polls the lobby and renders a mode-aware live board
— survival/battle show score bars with alive/crashed state (and the winner in
gold), sprint shows progress toward the finish line with a "cleared" marker.

## Honest limitations

- **Not real-time interactive.** Players don't collide with each other; they race
  the same world (you see opponent *ghosts*, not true collisions). This is the
  only model that fits the hardware + transport, and it is why the game modes
  differ in goal/presentation rather than in physics.
- **Heartbeat granularity.** Live standings update at the progress cadence
  (~1–2 s), not per frame.
- **Network hitches pause the local view.** The deterministic sim tolerates this
  (it never desyncs), but a player on a bad connection sees stutter. A background
  sync thread could smooth this; it is intentionally kept simple here.
- **Achievements** are reported from single-player runs; the multiplayer finish
  path verifies scores but does not separately grant achievements.
