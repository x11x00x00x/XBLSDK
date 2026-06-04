/*
 * xblsdk.h - xb.live Homebrew SDK (original Xbox / nxdk).
 *
 * A small C library any approved homebrew game links against to:
 *   1. Log a player in to their xb.live (Insignia) account via the on-screen QR
 *      device flow, reusing a saved session on later runs.
 *   2. Open a server-tracked "run" (gets a server-issued seed + nonce).
 *   3. Submit a score together with the run's seed + input log, HMAC-signed with
 *      the game's secret. The server re-simulates the run to verify the score
 *      (see docs/anti-cheat.md and sim/sim-spec.md).
 *   4. Read back a leaderboard.
 *
 * The SDK is UI-agnostic: it renders nothing itself. Instead it calls the
 * callbacks in XblConfig so the game draws the QR code / status with its own UI.
 *
 * Networking must be brought up with xbl_net_up() before login/score calls.
 */
#ifndef XBLSDK_H
#define XBLSDK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBL_SK_MAX 256
#define XBL_USER_MAX 128
#define XBL_RUNID_MAX 80
#define XBL_NONCE_MAX 80
#define XBL_MSG_MAX 256
#define XBL_CODE_MAX 12
#define XBL_MAPNAME_MAX 24
#define XBL_MP_MAX_PLAYERS 32
#define XBL_LOBBY_NAME_MAX 64
#define XBL_PRESENCE_STATUS_MAX 12
#define XBL_PRESENCE_LIST_MAX 32
#define XBL_GAMEID_MAX 40
#define XBL_GAMENAME_MAX 48
#define XBL_FRIENDS_MAX 256
#define XBL_INVITES_MAX 16
#define XBL_ACCOUNTS_MAX 8

/* Standard, cross-app sign-in store. Every SDK app reads/writes this shared file
 * by default so a single Insignia login is reusable across all homebrew apps,
 * and a re-login (refreshing a dead key) in one app propagates to the rest.
 * Override per-app with XblConfig.accounts_path only if isolation is required. */
#define XBL_SHARED_ACCOUNTS_DIR "E:\\Insignia"
#define XBL_SHARED_ACCOUNTS_PATH XBL_SHARED_ACCOUNTS_DIR "\\accounts.txt"

/* Return codes. */
#define XBL_OK 0
#define XBL_ERR_CONFIG (-1)
#define XBL_ERR_NET (-2)
#define XBL_ERR_HTTP (-3)
#define XBL_ERR_AUTH (-4)
#define XBL_ERR_PARSE (-5)
#define XBL_ERR_REJECTED (-6) /* server rejected the score (failed verification) */
#define XBL_ERR_NOMEM (-7)

/* Held-input bitmask values reported into the input log. LEFT/RIGHT match
 * DODGE_IN_LEFT/RIGHT in sim/dodge_sim.h (the dodgers use only those two). The
 * extra bits are used by Shooter Test (sim/shooter_sim.h): a 2D crosshair plus
 * fire/reload. XBL_IN_MASK is the full set the input log may carry. */
#define XBL_IN_LEFT 1u
#define XBL_IN_RIGHT 2u
#define XBL_IN_UP 4u
#define XBL_IN_DOWN 8u
#define XBL_IN_FIRE 16u
#define XBL_IN_RELOAD 32u
#define XBL_IN_MASK 0x3Fu

