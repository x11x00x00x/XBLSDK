# Online presence

Show **who** and **how many** players are currently in a homebrew game, both on
the website (the [Homebrew Leaderboards](../../forza/insignia%20stats/homebrew.html)
page) and inside the game itself.

Presence is intentionally simple and cheap: a **heartbeat**. There is no
persistent socket (the original Xbox + HTTPS makes that impractical — see
[multiplayer.md](multiplayer.md) for the same reasoning). While a signed-in
player has the game open, the client POSTs a small heartbeat every ~30s. The
server records the timestamp, and anyone whose last heartbeat is within the
**presence window** (90s) counts as online.

## Model

- One row per `(game_id, username)` in `homebrew_presence` with a `status` and
  `last_seen` timestamp. A heartbeat upserts that row.
- **Online** = `last_seen` within `window_sec` (90s). Miss ~3 heartbeats and you
  silently drop off; rows older than 10 minutes are purged.
- `status` is a coarse activity hint: `online` (in menus), `playing` (in a run),
  or `lobby` (in a multiplayer room). Unknown values become `online`.
- Several endpoints refresh presence opportunistically (`run/start`,
  `mp/progress`, `mp/finish`), so a player mid-game stays online even if the
  periodic heartbeat is skipped.

## One online session per account

A player can be signed in on several consoles/games at once (the session key is a
shared, cross-app credential — see [sdk-reference.md](sdk-reference.md)), but they
may only be **online in one place at a time**. Going online somewhere new backs
out wherever they were before.

- Each launch tags its heartbeats with a random `instance` token. A single
  `homebrew_online_session` row per `username` records the current owning instance
  (it spans every homebrew game, not just one).
- A heartbeat with `claim:true` (sent when a launch goes online — startup, or a
  deliberate take-over) becomes the owner. If the previous owner was online in a
  different game, it's dropped from that game's count immediately.
- A heartbeat with `claim:false` is a plain refresh. If a newer instance now owns
  the slot, the response carries `superseded:true` and that client stops counting
  as online (and should stop online play). It won't refresh its presence, so it
  ages out of the window naturally.
- Because a take-over only happens on an explicit `claim` (a new login), idle
  clients never ping-pong the slot back and forth.

The Test Game claims at startup, shows a "Signed in elsewhere" notice when it's
superseded, and lets the player either reclaim online play here (backing out the
other console) or quit.

## Trust

Presence can't be spoofed for someone else. `POST /api/hb/presence` requires:

1. a valid Insignia **session** (so you are who you say you are), and
2. the per-game **app HMAC** `hmac_sha256_hex(secret, "presence|" + game_id)`
   (so only an approved build of the game can report at all).

The result is "best effort, honest": a heartbeat only ever marks the calling user
online. The count reflects real clients running the game, not arbitrary API
traffic. (It is *not* a security boundary — a modified client could choose not to
report, or report `online` while idle — but it cannot inflate the count with fake
users.)

## Wire protocol

See [rest-api.md](rest-api.md#online-presence-apihbpresence) for full request/
response shapes.

| Method & path | Auth | Purpose |
|---------------|------|---------|
| `POST /api/hb/presence` | session + `presence\|game_id` HMAC | heartbeat; returns `online_count` |
| `GET /api/hb/games/:gameId/presence` | public | `{ online_count, window_sec, players[] }` |

```text
sig = hmac_sha256_hex(secret, "presence|" + game_id)
POST /api/hb/presence { game_id, status:"online", sig }   with X-Session-Key
   -> { success:true, online_count:3, window_sec:90 }
```

## SDK surface

```c
#define XBL_PRESENCE_ONLINE  "online"
#define XBL_PRESENCE_PLAYING "playing"
#define XBL_PRESENCE_LOBBY   "lobby"

typedef struct { char name[128]; char status[12]; int idle_sec; } XblPresence;

int xbl_presence_ping   (const XblSession *s, const char *status, int *online_out);
int xbl_presence_ping_ex(const XblSession *s, const char *status, int claim,
                         int *online_out, int *superseded_out);
int xbl_presence_count(int *online_out);
int xbl_presence_fetch(XblPresence *out, int max, int *count);
```

Typical use from a menu loop (the Test Game does exactly this in `game/main.c`):

```c
// Going online here (startup / re-entry): claim=1 backs out any other place.
int online = 0, superseded = 0;
xbl_presence_ping_ex(&session, XBL_PRESENCE_ONLINE, 1, &online, &superseded);

// Periodic refresh (rate-limited to ~every 25-30s): claim=0.
if (xbl_presence_ping_ex(&session, XBL_PRESENCE_ONLINE, 0, &online, &superseded) == XBL_OK) {
    if (superseded) {
        // account went online elsewhere -> show "Signed in elsewhere", stop play.
    } else {
        // draw "Online: <online>" in the UI
    }
}
// xbl_presence_ping(...) is just xbl_presence_ping_ex(..., claim=0, ..., NULL).
```

Don't call `xbl_presence_ping` every frame — a heartbeat every 25–30s is plenty
for a 90s window and keeps traffic tiny. Use `XBL_PRESENCE_PLAYING` / `_LOBBY`
when you want the status hint to reflect what the player is doing.

## On the website

`homebrew.html` shows a live "N online" badge next to the game selector (with the
first few usernames), polling `GET /api/hb/games/:gameId/presence` every 20s. The
badge is hollow/grey when nobody is online.

A player who is live in a homebrew game also shows as **online on their xb.live
profile** (and own live status), with the game name. The public profile bundle
(`GET /api/profile/:username`) and `POST /api/me/profile-live` overlay homebrew
presence onto Insignia presence: if a fresh homebrew heartbeat exists (within
~120s), `playTime.lastState` is reported as `online` and `currentGame` as the
homebrew game (e.g. "Test Game"). Insignia and homebrew presence are treated as
one — being online in Test Game lights up the same online indicator.

## Notes & limitations

- Presence is per game. `online_count` counts distinct signed-in users in the
  window; a global "now playing" across all games is available server-side via
  `hbPresenceCountAll`.
- There is no explicit "log off" — quitting the game simply stops the heartbeat
  and the player ages out of the window within `window_sec`.
- Single-session ownership lives in `homebrew_online_session` (keyed by username)
  and is enforced only when a heartbeat carries an `instance` token; clients
  without one keep the plain per-game behavior.
- The window/TTL are server constants (`PRESENCE_WINDOW_SEC`, `PRESENCE_TTL_SEC`
  in `homebrew/hb-routes.js`); tune them there.
