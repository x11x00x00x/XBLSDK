/*
 * xbl_net.h - tiny UDP datagram transport for the xb.live Homebrew SDK.
 *
 * This is the low-latency peer-to-peer channel used by "Live Battle" (real-time
 * 1v1). The xb.live lobby is only a *signaling* broker: it pairs the two
 * consoles and hands each the other's address (see xbl_mp_net_* in xblsdk.h).
 * The actual per-frame inputs are exchanged directly between the two consoles
 * over this UDP socket (true peer-to-peer), so there is no server in the live
 * data path.
 *
 * Transport notes / honest limits:
 *   - Live Battle binds the well-known Xbox Live port 3074 (XBL_NET_LIVE_PORT) so
 *     a peer can be dialed on its public address:3074 — the port most home routers
 *     already open/forward for the Xbox. xbl_net_connect_peer() probes the peer's
 *     public + LAN endpoints at once and locks onto whichever answers (a basic
 *     simultaneous-open rendezvous), so it works on a LAN, under xemu, and over
 *     the internet when 3074 is reachable (forwarded / Open NAT / UPnP).
 *   - This is NOT a full ICE/TURN stack: strict symmetric-NAT pairs with no port
 *     forwarding can still fail to connect; a relay would be follow-up work.
 *   - UDP is lossy/unordered. The XblLockstep layer below handles resending the
 *     latest inputs every frame and tolerating reordering, so games don't have
 *     to reimplement deterministic-lockstep netcode themselves.
 */
#ifndef XBL_NET_H
#define XBL_NET_H

#include <stddef.h>
#include <stdint.h>

#define XBL_NET_IP_MAX 40 /* room for IPv4 dotted-quad (IPv6 not used here) */

/* The well-known Xbox Live UDP port. Live Battle binds this by default: it's the
 * port most home routers already open/forward for the Xbox, so dialing a peer's
 * public address on 3074 gives the best chance of a direct link over the internet
 * (the same idea retail Xbox Live uses). Falls back to an ephemeral port if 3074
 * is unavailable. */
#define XBL_NET_LIVE_PORT 3074

typedef struct {
    int fd;             /* lwIP socket, -1 when closed */
    uint16_t localPort; /* the port we are bound to (host byte order) */
    int hasPeer;        /* 1 once a peer address has been set */
    /* Opaque storage for the peer sockaddr_in (kept opaque so callers don't
     * need lwIP headers). Sized for struct sockaddr_in. */
    unsigned char peer[16];
} XblNet;

/* Opens a non-blocking UDP socket bound to `localPort` (0 = let the OS choose).
 * On success `n` is initialized and `n->localPort` holds the actual bound port.
 * Returns 0 on success, negative on error. */
int xbl_net_open(XblNet *n, uint16_t localPort);

/* Sets the datagram destination (the peer console). `ip` is a dotted-quad
 * string, `port` is host byte order. Returns 0 on success. */
int xbl_net_set_peer(XblNet *n, const char *ip, uint16_t port);

/* Establish the peer link by probing several candidate endpoints at once
 * (best-first) and locking onto the ACTUAL source address of the first reply —
 * which also tolerates NAT port remapping. `ips`/`ports` are `nCand` parallel
 * arrays (ports in host byte order; empty/NULL `ip` entries are skipped). On
 * success the peer is set (n->hasPeer) and 1 is returned; 0 on timeout. Used by
 * Live Battle so two consoles can rendezvous on their public 3074 endpoints. */
int xbl_net_connect_peer(XblNet *n, const char *const *ips, const uint16_t *ports, int nCand,
                         int timeoutMs);

/* Sends `len` bytes to the configured peer. Returns bytes sent, or negative. */
int xbl_net_send(XblNet *n, const void *buf, int len);

/* Non-blocking receive of one datagram into `buf` (up to `maxlen`). Returns the
 * number of bytes received, 0 if there was nothing to read, or negative on a
 * real error. */
int xbl_net_recv(XblNet *n, void *buf, int maxlen);

/* Like xbl_net_recv but also copies the datagram's SOURCE address into `src16`
 * (16 opaque bytes, sized for struct sockaddr_in) when non-NULL. Used by the
 * Live Mode host to learn each guest's real (NAT-translated) endpoint from the
 * packets they send, then reply to it. */