typedef struct {
    /* xb.live site host that serves the /api/hb/ leaderboard endpoints. */
    const char *host; /* e.g. "xb.live" */
    const char *port; /* e.g. "443" */
    /* Insignia auth host that serves /api/auth/ (device flow + session check). */
    const char *auth_host; /* default "auth.insigniastats.live" */
    const char *auth_port; /* default "443" */

    /* Identity of this game, issued by xb.live when the game is approved. */
    const char *game_id;     /* slug, e.g. "og-testgame" */
    const char *game_secret; /* HMAC key (see docs/anti-cheat.md) */

    /* File used to persist the login between runs (line1 sessionKey, line2 user). */
    const char *session_path; /* e.g. "E:\\TestGame\\session.txt" */
    /* Optional: file used to remember multiple accounts for the login picker
     * ("username\tsessionKey" per line, most-recent first). If NULL, the SDK
     * uses the SHARED, cross-app store at XBL_SHARED_ACCOUNTS_PATH so a sign-in
     * in one homebrew app is reusable by every other SDK app. Set this only if a
     * game wants its own isolated account list. */
    const char *accounts_path;

    /* Optional UI hooks (any may be NULL). `ud` is passed back to each. */
    void *ud;
    void (*on_status)(const char *msg, void *ud);
    /* Called once when a QR code must be shown. `qr`/`modules` is a qrcodegen
     * matrix (use qrcodegen_getModule) for the verification URL in `url`. */
    void (*on_qr)(const char *url, const uint8_t *qr, int modules, void *ud);
    void (*on_logged_in)(const char *username, void *ud);
} XblConfig;

typedef struct {
    char session_key[XBL_SK_MAX];
    char username[XBL_USER_MAX];
    int logged_in;
} XblSession;

/* A remembered account for the login picker (username + its saved session key). */
typedef struct {
    char username[XBL_USER_MAX];
    char session_key[XBL_SK_MAX];
} XblAccount;

typedef struct {
    char run_id[XBL_RUNID_MAX];
    char nonce[XBL_NONCE_MAX];
    uint32_t seed;                  /* server-issued; the game MUST play this seed */
    int map_id;                     /* server-recorded map; the game MUST play it */
    unsigned long long server_time; /* server epoch seconds at run start */
} XblRun;

typedef struct {
    int rank;
    char name[XBL_USER_MAX];
    long long score;
    int verified;
} XblEntry;

/* Growable "frame:state,..." input log (see sim/sim-spec.md transport format). */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    int last_state;
    int count;
} XblInputLog;

/* ---- lifecycle ---- */

/* Stores a copy of cfg (strings are NOT copied; keep them alive). Applies
 * defaults for auth_host/port. Returns XBL_OK or XBL_ERR_CONFIG. */
int xbl_init(const XblConfig *cfg);

/* Brings the network up (DHCP) with progress via on_status. Blocks until an IPv4
 * address is bound or it times out. Returns XBL_OK or XBL_ERR_NET. */
int xbl_net_up(void);

/* ---- session ---- */

int xbl_session_load(XblSession *out);             /* from session_path */
int xbl_session_save(const XblSession *s);
void xbl_session_clear(void);
/* Validates s->session_key against /api/auth/user; refreshes username on success.
 * Returns XBL_OK if still valid, XBL_ERR_AUTH if not, or a transport error. */
int xbl_session_validate(XblSession *s);
/* Reuse-or-QR: loads a saved session and validates it; if missing/invalid runs
 * the QR device login. On success fills `out`, persists it, and remembers the
 * account for the login picker. */
int xbl_login(XblSession *out);

/* ---- multiple accounts (login picker) ---- */

/* Effective path of the account store: the shared, cross-app
 * XBL_SHARED_ACCOUNTS_PATH unless XblConfig.accounts_path overrides it. */
const char *xbl_accounts_store_path(void);
/* Load remembered accounts (most-recently-used first) into out[0..max). Sets
 * *count. Returns XBL_OK if at least one account was read, XBL_ERR_AUTH if the
 * store is empty/missing. */
int xbl_accounts_load(XblAccount *out, int max, int *count);
/* Add or update an account from a live session and move it to the front of the
 * list. Called automatically by xbl_login / xbl_login_new / xbl_session_resume. */
int xbl_account_remember(const XblSession *s);
/* Remove a remembered account by username. */
int xbl_account_forget(const char *username);
/* Resume a remembered account: validates its stored key against the server. On
 * XBL_OK fills `out`, makes it the active session and bumps it to the front. On
 * XBL_ERR_AUTH the key is stale (caller should forget it and re-add). */
