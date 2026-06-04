# Friends & invites

The SDK lets a game show the player's **friends** (mirrored from their Insignia
account) annotated with **live homebrew presence**, and lets players **invite**
each other into a multiplayer lobby. An invite can be accepted **in-game** or
**on the website**; accepting yields the lobby code so the Xbox joins.

This builds directly on the existing [presence](presence.md) and
[multiplayer](multiplayer.md) systems — no new identity store. A player is always
identified by their signed-in Insignia **account name** (the same key presence
and lobby membership use).

## What the friends list shows

The friends roster is **read-only** — it mirrors the player's **full** Insignia
friends list (`GET /auth/friends`), online and offline; games don't add/remove
friends. The server fetches the live list, persists it to a stored roster (so the
full list survives even if the auth API is briefly unreachable — the same roster
the website shows), and returns every friend annotated with their current status:

| Field | Meaning |
|-------|---------|
| `name` | Friend's display name (gamertag/account) |
| `online` | They're online in **a** homebrew game right now |
| `in_this_game` | That homebrew game is **this** game |
| `status` | `online` / `playing` / `lobby` (their presence hint) |
| `game_id` | Which homebrew game they're in |
| `lobby_code` | A **joinable** lobby of *this* game they're in (else empty) |
| `xbox_online` | They're online on Insignia (any game, not just homebrew) |
| `xbox_game` | The Insignia game they're playing (if known) |
| `playing` | **Human-readable game they're in right now** ("" if offline) |
| `online_any` | **Unified online**: online in homebrew **or** on Insignia |
| `invite_to` | The account name to address an invite to (only set when they're online in homebrew — the inviteable case) |

The response also carries `online_count` (friends with `online_any`). **Insignia
presence and homebrew presence are treated as one** — a friend online anywhere
shows as online. `online_any` is the field to drive that single "online" badge;
`online`/`xbox_online` are still available if you want to distinguish *where*.

`playing` is the single label to show next to an online friend: it's the homebrew
game's display name when they're in one (homebrew presence takes precedence since
that's the live state we broker), otherwise the Insignia game from the friends
list. The in-game friends list shows it as e.g. "playing Halo 2".

"Online" (homebrew) means a presence heartbeat within the 90-second window (see
[presence.md](presence.md)). Because presence is keyed by account name, a friend
is matched by comparing their Insignia identifiers (username/gamertag) against
the homebrew presence table. Insignia online status comes straight from the
friends list (`isOnline`). The Test Game colors friends in-this-game green,
in-another-homebrew-game orange, online-on-Insignia gold, and offline grey.

## Invites

An invite ties a **sender**, a **recipient**, and a **lobby code** together,
with a short time-to-live (5 minutes). The recipient sees it wherever they are:

- **In-game**: the game polls `GET /api/hb/invites`; the main menu badges
  "Friends (N invites)" and the Friends screen lists them. Accepting joins the
  lobby.
- **On the website**: the homebrew page polls the same endpoint (when signed in)
  and shows an Accept/Decline banner. Accepting marks the invite accepted and
  tells the player to open the game on their Xbox to join the lobby.

Either way the **Xbox** is what actually joins the lobby (only it can play), so
the flow is: *invite → accept (anywhere) → game joins by lobby code*.

### Rules / safety

- Sending an invite requires a valid session **and** the game's HMAC app
  signature (`invite|<game_id>`), and you must be a **member of the lobby** you
  invite to. You can't invite yourself.
- Accept/decline are **session-only** (no app secret) so the website can accept
  them too — the recipient is authenticated by their own session.
- Invites whose lobby has been disbanded are dropped from the inbox and can't be
  accepted (the accept returns `410`).
- Invites are short-lived and purged opportunistically; repeated invites from the
  same sender to the same recipient collapse into one.

There's no new anti-cheat surface here: an invite is just a pointer to a lobby.
Joining still goes through the normal lobby join, and any score played still goes
through replay verification ([anti-cheat.md](anti-cheat.md)).

## SDK API (C)

```c
/* Friends + live homebrew presence (most actionable first). Needs a session. */
int xbl_friends_fetch(const XblSession *s, XblFriend *out, int max, int *count);

/* Invite `to_username` to your lobby `lobby_code` (you must be in it). App-signed. */
int xbl_invite_send(const XblSession *s, const char *to_username, const char *lobby_code);

/* Your live invites (pending or accepted-elsewhere). Poll to badge notifications. */
int xbl_invites_fetch(const XblSession *s, XblInvite *out, int max, int *count);

/* Accept invite `id`; `lobby_code_out` gets the lobby to join. Session-only. */
int xbl_invite_accept(const XblSession *s, long long id, char *lobby_code_out, size_t sz);

/* Decline invite `id`. Session-only. */
int xbl_invite_decline(const XblSession *s, long long id);
```

Typical game flow:

1. Poll `xbl_invites_fetch` on a timer; badge the count and list them on a
   Friends screen.
2. To invite: from inside a lobby, `xbl_friends_fetch`, pick a friend with a
   non-empty `invite_to`, call `xbl_invite_send(s, friend.invite_to, code)`.
3. To accept: `xbl_invite_accept(s, id, code, sizeof code)` then
   `xbl_mp_join(s, code, &lobby)` and enter the lobby room.
4. A friend with a `lobby_code` can be joined directly with `xbl_mp_join`.

See [rest-api.md](rest-api.md) for the wire format and
[sdk-reference.md](sdk-reference.md) for the structs.

## In the Test Game

- **Main menu → Friends**: lists pending invites (gold, on top) then the friends
  roster with live status. Selecting an invite opens Accept / Decline; selecting
  a friend who's in a joinable lobby jumps straight in. The menu shows a
  "Friends (N invites)" badge when invites are waiting.
- **In a lobby, press B** to "Invite a friend": pick an online friend and an
  invite is sent.
- **On the website** ([/homebrew.html](../../forza/insignia%20stats/homebrew.html)):
  a "Game invites" banner appears when signed in, with Accept/Decline.
