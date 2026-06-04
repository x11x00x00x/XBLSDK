# xb.live Homebrew SDK reference (`sdk/xblsdk.h`)

A small C/nxdk static library that handles Insignia QR login, saved sessions,
opening server runs, HMAC-signed score submission, leaderboard reads, and input-log
recording. The SDK draws nothing itself — it calls your UI callbacks — so it stays
reusable across games.

Link `sdk/*.c` + `sim/dodge_sim.c` (if you use it) + the `third_party/` transport
into your nxdk app. See the project [Makefile](../Makefile) for the exact flags.

## Configuration & lifecycle

```c
typedef struct {
    const char *host;        // "xb.live"
    const char *port;        // "443"
    const char *auth_host;   // default "auth.insigniastats.live"
    const char *auth_port;   // default "443"
    const char *game_id;     // issued on approval, e.g. "og-testgame"
    const char *game_secret; // HMAC key (see docs/anti-cheat.md)
    const char *session_path;// e.g. "E:\\TestGame\\session.txt"
    void *ud;                // passed back to callbacks
    void (*on_status)(const char *msg, void *ud);
    void (*on_qr)(const char *url, const uint8_t *qr, int modules, void *ud);
    void (*on_logged_in)(const char *username, void *ud);
} XblConfig;

int  xbl_init(const XblConfig *cfg);  // copies cfg (keep strings alive); applies defaults
int  xbl_net_up(void);                // brings DHCP up, reports via on_status
```

`on_qr` receives a `qrcodegen` matrix for the verification URL — render it with
`qrcodegen_getModule(qr, x, y)` for `x,y` in `[-1, modules]` (see `game/game_ui.c`).

## Session & login

```c
int  xbl_login(XblSession *out);      // reuse saved session, else QR device login; persists + remembers on success
int  xbl_session_validate(XblSession *s);   // GET /api/auth/user; refreshes username
int  xbl_session_load(XblSession *out);
int  xbl_session_save(const XblSession *s);
void xbl_session_clear(void);

typedef struct { char session_key[256]; char username[128]; int logged_in; } XblSession;
```

### Multiple accounts & the shared sign-in store

The SDK can remember several signed-in accounts so a game can show a "choose an
account" screen at launch instead of always running the QR flow.

```c
#define XBL_ACCOUNTS_MAX 8
typedef struct { char username[128]; char session_key[256]; } XblAccount;

const char *xbl_accounts_store_path(void);                   // effective store path
int xbl_accounts_load(XblAccount *out, int max, int *count); // remembered accounts, MRU first
int xbl_account_remember(const XblSession *s);               // add/update + move to front
int xbl_account_forget(const char *username);                // remove one account
int xbl_session_resume(const XblAccount *a, XblSession *out); // validate a saved key & make it active
int xbl_login_new(XblSession *out);                          // force the QR flow ("Add another account")
```

#### Standard shared location

By default the account list lives in one **shared, cross-app** file so a player
signs in **once** and every SDK-based homebrew app can reuse it — there is no
per-app re-login:

```
E:\Insignia\accounts.txt        (XBL_SHARED_ACCOUNTS_PATH)
```

Each line is `username\tsessionKey`, most-recently-used first. The SDK creates
`E:\Insignia\` on first write. A game that wants its own isolated list can set
`XblConfig.accounts_path`; leave it `NULL` (the default) to share. The active
session for *this* app still lives in its own `session_path` (`session.txt`), so
different apps may currently be using different accounts from the shared pool.

Because the Insignia `sessionKey` is account-wide (it authenticates the user to
`/api/auth/user`; each game adds its own HMAC `game_secret` separately), the same
key is valid in any app. So:

- **Sign in once, reuse everywhere.** App A's QR login writes the account to the
  shared store; App B lists it at launch and `xbl_session_resume` logs in with no
  QR.
- **A dead key heals for everyone.** `xbl_session_resume` returns `XBL_ERR_AUTH`
  when a stored key has expired. The picker forgets it (so other apps stop trying
  it) and the user re-adds it via QR; the **fresh key is written back to the
  shared store**, so the next app to launch picks it up automatically.

`xbl_login`, `xbl_login_new` and a successful `xbl_session_resume` all call
`xbl_account_remember` automatically, so the active account is always the first
entry in the shared store.

> Keys are stored in plaintext, exactly like the legacy `session.txt`. The store
> is local to the console (the trust boundary is physical access), so treat any
> account on the box as usable by any homebrew app on the box.

Typical launch flow:

```c
XblAccount accts[XBL_ACCOUNTS_MAX]; int n = 0;
xbl_accounts_load(accts, XBL_ACCOUNTS_MAX, &n);
if (n <= 0) {
    xbl_login(&session);            // first run: reuse session.txt or QR
} else {
    // show accts[0..n).username + an "Add another account" row…
    if (chose_add) xbl_login_new(&session);
    else           xbl_session_resume(&accts[chosen], &session);
}
```

## Runs & scores

```c
typedef struct {
    char run_id[80];
    char nonce[80];
    uint32_t seed;                  // server-issued — play THIS seed
    int map_id;                     // server-recorded map — play THIS map
    unsigned long long server_time;
} XblRun;