int xbl_net_recv_from(XblNet *n, void *buf, int maxlen, void *src16);

/* Sends `len` bytes to an explicit destination `dst16` (16 opaque bytes from a
 * prior xbl_net_recv_from / xbl_net_set_peer), independent of n->peer. Used by
 * the Live Mode host to relay to each guest. Returns bytes sent or negative. */
int xbl_net_send_to(XblNet *n, const void *buf, int len, const void *dst16);

/* Closes the socket (safe to call on an already-closed XblNet). */
void xbl_net_close(XblNet *n);

/* Writes this console's primary LAN IPv4 (dotted quad) into `out`. Used to tell
 * the lobby where the peer can reach us. Returns 0 on success. */
int xbl_net_local_ip(char *out, size_t outsz);

/* ---- Deterministic lockstep over an XblNet link (2 players) --------------
 *
 * This is the reusable real-time 1v1 netcode used by "Live Battle". Both
 * consoles run the SAME deterministic simulation and exchange ONLY per-frame
 * inputs. An input sampled at frame f is APPLIED at frame f + delay on both
 * sides (the delay buys time to receive the peer's input before it's needed);
 * a frame is only advanced once BOTH players' inputs for it are in hand, so the
 * two simulations can never desync. Each input is retransmitted `delay`+1 times
 * (across consecutive frames) so UDP loss is tolerated without an ACK protocol.
 *
 * It is sim-agnostic: it just delivers the ordered (player 0, player 1) input
 * for each frame; the game runs its own deterministic step with them and records
 * its own input log for server-side replay verification.
 *
 * Typical per-frame use (the game owns the loop, render, and gamepad polling):
 *
 *     xbl_lockstep_record_local(&ls, f, myInputBits);   // schedule @ f+delay
 *     xbl_lockstep_pump(&ls, f);                         // send (redundant)+recv
 *     while (!xbl_lockstep_have(&ls, f)) {               // block until peer's f
 *         xbl_lockstep_pump(&ls, f);
 *         if (user_wants_abort()) break;                 // game stays responsive
 *         Sleep(2);
 *     }
 *     uint32_t s0, s1; xbl_lockstep_inputs(&ls, f, &s0, &s1);
 *     my_sim_step(&state, s0, s1);                       // game's own sim
 */
typedef struct {
    XblNet *net;
    int role;  /* 0 = host (sim player 0), 1 = guest (player 1) */
    int delay; /* input delay in frames */
    int cap;   /* allocated frame capacity (maxFrames + delay + 1) */
    uint16_t *mine;
    uint16_t *peer;
    unsigned char *peerKnown;
} XblLockstep;

/* Initialize a lockstep session over `net` (already opened + peer set). `role`
 * is 0 for the host (sim player 0) or 1 for the guest. `delay` is the input-delay
 * frame count (2-4 is typical). `maxFrames` is the longest possible match in
 * frames. Allocates internal buffers. Returns 0 on success, negative on error. */
int xbl_lockstep_init(XblLockstep *ls, XblNet *net, int role, int delay, int maxFrames);

/* Prime the peer-to-peer link (a few hello packets / hole-punch). Returns 1 if a
 * packet was heard back from the peer within `timeoutMs`, else 0 (you may still
 * proceed; the per-frame retransmits also establish the link). */
int xbl_lockstep_handshake(XblLockstep *ls, int timeoutMs);

/* Record the local input sampled at `frame` (it is applied at frame+delay). */
void xbl_lockstep_record_local(XblLockstep *ls, int frame, uint16_t state);

/* Send our scheduled inputs for `frame` (with redundancy) and drain any
 * incoming peer datagrams. Call once per frame and again while waiting. */
void xbl_lockstep_pump(XblLockstep *ls, int frame);

/* 1 once the peer's input for `frame` has been received (ready to advance). */
int xbl_lockstep_have(XblLockstep *ls, int frame);

/* The ordered inputs to apply at `frame`: *s0 = player 0 (host), *s1 = player 1
 * (guest), regardless of which side we are. Either pointer may be NULL. */