int xbl_session_resume(const XblAccount *a, XblSession *out);
/* Always run the QR device login (e.g. "Add another account"), then persist it
 * as the active session and remember it. */
int xbl_login_new(XblSession *out);

/* ---- runs + scores ---- */

/* POST /api/hb/run/start on map 0. Fills run (run_id, nonce, seed, map_id). */
int xbl_run_begin(const XblSession *s, XblRun *out);

/* POST /api/hb/run/start requesting `map_id` (clamped server-side). The returned
 * run carries the map the game MUST play; score replay uses it. */
int xbl_run_begin_map(const XblSession *s, int map_id, XblRun *out);

/* POST /api/hb/score. `input_log` is the "f:s,..." string from XblInputLog.
 * On a 2xx the score was verified+stored; *accepted is set to 1. On a 4xx the
 * server rejected it (verification/anomaly); *accepted is 0 and msg explains.
 * Returns XBL_OK when a definitive answer was received (accepted or rejected),
 * or a negative transport error. accepted/msg may be NULL. */
int xbl_score_submit(const XblSession *s, const XblRun *run, const char *board_id,
                     long long score, uint32_t seed, const char *input_log, int *accepted,
                     char *msg, size_t msgsz);

/* GET /api/hb/leaderboard/<game_id>/<board_id>?limit=top_n. Fills up to top_n
 * entries; *count gets how many were parsed. Public (no session needed). */
int xbl_leaderboard_fetch(const char *board_id, int top_n, XblEntry *out, int *count);

/* ---- maps ---- */

typedef struct {
    int map_id;
    char name[XBL_MAPNAME_MAX];
} XblMapInfo;

/* GET /api/hb/games/<game_id>/maps. Fills up to `max` maps; *count gets how many.
 * The map list is also available locally via DODGE_MAPS in sim/dodge_sim.h. */
int xbl_maps_fetch(XblMapInfo *out, int max, int *count);

/* ---- online presence (see docs/presence.md) ----
 *
 * Call xbl_presence_ping() on a timer (~every 30s) while the game is open and the
 * user is logged in; the server marks them online for a short window. Use
 * xbl_presence_count()/xbl_presence_fetch() to show how many / who is online. */

/* Coarse activity hints passed to xbl_presence_ping (server clamps unknowns). */
#define XBL_PRESENCE_ONLINE "online"   /* in menus */
#define XBL_PRESENCE_PLAYING "playing" /* in a run */
#define XBL_PRESENCE_LOBBY "lobby"     /* in a multiplayer room */

typedef struct {
    char name[XBL_USER_MAX];
    char status[XBL_PRESENCE_STATUS_MAX];
    int idle_sec; /* seconds since their last heartbeat */
} XblPresence;

/* Heartbeat: marks the logged-in user online for this game. `status` may be NULL
 * (treated as "online"). On success, *online_out (may be NULL) gets the current
 * online count. Requires a valid session + the app HMAC. */
int xbl_presence_ping(const XblSession *s, const char *status, int *online_out);

/* Heartbeat with single-online-session control. Each account may only be "online"
 * in one place at a time. Pass `claim`=1 when this launch is going online (e.g.
 * at startup, or to deliberately take over) — it makes THIS instance the online
 * session and backs out any other place. Pass `claim`=0 for periodic refreshes;
 * if a newer instance has taken over, *superseded_out (may be NULL) is set to 1
 * and the caller should stop online play here. */
int xbl_presence_ping_ex(const XblSession *s, const char *status, int claim, int *online_out,
                         int *superseded_out);

/* GET /api/hb/games/<game_id>/presence. *online_out gets how many are online
 * right now. Public (no session needed). */
int xbl_presence_count(int *online_out);

/* GET /api/hb/games/<game_id>/presence. Fills up to `max` online players (most
 * recent first); *count gets how many were parsed. Public (no session needed). */
int xbl_presence_fetch(XblPresence *out, int max, int *count);

/* ---- multiplayer lobbies (shared-seed competitive; see docs/multiplayer.md) ---- */