int xbl_run_begin(const XblSession *s, XblRun *out);             // POST /api/hb/run/start (map 0)
int xbl_run_begin_map(const XblSession *s, int map_id, XblRun *out); // request a specific map

int xbl_score_submit(const XblSession *s, const XblRun *run, const char *board_id,
                     long long score, uint32_t seed, const char *input_log,
                     int *accepted, char *msg, size_t msgsz);  // POST /api/hb/score
```

The run carries `map_id`; play `run.map_id` (the server replays the score on that
map). Score submission doesn't send the map — the server uses the run's recorded
map, so a client can't change it after the fact.

`xbl_score_submit` builds `input_hash = SHA256(input_log)`, signs
`score|game_id|run_id|nonce|board_id|score|seed|input_hash` with the game secret,
and POSTs. On a definitive answer it returns `XBL_OK` and sets `*accepted`
(1 = verified+stored, 0 = rejected, with `msg` explaining). `XBL_ERR_REJECTED` is
returned for a 4xx rejection.

## Achievements

The SDK is generic plumbing; your game owns the catalog and the unlock
conditions. Run-derived achievements are awarded live (for the toast) and the
server re-derives every unlock by replaying the run — see
[achievements.md](achievements.md).

```c
typedef struct { const char *id, *name, *description; } XblAchievementDef;
typedef void (*XblAchievementToast)(const XblAchievementDef *def, void *ud);

int  xbl_ach_init(const XblAchievementDef *defs, int count, XblAchievementToast on_toast, void *ud);
int  xbl_ach_sync(const XblSession *s); // after sign-in: seed already-unlocked (silent, no toast)
void xbl_ach_reset(void);              // ONLY for account switch (then xbl_ach_sync); not per-run
void xbl_ach_award(const char *id);    // condition met → fires the toast once (skips already-earned)
int  xbl_ach_was_awarded(const char *id);
int  xbl_ach_report(const XblSession *s, const XblRun *run, long long score,
                    uint32_t seed, const char *input_log, char *msg, size_t msgsz); // POST /api/hb/achievements
```

`xbl_ach_report` builds `input_hash = SHA256(input_log)`, signs
`ach|game_id|run_id|seed|input_hash`, and POSTs the run. The server replays it,
grants what it can prove, and returns the newly-unlocked set; the SDK toasts any
of those not already shown (this is how cumulative achievements get their toast).
Call it after `xbl_score_submit` succeeds, **before** freeing the input log.

## Leaderboard

```c
typedef struct { int rank; char name[128]; long long score; int verified; } XblEntry;
int xbl_leaderboard_fetch(const char *board_id, int top_n, XblEntry *out, int *count);
```

## Maps

A map is a deterministic parameter preset (see [multiplayer.md](multiplayer.md)).
The built-in table is also available locally as `DODGE_MAPS` / `DODGE_MAP_COUNT`.

```c
typedef struct { int map_id; char name[24]; } XblMapInfo;
int xbl_maps_fetch(XblMapInfo *out, int max, int *count);  // GET /api/hb/games/<id>/maps
```

## Online presence

Heartbeat so the site/game can show who and how many are online. Call
`xbl_presence_ping` on a timer (~every 30s) while logged in; the server keeps the
player online for a short window (90s). See [presence.md](presence.md).

```c
#define XBL_PRESENCE_ONLINE  "online"   // in menus
#define XBL_PRESENCE_PLAYING "playing"  // in a run
#define XBL_PRESENCE_LOBBY   "lobby"    // in a multiplayer room