void xbl_lockstep_inputs(XblLockstep *ls, int frame, uint32_t *s0, uint32_t *s1);

/* Our own applied input at `frame` (== mine[frame]); convenient for recording
 * the input log the server replays. */
uint16_t xbl_lockstep_local(XblLockstep *ls, int frame);

/* Free the internal buffers (safe to call twice). */
void xbl_lockstep_free(XblLockstep *ls);

/* ---- N-player host-relay lockstep (Live Mode, 2..8 players) --------------
 *
 * Generalizes the 2-player lockstep above to up to 8 players using a star
 * (host-relay) topology, which scales far better than a full mesh and stays
 * peer-to-peer (no game server in the live data path):
 *
 *   - Every console runs the SAME deterministic N-player sim and exchanges only
 *     per-frame inputs, applied at frame f + delay (same delay model as 2P).
 *   - GUESTS talk ONLY to the host: each guest sends its own per-frame input to
 *     the host and receives back an aggregated packet carrying ALL players'
 *     inputs for a frame.
 *   - The HOST collects every player's input for a frame, then broadcasts the
 *     aggregated frame to every guest. It learns each guest's real UDP endpoint
 *     from the packets they send (so it works through the guest's NAT as long as
 *     the host itself is reachable on its port — the usual home-router case).
 *
 * Player index 0 is the host; guests follow in roster order, the SAME order on
 * every console and on the server's replay, so the verified result is symmetric.
 * The N=2 case is equivalent to the 2-player lockstep. */
#define XBL_NET_MAX_PLAYERS 8

typedef struct {
    XblNet *net;
    int role;        /* this console's player index (0 = host) */
    int playerCount; /* N (2..8) */
    int isHost;      /* role == 0 */
    int delay;       /* input delay in frames */
    int cap;         /* allocated frame capacity (maxFrames + delay + 1) */
    uint16_t *in;    /* [player * cap + frame] applied input per player */
    unsigned char *known; /* host: [player*cap+frame] per-player received flag */
    unsigned char *ready; /* [frame] frame fully exchanged (host) / received (guest) */
    /* Host only: each guest's learned UDP endpoint (opaque sockaddr_in). */
    unsigned char gpeer[XBL_NET_MAX_PLAYERS][16];
    int gpeerSet[XBL_NET_MAX_PLAYERS];
} XblLockstepN;

/* Initialize an N-player lockstep session over `net`. `role` is this console's
 * player index (0 = host). `playerCount` is N (2..8). Guests must have `net`'s
 * peer set to the host; the host needs only its bound socket (it learns guests
 * from their packets). Returns 0 on success, negative on error. */
int xbl_lockstep_n_init(XblLockstepN *ls, XblNet *net, int role, int playerCount, int delay,
                        int maxFrames);

/* Prime the link: guests blast hellos at the host; the host learns and replies.
 * Returns 1 once the expected links are seen (host: all guests; guest: the
 * host), else 0 on timeout (you may still proceed; per-frame traffic re-links). */
int xbl_lockstep_n_handshake(XblLockstepN *ls, int timeoutMs);

/* Record the local input sampled at `frame` (applied at frame + delay). */
void xbl_lockstep_n_record_local(XblLockstepN *ls, int frame, uint16_t state);

/* Send/relay scheduled inputs for `frame` (with redundancy) and drain incoming
 * datagrams. Call once per frame and again while waiting on a frame. */
void xbl_lockstep_n_pump(XblLockstepN *ls, int frame);

/* 1 once every player's input for `frame` is available (ready to advance). */
int xbl_lockstep_n_have(XblLockstepN *ls, int frame);

/* Fill `out[0..playerCount-1]` with the inputs to apply at `frame` (out[0] is
 * the host). `max` bounds how many are written. */
void xbl_lockstep_n_inputs(XblLockstepN *ls, int frame, uint32_t *out, int max);

/* Our own applied input at `frame`; convenient for recording the input log the
 * server replays. */
uint16_t xbl_lockstep_n_local(XblLockstepN *ls, int frame);

/* Free the internal buffers (safe to call twice). */
void xbl_lockstep_n_free(XblLockstepN *ls);

#endif