typedef struct {
    char name[XBL_USER_MAX];
    long long score; /* live score, or final score once finished */
    int frame;
    int px;       /* last-known player x (for live opponent ghosts) */
    int alive;
    int finished;
    int cleared;  /* sprint: 1 if this player passed the finish line */
    int verified;
    /* Live Battle head-to-head record (0 for other modes). */
    int wins;
    int losses;
    int draws;
    int streak;   /* current win streak (negative/0 once broken) */
} XblPlayer;

/* Game modes (see docs/multiplayer.md). All share the same replay-verified run;
 * they differ in win condition + presentation. */
#define XBL_MODE_SURVIVAL "survival" /* longest run wins */
#define XBL_MODE_BATTLE "battle"     /* last standing wins (elimination view) */
#define XBL_MODE_SPRINT "sprint"     /* reach the finish line (clear the gauntlet) */
/* Real-time 1v1: both players dodge the SAME field at the same time over a
 * direct peer-to-peer UDP channel (deterministic lockstep). Sudden death — the
 * first player hit loses. Lobby is capped at 2. See docs/multiplayer.md. */
#define XBL_MODE_LIVE_BATTLE "livebattle"
/* Single-player "3D Mode": a separate deterministic forward-runner sim (bars rush
 * toward the player in perspective). Sim-neutral runs post to "threed-<mapId>",
 * which the server verifies with its own 3D sim. See docs/single-player.md. */
#define XBL_MODE_THREED "threed"
/* Single-player "Shooter Test": a separate deterministic paintball-range sim
 * (aim a crosshair, fire at moving targets; highest points wins). Single map.
 * Sim-neutral runs post to "shooter-<mapId>", which the server verifies with its
 * own shooter sim. See docs/single-player.md. */
#define XBL_MODE_SHOOTER "shooter"
#define XBL_MODE_MAX 12

#define XBL_IP_MAX 40 /* dotted-quad IPv4 string */

typedef struct {
    char code[XBL_CODE_MAX];
    char host[XBL_USER_MAX];
    char name[XBL_LOBBY_NAME_MAX];
    int map_id;
    char map_name[XBL_MAPNAME_MAX];
    char mode[XBL_MODE_MAX]; /* "survival" | "battle" | "sprint" */
    int target;              /* finish-line frame for sprint (0 = open-ended) */
    int max_players;
    char status[16]; /* "waiting" | "playing" | "finished" */
    uint32_t seed;   /* valid once status != "waiting" */
    int player_count;
    int alive_count;            /* players still running this round */
    int win_stays;              /* Live Battle option: winner keeps the lobby */
    int friends_only;           /* lobby option: only host's friends / invitees may join */
    char winner[XBL_USER_MAX];  /* set once status == "finished" */
    XblPlayer players[XBL_MP_MAX_PLAYERS]; /* sorted by score desc */
} XblLobby;

typedef struct {
    char code[XBL_CODE_MAX];
    char host[XBL_USER_MAX];
    char name[XBL_LOBBY_NAME_MAX];
    int map_id;
    char map_name[XBL_MAPNAME_MAX];
    char mode[XBL_MODE_MAX];
    int player_count;
    int max_players;
    char status[16]; /* "waiting" | "playing" */
} XblLobbyInfo;

/* Create a lobby (you become host). `name` may be NULL. Fills `out` (may be NULL). */
int xbl_mp_create(const XblSession *s, int map_id, const char *name, XblLobby *out);
/* List open ("waiting") lobbies. Fills up to `max`; *count gets how many. Public. */
int xbl_mp_list(XblLobbyInfo *out, int max, int *count);
/* Join lobby `code`. Fills `out` (may be NULL). */
int xbl_mp_join(const XblSession *s, const char *code, XblLobby *out);
/* Leave lobby `code` (host leaving disbands it). */
int xbl_mp_leave(const XblSession *s, const char *code);
/* Return to the lobby after a round: re-arms a finished/playing lobby back to the
 * 'waiting' room so the same group can play again (and late joiners are dealt in).
 * Idempotent. Fills `out` (may be NULL) with the lobby. */