typedef struct { char name[128]; char status[12]; int idle_sec; } XblPresence;

// Heartbeat (session + app HMAC "presence|game_id"); *online_out gets the count.
int xbl_presence_ping (const XblSession *s, const char *status, int *online_out);
// Heartbeat with single-online-session control. `claim`=1 makes THIS launch the
// account's online session (going online here backs out any other place); `claim`=0
// just refreshes. *superseded_out (may be NULL) becomes 1 if a newer launch took
// over — the caller should then stop online play here.
int xbl_presence_ping_ex(const XblSession *s, const char *status, int claim,
                         int *online_out, int *superseded_out);
// Public reads of /api/hb/games/<id>/presence:
int xbl_presence_count(int *online_out);                       // how many online
int xbl_presence_fetch(XblPresence *out, int max, int *count); // who is online
```

**One account is "online" in only one place at a time.** The SDK tags each launch
with a random `instance` token. `xbl_presence_ping` is `claim=0` (plain refresh);
use `xbl_presence_ping_ex` with `claim=1` when going online (startup, or to take
over) so a newer login elsewhere backs out the previous place. A backed-out client
sees `*superseded_out == 1` on its next refresh. The Test Game claims at startup,
shows "Signed in elsewhere" when superseded, and lets the player reclaim (which
backs out the other console) or quit.

Only a heartbeat signed with the game secret can mark *you* online — presence
can't be spoofed for other players. The Test Game pings from its main menu and
shows the live count in the menu subtitle.

## Friends & invites

Show the player's Insignia friends (read-only) annotated with live homebrew
presence, and let players invite each other into a lobby. Invites can be accepted
in-game or on the website. See [friends.md](friends.md).

```c
typedef struct {
    char name[128];        // display name / account
    char invite_to[128];   // account to address an invite to ("" if not inviteable)
    int  online;           // online in a homebrew game now
    int  in_this_game;     // ...and it's THIS game
    char status[12];       // online | playing | lobby
    char game_id[40];      // which homebrew game
    char lobby_code[12];   // a joinable lobby of THIS game ("" if none)
    int  xbox_online;      // online on Insignia (any game) per the friends list
    int  online_any;       // unified online: homebrew OR Insignia (use for one badge)
    char playing[48];      // game they're in right now ("" if offline)
} XblFriend;

typedef struct {
    long long id;
    char from[128];        // who invited you
    char lobby_code[12];
    char status[16];       // "pending" | "accepted"
    char mode[12];         // lobby mode
    char map_name[24];
    int  map_id, player_count, max_players, age_sec;
} XblInvite;

// Friends + live homebrew presence (session). Most actionable first.
int xbl_friends_fetch (const XblSession *s, XblFriend *out, int max, int *count);
// Invite a friend to your lobby (session + app HMAC "invite|game_id"; must be a member).
int xbl_invite_send   (const XblSession *s, const char *to_username, const char *lobby_code);
// Your live invites (session) — poll to badge a notification.
int xbl_invites_fetch (const XblSession *s, XblInvite *out, int max, int *count);
// Accept invite id; lobby_code_out gets the lobby to join (session-only, works from web).
int xbl_invite_accept (const XblSession *s, long long id, char *lobby_code_out, size_t sz);
// Decline invite id (session-only).
int xbl_invite_decline(const XblSession *s, long long id);
```

`xbl_friends_fetch` returns the player's **full** Insignia friends list (online
and offline), each annotated with live homebrew presence — up to `max` (pass
`XBL_FRIENDS_MAX`, 256). Insignia and homebrew presence are unified: use
`online_any` to show a single "online" indicator (`online`/`xbox_online`
distinguish *where* if you need it), and `playing` for the game they're in right
now (homebrew game name, else the Insignia game). See [friends.md](friends.md).

Send is app-signed and requires lobby membership; accept/decline are session-only
so the **website** can accept them too. Accepting yields the lobby code — the
console then `xbl_mp_join`s it. The Test Game badges "Friends (N invites)" on the
main menu, lists invites + friends on a Friends screen, and lets you invite a
friend with **B** from inside a lobby.

## Multiplayer (lobbies)

Shared-seed competitive rooms for up to 32 players. See
[multiplayer.md](multiplayer.md) for the model and flow.

```c
typedef struct {
    char name[128]; long long score; int frame;
    int px;                              // live x (for opponent ghosts)
    int alive, finished;
    int cleared;                         // sprint: passed the finish line
    int verified;
    int wins, losses, draws, streak;     // Live Battle head-to-head record
} XblPlayer;

// Modes (survival/battle/sprint share one replay-verified run; they differ in
// win condition + UI). livebattle is real-time 1v1 over peer-to-peer UDP:
#define XBL_MODE_SURVIVAL   "survival"   // longest run wins
#define XBL_MODE_BATTLE     "battle"     // last standing wins
#define XBL_MODE_SPRINT     "sprint"     // clear the finish line
#define XBL_MODE_LIVE_BATTLE "livebattle" // sudden-death 1v1, lockstep P2P (max 2)

// Single-player only — its own deterministic forward-runner sim, verified by the
// server's 3D sim. Sim-neutral runs post to the "threed-<mapId>" boards:
#define XBL_MODE_THREED     "threed"     // 3D Mode: obstacles rush toward you

// Single-player only — a free-roam first-person arena shooter (own FPS sim).
// Single map; runs post to the "shooter-0" board (ranked by points). This board
// is TRUST-BASED: smooth analog free-look isn't replayable, so the server uses
// plausibility caps instead of a deterministic replay (scores store unverified):
#define XBL_MODE_SHOOTER    "shooter"    // Shooter Test: free-roam FPS

typedef struct {
    char code[12], host[128], name[64];
    int map_id; char map_name[24];
    char mode[12]; int target;           // mode + sprint finish frame (0 = open)
    int max_players; char status[16];    // "waiting" | "playing" | "finished"
    uint32_t seed;                       // valid once status != "waiting"
    int player_count, alive_count;
    int win_stays;                       // Live Mode option: winner keeps the lobby
    int friends_only;                    // join restricted to host's friends + invitees
    char winner[128];                    // set once status == "finished"
    XblPlayer players[32];               // sorted by score desc
} XblLobby;

typedef struct {
    char code[12], host[128], name[64];
    int map_id; char map_name[24]; char mode[12];
    int player_count, max_players;
    char status[16];                     // "waiting" | "playing"
} XblLobbyInfo;

int xbl_mp_create  (const XblSession *s, int map_id, const char *name, XblLobby *out);
int xbl_mp_list    (XblLobbyInfo *out, int max, int *count);           // lobbies incl. in-progress (public)
int xbl_mp_join    (const XblSession *s, const char *code, XblLobby *out); // allowed mid-round (spectate)
int xbl_mp_leave   (const XblSession *s, const char *code);
int xbl_mp_return  (const XblSession *s, const char *code, XblLobby *out); // re-arm to waiting after a round
int xbl_mp_get     (const char *code, XblLobby *out);                  // poll state (public, e.g. website spectate)
int xbl_mp_poll    (const XblSession *s, const char *code, XblLobby *out); // poll from the lobby room: also refreshes
                                                                       // this member's liveness so an occupied lobby
                                                                       // stays up and an abandoned one gets reaped
int xbl_mp_set_map (const XblSession *s, const char *code, int map_id, XblLobby *out);     // host
int xbl_mp_set_mode(const XblSession *s, const char *code, const char *mode, XblLobby *out); // host
int xbl_mp_set_win_stays(const XblSession *s, const char *code, int on, XblLobby *out);      // host (Live Mode)
int xbl_mp_set_friends_only(const XblSession *s, const char *code, int on, XblLobby *out);   // host: friends-only join
int xbl_mp_set_max_players(const XblSession *s, const char *code, int n, XblLobby *out);     // host: lobby size 2..8 (any mode)
int xbl_mp_kick    (const XblSession *s, const char *code, const char *username, XblLobby *out); // host: disconnect a player (no ban)
int xbl_mp_start   (const XblSession *s, const char *code, XblRun *run,
                    uint32_t *seed, int *map_id, XblLobby *out);       // host
int xbl_mp_round   (const XblSession *s, const char *code, XblRun *run,
                    uint32_t *seed, int *map_id, XblLobby *out);       // fetch MY run