int xbl_mp_return(const XblSession *s, const char *code, XblLobby *out);
/* Poll lobby state (public). Fills `out`. Use this in the room + during the round. */
int xbl_mp_get(const char *code, XblLobby *out);
/* Authenticated poll: same as xbl_mp_get, but sends the session so the server
 * refreshes this member's liveness (so an abandoned waiting lobby can be reaped
 * while an occupied one is kept). Use this from the lobby room. Falls back to the
 * public GET when `s` has no session. Fills `out`. */
int xbl_mp_poll(const XblSession *s, const char *code, XblLobby *out);
/* Host: change the lobby map (waiting only). Fills `out` (may be NULL). */
int xbl_mp_set_map(const XblSession *s, const char *code, int map_id, XblLobby *out);
/* Host: change the game mode (waiting only). `mode` is one of XBL_MODE_*.
 * Fills `out` (may be NULL). */
int xbl_mp_set_mode(const XblSession *s, const char *code, const char *mode, XblLobby *out);
/* Host: toggle the "winner stays on" option (Live Battle; waiting only). When on,
 * the loser of a decided 1v1 is dropped and the winner keeps the lobby for the
 * next challenger. Fills `out` (may be NULL). */
int xbl_mp_set_win_stays(const XblSession *s, const char *code, int on, XblLobby *out);
/* Host: set the lobby size (2..8; waiting only). For shared-seed lobbies all
 * players run the same field; for Live Mode it caps the real-time match. Fills
 * `out` (may be NULL). */
int xbl_mp_set_max_players(const XblSession *s, const char *code, int max_players, XblLobby *out);
/* Host: toggle "friends only" (waiting only). When on, only the host's friends
 * (or players the host invited) may join. Fills `out` (may be NULL). */
int xbl_mp_set_friends_only(const XblSession *s, const char *code, int on, XblLobby *out);
/* Host: kick `username` from the lobby. They are merely disconnected and may
 * rejoin later (no ban). Fills `out` (may be NULL) with the updated lobby. */
int xbl_mp_kick(const XblSession *s, const char *code, const char *username, XblLobby *out);
/* Host: start the round. Fills `run` (run_id/nonce), *seed and *map_id with the
 * shared values every member must play. `out` (may be NULL) gets the lobby. */
int xbl_mp_start(const XblSession *s, const char *code, XblRun *run, uint32_t *seed, int *map_id,
                 XblLobby *out);
/* Fetch YOUR run for the round once status == "playing" (non-host members call
 * this after seeing the round start). Fills `run`, *seed, *map_id, `out`. */
int xbl_mp_round(const XblSession *s, const char *code, XblRun *run, uint32_t *seed, int *map_id,
                 XblLobby *out);
/* Push your live progress; fills `out` with current standings. Call ~1x/second.
 * `px` is your current player x (lets others render your live ghost). */
int xbl_mp_progress(const XblSession *s, const char *code, int frame, long long score, int alive,
                    int px, XblLobby *out);
/* Submit your final score for the round (replay-verified). *accepted set to 1 if
 * verified+stored. Fills `out` with final standings. */
int xbl_mp_finish(const XblSession *s, const char *code, const XblRun *run, long long score,
                  uint32_t seed, const char *input_log, int *accepted, XblLobby *out);

/* ---- no-code matchmaking (find / quick-match by mode) ----
 *
 * The Xbox-Live model: players join a match without typing a lobby code. Built on
 * the lobby endpoints above — useful for any homebrew game, and the basis of the
 * Test Game's Live Battle 1v1 browser. */

/* List joinable lobbies of `mode` (one of XBL_MODE_*): "waiting" rooms that still
 * have a free slot. Pass NULL/"" for `mode` to list joinable rooms of any mode.
 * Fills up to `max`; *count gets how many. Public (no session needed). */
int xbl_mp_find(const char *mode, XblLobbyInfo *out, int max, int *count);

/* Create a lobby already set to `mode` (you become host) — create + set_mode in
 * one call (e.g. host a 1v1 with XBL_MODE_LIVE_BATTLE). `name` may be NULL.
 * Fills `out` (may be NULL). */
int xbl_mp_create_mode(const XblSession *s, int map_id, const char *mode, const char *name,
                       XblLobby *out);

/* Quick match (no code): join the first open lobby of `mode`, or host a new one
 * (on `map_id`) if none are joinable. Fills `out`; *is_host (may be NULL) is set
 * to 1 when you created the room and are now waiting for an opponent. */
int xbl_mp_quick_match(const XblSession *s, const char *mode, int map_id, XblLobby *out,
                       int *is_host);

/* ---- friends + invites (see docs/friends.md) ----
 *
 * The friends list mirrors the player's Insignia friends (read-only) annotated
 * with live homebrew presence: whether each friend is online in a homebrew game
 * right now, whether it's THIS game, and a joinable lobby code if they're in one.
 * Invites let you ask a friend to join a lobby you're in; the friend gets it
 * in-game (poll xbl_invites_fetch) or on the website, accepts, and joins. */

typedef struct {
    char name[XBL_USER_MAX];                 /* friend's display name / account */
    char invite_to[XBL_USER_MAX];            /* account to address an invite to ("" if not inviteable) */
    int online;                              /* online in a homebrew game now */
    int in_this_game;                        /* that homebrew game is THIS game */
    char status[XBL_PRESENCE_STATUS_MAX];    /* online | playing | lobby (if online) */
    char game_id[XBL_GAMEID_MAX];            /* which homebrew game ("" if not online) */
    char lobby_code[XBL_CODE_MAX];           /* a joinable lobby of THIS game ("" if none) */
    int xbox_online;                         /* online on Insignia/Xbox per friends list */
    int online_any;                          /* unified online: homebrew OR Insignia */
    char playing[XBL_GAMENAME_MAX];          /* human-readable game they're in ("" if offline) */
} XblFriend;

typedef struct {
    long long id;
    char from[XBL_USER_MAX];      /* who invited you */
    char lobby_code[XBL_CODE_MAX];
    char status[16];              /* "pending" | "accepted" */
    char mode[XBL_MODE_MAX];      /* lobby mode */
    char map_name[XBL_MAPNAME_MAX];
    int map_id;
    int player_count;
    int max_players;
    int age_sec;                  /* seconds since the invite was sent */
} XblInvite;

/* GET /api/hb/friends: the player's friends + live homebrew presence (most
 * actionable first). Fills up to `max`; *count gets how many. Needs a session. */
int xbl_friends_fetch(const XblSession *s, XblFriend *out, int max, int *count);

/* POST /api/hb/invites/send: invite `to_username` to join your lobby `lobby_code`
 * (you must be a member of it). App-signed. Returns XBL_OK on success. */
int xbl_invite_send(const XblSession *s, const char *to_username, const char *lobby_code);

/* GET /api/hb/invites: your live invites (pending or accepted-elsewhere). Fills up
 * to `max`; *count gets how many. Poll this to surface invite notifications. */
int xbl_invites_fetch(const XblSession *s, XblInvite *out, int max, int *count);

/* POST /api/hb/invites/<id>/accept: accept invite `id`. On success `lobby_code_out`
 * (may be NULL) receives the lobby code to join. Session-only (works from web too). */
int xbl_invite_accept(const XblSession *s, long long id, char *lobby_code_out, size_t sz);

/* POST /api/hb/invites/<id>/decline: dismiss invite `id`. Session-only. */
int xbl_invite_decline(const XblSession *s, long long id);

/* ---- Live Battle: real-time 1v1 over peer-to-peer UDP (deterministic lockstep)
 *
 * Flow: the host sets mode to XBL_MODE_LIVE_BATTLE and starts the round (shared
 * seed). Each console opens an XblNet UDP socket, registers its endpoint with
 * the lobby (signaling), polls until it learns the peer's endpoint, then runs
 * the two-player sim locally, exchanging only per-frame inputs over UDP. After
 * the match, each console uploads its input log; the server replays the 2P sim
 * from BOTH logs and decides the verified winner (anti-cheat: the live result is
 * provisional, the server's replay is authoritative). */