int xbl_mp_progress(const XblSession *s, const char *code, int frame,
                    long long score, int alive, int px, XblLobby *out); // heartbeat (+px)
int xbl_mp_finish  (const XblSession *s, const char *code, const XblRun *run,
                    long long score, uint32_t seed, const char *input_log,
                    int *accepted, XblLobby *out);                     // replay-verified

// --- No-code matchmaking (join a match without typing a lobby code) ---
// List joinable rooms of a mode (waiting + free slot); NULL/"" mode = any. Public.
int xbl_mp_find       (const char *mode, XblLobbyInfo *out, int max, int *count);
// Host a lobby already set to `mode` (create + set_mode in one call).
int xbl_mp_create_mode(const XblSession *s, int map_id, const char *mode,
                       const char *name, XblLobby *out);
// Quick match: join the first open `mode` room, or host one if none. *is_host=1
// when you ended up hosting and are waiting for an opponent.
int xbl_mp_quick_match(const XblSession *s, const char *mode, int map_id,
                       XblLobby *out, int *is_host);
```

`xbl_mp_finish` is signed exactly like a score with the fixed board id `mp`
(`score|game_id|run_id|nonce|mp|score|seed|input_hash`); the server replays the
member's run before recording it. Lobby create/join/leave are signed with
`mp|game_id`.

### Live Mode (real-time, up to 8 players, peer-to-peer UDP)

`livebattle` (**Live Mode**) is the one real-time mode, for **2 to 8 players**.
Instead of each console rendering lagged ghosts, every console runs **one shared
N-player sim in deterministic lockstep**, exchanging only per-frame inputs. For
N > 2 it's a **host-relay star**: each guest sends its input to the host, the host
aggregates all N inputs for a frame and broadcasts the combined frame to everyone
(the classic 1v1 is the N=2 case). The lobby is used purely to **signal** the
peers (swap UDP endpoints); the live data path is peer-to-peer. Each console binds
the well-known **Xbox Live port 3074** and guests dial the host's public + LAN
endpoints on 3074 (`xbl_net_connect_peer`), the port most routers already pass —
so internet play connects without extra setup on Open/forwarded NATs. After the
round each console uploads its input log and the **server replays the N-player
sim** to decide the verified winner. Players find each other **without a code** via
the no-code matchmaking helpers above (`xbl_mp_find` / `xbl_mp_create_mode` /
`xbl_mp_quick_match`); the Test Game's **Live Mode** browser is built on them. See
[multiplayer.md](multiplayer.md) for the full design and NAT limits.

Every decided round updates a server-side **head-to-head record** (win for the
survivor, a loss for each other player, draw for all on a tie). Each player's
`wins`/`losses`/`draws`/`streak` is reported in the lobby's `players` array. The
host can also set **"winner stays on"** with `xbl_mp_set_win_stays(s, code, 1, out)`
(reported as `XblLobby.win_stays`): when on, after a decided round the losers are
dropped and the winner keeps the lobby (becoming host) for the next challengers — a
knocked-out client sees itself vanish from the roster and should return to
matchmaking.

```c
// --- UDP transport (sdk/xbl_net.h): the low-latency P2P channel ---
#define XBL_NET_LIVE_PORT 3074  // well-known Xbox Live port; bind it for the best P2P reach
typedef struct { int fd; uint16_t localPort; int hasPeer; /* ... */ } XblNet;
int xbl_net_open    (XblNet *n, uint16_t localPort);   // 0 = ephemeral; non-blocking
int xbl_net_set_peer(XblNet *n, const char *ip, uint16_t port);
// Probe several candidate endpoints at once (best-first) and lock onto the first
// that replies — also tolerates NAT port remapping. Used to rendezvous on 3074.
int xbl_net_connect_peer(XblNet *n, const char *const *ips, const uint16_t *ports,
                         int nCand, int timeoutMs);
int xbl_net_send    (XblNet *n, const void *buf, int len);
int xbl_net_recv    (XblNet *n, void *buf, int maxlen); // 0 = nothing pending
// Multi-peer variants for the host's single socket (N-player relay): send to an
// explicit destination, or recv and capture the source endpoint.
int xbl_net_send_to (XblNet *n, const char *ip, uint16_t port, const void *buf, int len);
int xbl_net_recv_from(XblNet *n, void *buf, int maxlen, char *srcIp, size_t srcIpSz, uint16_t *srcPort);
void xbl_net_close  (XblNet *n);
int xbl_net_local_ip(char *out, size_t outsz);          // our LAN IPv4 (for signaling)

// --- Deterministic lockstep netcode (sdk/xbl_net.h) ---
// Sim-agnostic 2-player lockstep over an XblNet link: exchanges only per-frame
// inputs, retransmits each input delay+1 times (loss-tolerant, no ACKs), and
// never lets the two sims desync. The game owns the loop/render/sim step.
typedef struct { XblNet *net; int role, delay, cap; /* + buffers */ } XblLockstep;
int  xbl_lockstep_init        (XblLockstep *ls, XblNet *net, int role, int delay, int maxFrames);
int  xbl_lockstep_handshake   (XblLockstep *ls, int timeoutMs);            // hole-punch / first contact
void xbl_lockstep_record_local(XblLockstep *ls, int frame, uint16_t state);// schedule @ frame+delay
void xbl_lockstep_pump        (XblLockstep *ls, int frame);                // send (redundant) + recv
int  xbl_lockstep_have        (XblLockstep *ls, int frame);                // peer input for frame ready?
void xbl_lockstep_inputs      (XblLockstep *ls, int frame, uint32_t *s0, uint32_t *s1); // ordered host/guest
uint16_t xbl_lockstep_local   (XblLockstep *ls, int frame);               // our applied input (for the log)
void xbl_lockstep_free        (XblLockstep *ls);

// --- N-player host-relay lockstep (sdk/xbl_net.h), XBL_NET_MAX_PLAYERS = 8 ---
// Star topology: the host collects every guest's per-frame input and rebroadcasts
// the aggregated frame; guests talk only to the host. Generalizes the 2-player
// layer above (use it for any size 2..8). `role` is the player index (host = 0).
typedef struct { XblNet *net; int playerCount, role, isHost, delay, cap; /* + buffers */ } XblLockstepN;
int  xbl_lockstep_n_init        (XblLockstepN *ls, XblNet *net, int playerCount, int role,
                                 int delay, int maxFrames);
int  xbl_lockstep_n_handshake   (XblLockstepN *ls, int timeoutMs);          // guests hello host; host waits for all
void xbl_lockstep_n_record_local(XblLockstepN *ls, int frame, uint16_t state);// schedule @ frame+delay
void xbl_lockstep_n_pump        (XblLockstepN *ls, int frame);              // host aggregates+broadcasts / guest sends+drains
int  xbl_lockstep_n_have        (XblLockstepN *ls, int frame);             // all inputs for frame ready?
void xbl_lockstep_n_inputs      (XblLockstepN *ls, int frame, uint32_t *out);// fills out[playerCount], ordered by index
uint16_t xbl_lockstep_n_local   (XblLockstepN *ls, int frame);            // our applied input (for the log)
void xbl_lockstep_n_free        (XblLockstepN *ls);

// --- Signaling + verified finish (sdk/xblsdk.h) ---
typedef struct {
    int role;             // caller's index in canonical order (host = 0, guests by join time)
    int player_count;     // round size (2..8)
    int peer_ready;       // 1 once the (2-player) peer has registered
    char peer_name[128];
    char peer_ip[40];     // peer's reported (LAN) IP
    char peer_wan_ip[40]; // peer's public IP as the server saw it — dialed on :3074
    int peer_port;        // peer UDP port (host byte order)
    uint32_t seed; int map_id;
    // peers[]: every participant's endpoint (guests dial index 0, the host).
} XblNetMatch;

// Register THIS console's UDP endpoint and fetch match/peer info (call once
// after the round starts); poll until out->peer_ready.
int xbl_mp_net_register(const XblSession *s, const char *code,
                        const char *local_ip, int local_port, XblNetMatch *out);
int xbl_mp_net_poll    (const XblSession *s, const char *code, XblNetMatch *out);

// Upload your input log; server replays the N-player sim once ALL logs arrive.
// *result = +1 (you won), 0 (you lost), -1 (draw / not yet decided).
int xbl_mp_battle_finish(const XblSession *s, const char *code, const XblRun *run,
                         uint32_t seed, const char *input_log,
                         int *result, char *winner_name, size_t wnsz);