typedef struct {
    int role;        /* this console's player index: 0 = host, else guest order */
    int player_count; /* N players in the round (2..8) */
    int peer_ready;  /* 1 once the endpoint(s) this console needs are registered */
    char peer_name[XBL_USER_MAX];
    char peer_ip[XBL_IP_MAX];     /* peer's reported (LAN) IP — try this first */
    char peer_wan_ip[XBL_IP_MAX]; /* peer IP as seen by the server (WAN hint) */
    int peer_port;                /* peer UDP port (host byte order) */
    uint32_t seed;                /* shared round seed */
    int map_id;
} XblNetMatch;

/* Register THIS console's UDP endpoint with the lobby and fetch match/peer info.
 * `local_ip` is where the peer should send (see xbl_net_local_ip), `local_port`
 * the bound UDP port. Fills `out`. Call once after the round starts. */
int xbl_mp_net_register(const XblSession *s, const char *code, const char *local_ip, int local_port,
                        XblNetMatch *out);

/* Poll the match/peer endpoint (the peer's endpoint appears once it registers).
 * Fills `out`. Returns XBL_OK even while out->peer_ready is still 0. */
int xbl_mp_net_poll(const XblSession *s, const char *code, XblNetMatch *out);

/* Upload your input log for a finished live battle. The server replays the 2P
 * sim once BOTH players' logs are in and decides the verified winner. `*result`
 * is set to +1 (you won), 0 (you lost), or -1 (draw / not yet decided);
 * `winner_name` (may be NULL) receives the decided winner ("" if undecided). */
int xbl_mp_battle_finish(const XblSession *s, const char *code, const XblRun *run, uint32_t seed,
                         const char *input_log, int *result, char *winner_name, size_t wnsz);

/* ---- game saves (original-Xbox UDATA format; see docs/saves.md) ----
 *
 * Writes E:\\UDATA\\<TitleID>\\<slot>\\ with a dashboard-readable SaveMeta.xbx
 * plus a raw data blob, matching how retail Xbox games store saves. Requires the
 * E: (HDD partition 1) drive to be mounted by the game first. */

#define XBL_SAVE_SLOT_MAX 16
#define XBL_SAVE_NAME_MAX 64
#define XBL_SAVE_LIST_MAX 32

typedef struct {
    char slot[XBL_SAVE_SLOT_MAX]; /* folder name under the title, e.g. "00000001" */
    char name[XBL_SAVE_NAME_MAX]; /* display name (Name= in SaveMeta.xbx) */
} XblSaveInfo;

/* Ensures E:\\UDATA\\<TitleID>\\ exists and writes TitleMeta.xbx (TitleName=...).
 * Call once after mounting E:. `title_name` shows in the dashboard. */
int xbl_save_init(const char *title_name);
/* Writes save `slot` (folder name) with display `name` and `len` bytes of `data`
 * (a SaveMeta.xbx + data.bin). Overwrites an existing slot. */
int xbl_save_write(const char *slot, const char *name, const void *data, size_t len);
/* Reads slot `slot`'s data.bin into `buf` (up to `bufsz`); *out_len gets length. */
int xbl_save_read(const char *slot, void *buf, size_t bufsz, size_t *out_len);
/* Enumerates saves under the title folder. Fills up to `max`; *count gets how many. */
int xbl_save_list(XblSaveInfo *out, int max, int *count);
/* Deletes save slot `slot` (its SaveMeta.xbx + data.bin + folder). */
int xbl_save_delete(const char *slot);