```

`xbl_mp_battle_finish` is signed like a score with board id `livebattle` and a
score of `0` (`score|game_id|run_id|nonce|livebattle|0|seed|input_hash`) — the
winner is decided by the server's replay, not a client claim. The
deterministic-lockstep netcode (packet format, input delay, redundant
retransmits, ready-check, and the N-player host relay) is **all in the SDK**
(`xbl_lockstep_*` / `xbl_lockstep_n_*`); the game only samples the gamepad, runs
its own sim step with the ordered inputs, and renders. The Test Game
(`game/main.c` / `game/game_ui.c`) is the reference caller.

## Game saves (native Xbox UDATA format)

Writes dashboard-readable saves under `E:\UDATA\<TitleID>\`. Requires `E:` to be
mounted first. See [saves.md](saves.md).

```c
typedef struct { char slot[16]; char name[64]; } XblSaveInfo;

int xbl_save_init  (const char *title_name);                          // TitleMeta.xbx
int xbl_save_write (const char *slot, const char *name, const void *data, size_t len);
int xbl_save_read  (const char *slot, void *buf, size_t bufsz, size_t *out_len);
int xbl_save_list  (XblSaveInfo *out, int max, int *count);
int xbl_save_delete(const char *slot);
```

## Input log recorder

Record the held input each frame; entries are stored sparsely (only on change), in
exactly the wire format the server expects.

```c
typedef struct { char *buf; size_t len; size_t cap; int last_state; int count; } XblInputLog;

void        xbl_inputlog_init(XblInputLog *l);
int         xbl_inputlog_record(XblInputLog *l, int frame, int state); // state masked to XBL_IN_MASK
const char *xbl_inputlog_str(const XblInputLog *l);                    // "f:s,f:s,..."
void        xbl_inputlog_free(XblInputLog *l);
```

Input bits (`state`): the dodgers (and the 3D runner) use only `XBL_IN_LEFT` (1) /
`XBL_IN_RIGHT` (2), which intentionally equal `DODGE_IN_LEFT` / `DODGE_IN_RIGHT`.
`xbl_inputlog_record` masks `state` to `XBL_IN_MASK` (0x3F) and only records
changes, so the verified replay can reconstruct held state. Shooter Test does
**not** use the input log at all — as a trust-based free-roam FPS it submits its
score directly (the server applies plausibility caps rather than replaying), so it
reads the analog sticks/triggers itself and never calls `xbl_inputlog_record`.

## Crypto helpers (also public)

```c
int xbl_sha256_hex(const unsigned char *msg, size_t len, char *out);          // out >= 65
int xbl_hmac_sha256_hex(const char *key, const unsigned char *msg, size_t len, char *out);
```

## Typical flow (see `game/main.c`)

```c
xbl_init(&cfg);
xbl_net_up();
XblSession s; xbl_login(&s);

XblRun run; xbl_run_begin(&s, &run);

DodgeState st; dodgeInit(&st, run.seed);
XblInputLog log; xbl_inputlog_init(&log);
while (st.alive) {
    int state = read_pad_bits();            // XBL_IN_LEFT / XBL_IN_RIGHT
    xbl_inputlog_record(&log, st.frame, state);
    dodgeStep(&st, state);
    render(&st);
}
int accepted; char msg[256];
xbl_score_submit(&s, &run, "highscore", st.score, run.seed,
                 xbl_inputlog_str(&log), &accepted, msg, sizeof(msg));
if (accepted) {
    char amsg[256];
    xbl_ach_report(&s, &run, st.score, run.seed,   // server re-derives + grants
                   xbl_inputlog_str(&log), amsg, sizeof(amsg));
}
xbl_inputlog_free(&log);

XblEntry top[12]; int n;
xbl_leaderboard_fetch("highscore", 12, top, &n);
```

## Return codes

`XBL_OK (0)`, `XBL_ERR_CONFIG`, `XBL_ERR_NET`, `XBL_ERR_HTTP`, `XBL_ERR_AUTH`,
`XBL_ERR_PARSE`, `XBL_ERR_REJECTED`, `XBL_ERR_NOMEM`.