/* ---- achievements ----
 *
 * The SDK is the generic plumbing; the GAME owns the catalog and the conditions
 * (because they are game-specific). The flow is:
 *   1. xbl_ach_init(): register the catalog + a toast callback.
 *   2. xbl_ach_sync(): right after sign-in, seed the awarded set from the server's
 *      already-unlocked list so previously earned achievements never toast again.
 *   3. xbl_ach_award(id): the moment a condition is met in-game, call this. It
 *      fires the toast ONCE (instant on-screen feedback) and only if that
 *      achievement has not already been earned. Safe to call every frame.
 *   4. xbl_ach_report(): at run end, send the run (seed + input log, HMAC-signed)
 *      to the server. The server RE-SIMULATES it and grants only the achievements
 *      it can prove were earned; it returns any cumulative (account-level)
 *      achievements it just unlocked (toasted) plus the full unlocked set (used to
 *      keep the local awarded set in sync). Local awards are UX only and never
 *      trusted by the server.
 * The awarded set persists for the whole session; do NOT clear it between runs or
 * earned achievements will pop again. xbl_ach_reset() exists only for switching
 * accounts (clear, then xbl_ach_sync() the new account).
 * See docs/achievements.md.
 */

#define XBL_ACH_MAX 32
#define XBL_ACH_ID_MAX 48

typedef struct {
    const char *id;          /* stable slug, must match the server catalog */
    const char *name;        /* short display title */
    const char *description; /* one-line description */
} XblAchievementDef;

/* `def` is the matched catalog entry; fired on the UI thread from xbl_ach_award /
 * xbl_ach_report. The pointer is owned by the caller's catalog array. */
typedef void (*XblAchievementToast)(const XblAchievementDef *def, void *ud);

/* Registers the game's achievement catalog (the array must stay alive) and the
 * toast callback. Returns XBL_OK or XBL_ERR_CONFIG. */
int xbl_ach_init(const XblAchievementDef *defs, int count, XblAchievementToast on_toast, void *ud);

/* Clears the awarded set. NOT for per-run use (that re-pops earned achievements):
 * call only when switching accounts, then follow with xbl_ach_sync(). */
void xbl_ach_reset(void);

/* GET /api/hb/games/<id>/achievements with the caller's session and silently mark
 * every already-unlocked achievement as awarded (no toast), so earned ones never
 * pop up again. Call once right after sign-in. Returns XBL_OK or a negative error
 * (a failure just means the seed is skipped; nothing is toasted spuriously beyond
 * the usual once-per-earn). */
int xbl_ach_sync(const XblSession *s);

/* Marks `id` earned; fires the toast once if not already earned. Unknown ids are
 * ignored. Safe to call every frame. */
void xbl_ach_award(const char *id);

/* Returns 1 if `id` has been awarded/earned this session (or seeded by sync). */
int xbl_ach_was_awarded(const char *id);

/* POST /api/hb/achievements: reports the finished run so the server re-derives and
 * grants achievements. On success, any achievements the server reports as newly
 * unlocked are passed to the toast callback (this is how cumulative achievements,
 * which the client cannot predict, get shown). Returns XBL_OK on a definitive
 * answer, or a negative transport error. msg (optional) gets a short summary. */
int xbl_ach_report(const XblSession *s, const XblRun *run, long long score, uint32_t seed,
                   const char *input_log, char *msg, size_t msgsz);

/* ---- input log helpers ---- */

void xbl_inputlog_init(XblInputLog *l);
/* Records the held state for `frame`; only appends when the state changed from
 * the previous record (sparse). Returns XBL_OK or XBL_ERR_NOMEM. */
int xbl_inputlog_record(XblInputLog *l, int frame, int state);
const char *xbl_inputlog_str(const XblInputLog *l);
void xbl_inputlog_free(XblInputLog *l);

/* ---- crypto (also used internally) ---- */

/* Lowercase hex SHA-256 of msg[0..len). out must hold >= 65 bytes. */
int xbl_sha256_hex(const unsigned char *msg, size_t len, char *out);
/* Lowercase hex HMAC-SHA256(key, msg). out must hold >= 65 bytes. */
int xbl_hmac_sha256_hex(const char *key, const unsigned char *msg, size_t len, char *out);

#ifdef __cplusplus
}
#endif

#endif
