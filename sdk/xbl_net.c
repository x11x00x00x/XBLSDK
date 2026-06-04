/*
 * xbl_net.c - UDP datagram transport (lwIP BSD sockets) for Live Battle.
 * See xbl_net.h for the model and honest transport limits.
 */
#include "xbl_net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <lwip/inet.h>
#include <lwip/netif.h>
#include <lwip/sockets.h>

extern struct netif *g_pnetif;

int xbl_net_open(XblNet *n, uint16_t localPort)
{
    if (!n) {
        return -1;
    }
    memset(n, 0, sizeof(*n));
    n->fd = -1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(localPort);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(fd);
        return -1;
    }

    /* Read back the actual bound port when an ephemeral (0) port was asked. */
    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    if (getsockname(fd, (struct sockaddr *)&bound, &blen) == 0) {
        n->localPort = ntohs(bound.sin_port);
    } else {
        n->localPort = localPort;
    }

    /* Non-blocking so the per-frame poll never stalls the game loop. */
    int fl = lwip_fcntl(fd, F_GETFL, 0);
    if (fl < 0) {
        fl = 0;
    }
    lwip_fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    n->fd = fd;
    n->hasPeer = 0;
    return 0;
}

int xbl_net_set_peer(XblNet *n, const char *ip, uint16_t port)
{
    if (!n || n->fd < 0 || !ip) {
        return -1;
    }
    struct sockaddr_in *peer = (struct sockaddr_in *)n->peer;
    memset(peer, 0, sizeof(*peer));
    peer->sin_family = AF_INET;
    peer->sin_port = htons(port);
    if (inet_aton(ip, &peer->sin_addr) == 0) {
        return -1;
    }
    n->hasPeer = 1;
    return 0;
}

int xbl_net_send(XblNet *n, const void *buf, int len)
{
    if (!n || n->fd < 0 || !n->hasPeer || !buf || len <= 0) {
        return -1;
    }
    struct sockaddr_in *peer = (struct sockaddr_in *)n->peer;
    int r = sendto(n->fd, buf, (size_t)len, 0, (struct sockaddr *)peer, sizeof(*peer));
    return r;
}

int xbl_net_recv(XblNet *n, void *buf, int maxlen)
{
    return xbl_net_recv_from(n, buf, maxlen, NULL);
}

int xbl_net_recv_from(XblNet *n, void *buf, int maxlen, void *src16)
{
    if (!n || n->fd < 0 || !buf || maxlen <= 0) {
        return -1;
    }
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    int r = recvfrom(n->fd, buf, (size_t)maxlen, 0, (struct sockaddr *)&from, &flen);
    if (r < 0) {
        /* EWOULDBLOCK / EAGAIN -> nothing to read this poll. */
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return 0;
        }
        return -1;
    }
    if (src16) {
        memset(src16, 0, 16);
        memcpy(src16, &from, sizeof(from) < 16 ? sizeof(from) : 16);
    }
    return r;
}

int xbl_net_send_to(XblNet *n, const void *buf, int len, const void *dst16)
{
    if (!n || n->fd < 0 || !buf || len <= 0 || !dst16) {
        return -1;
    }
    struct sockaddr_in dst;
    memcpy(&dst, dst16, sizeof(dst));
    return sendto(n->fd, buf, (size_t)len, 0, (struct sockaddr *)&dst, sizeof(dst));
}

void xbl_net_close(XblNet *n)
{
    if (n && n->fd >= 0) {
        closesocket(n->fd);
        n->fd = -1;
        n->hasPeer = 0;
    }
}

int xbl_net_local_ip(char *out, size_t outsz)
{
    if (!out || outsz == 0) {
        return -1;
    }
    out[0] = '\0';
    if (!g_pnetif) {
        return -1;
    }
    const ip4_addr_t *a = netif_ip4_addr(g_pnetif);
    if (!a || a->addr == 0) {
        return -1;
    }
    char *s = ip4addr_ntoa(a);
    if (!s) {
        return -1;
    }
    snprintf(out, outsz, "%s", s);
    return 0;
}

/* ---- deterministic lockstep ---- */

#define XBL_LS_MAGIC 0x4C42424Cu /* 'LBBL' */

typedef struct {
    uint32_t magic;
    int32_t frame; /* the frame this input applies to (-1 = hello/keepalive) */
    uint16_t state;
    uint16_t pad;
} XblLsPkt;

int xbl_net_connect_peer(XblNet *n, const char *const *ips, const uint16_t *ports, int nCand,
                         int timeoutMs)
{
    if (!n || n->fd < 0 || !ips || !ports || nCand <= 0) {
        return 0;
    }
    /* Pre-resolve the candidate addresses once. */
    struct sockaddr_in cand[8];
    int nc = 0;
    for (int i = 0; i < nCand && nc < (int)(sizeof(cand) / sizeof(cand[0])); i++) {
        if (!ips[i] || !ips[i][0]) {
            continue;
        }
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(ports[i]);
        if (inet_aton(ips[i], &a.sin_addr) == 0) {
            continue;
        }
        cand[nc++] = a;
    }
    if (nc == 0) {
        return 0;
    }

    XblLsPkt hello = { XBL_LS_MAGIC, -1, 0, 0 };
    DWORD start = GetTickCount();
    while ((int)(GetTickCount() - start) < timeoutMs) {
        /* Blast a hello to every candidate so whichever path is reachable opens. */
        for (int i = 0; i < nc; i++) {
            sendto(n->fd, &hello, sizeof(hello), 0, (struct sockaddr *)&cand[i], sizeof(cand[i]));
        }
        /* Lock onto the real source address of the first reply (handles NAT
         * remapping: the port we hear back on is the one we must talk to). */
        XblLsPkt in;
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int r;
        while ((r = recvfrom(n->fd, &in, sizeof(in), 0, (struct sockaddr *)&from, &flen)) > 0) {
            if (r >= (int)sizeof(in) && in.magic == XBL_LS_MAGIC) {
                struct sockaddr_in *peer = (struct sockaddr_in *)n->peer;
                memcpy(peer, &from, sizeof(from));
                n->hasPeer = 1;
                /* Reply so the peer locks onto us too. */
                sendto(n->fd, &hello, sizeof(hello), 0, (struct sockaddr *)peer, sizeof(*peer));
                return 1;
            }
            flen = sizeof(from);
        }
        Sleep(60);
    }
    return 0;
}

int xbl_lockstep_init(XblLockstep *ls, XblNet *net, int role, int delay, int maxFrames)
{
    if (!ls || !net || delay < 0 || maxFrames <= 0) {
        return -1;
    }
    memset(ls, 0, sizeof(*ls));
    ls->net = net;
    ls->role = role ? 1 : 0;
    ls->delay = delay;
    int cap = maxFrames + delay + 1;
    ls->mine = (uint16_t *)calloc((size_t)cap, sizeof(uint16_t));
    ls->peer = (uint16_t *)calloc((size_t)cap, sizeof(uint16_t));
    ls->peerKnown = (unsigned char *)calloc((size_t)cap, 1);
    if (!ls->mine || !ls->peer || !ls->peerKnown) {
        xbl_lockstep_free(ls);
        return -1;
    }
    ls->cap = cap;
    return 0;
}

void xbl_lockstep_free(XblLockstep *ls)
{
    if (!ls) {
        return;
    }
    free(ls->mine);
    free(ls->peer);
    free(ls->peerKnown);
    ls->mine = NULL;
    ls->peer = NULL;
    ls->peerKnown = NULL;
    ls->cap = 0;
}

/* Absorb every pending datagram into the peer-input buffer. */
static void ls_drain(XblLockstep *ls)
{
    XblLsPkt in;
    int r;
    while ((r = xbl_net_recv(ls->net, &in, sizeof(in))) > 0) {
        if (r >= (int)sizeof(in) && in.magic == XBL_LS_MAGIC && in.frame >= 0 &&
            in.frame < ls->cap) {
            ls->peer[in.frame] = in.state;
            ls->peerKnown[in.frame] = 1;
        }
    }
}

int xbl_lockstep_handshake(XblLockstep *ls, int timeoutMs)
{
    if (!ls || !ls->net) {
        return 0;
    }
    DWORD start = GetTickCount();
    int got = 0;
    while ((int)(GetTickCount() - start) < timeoutMs && !got) {
        XblLsPkt hello = { XBL_LS_MAGIC, -1, 0, 0 };
        xbl_net_send(ls->net, &hello, sizeof(hello));
        /* Keep any early input packets that arrive during the handshake. */
        ls_drain(ls);
        XblLsPkt in;
        int r;
        while ((r = xbl_net_recv(ls->net, &in, sizeof(in))) > 0) {
            if (r >= (int)sizeof(in) && in.magic == XBL_LS_MAGIC) {
                got = 1;
            }
        }
        Sleep(120);
    }
    return got;
}

void xbl_lockstep_record_local(XblLockstep *ls, int frame, uint16_t state)
{
    if (!ls || !ls->mine) {
        return;
    }
    int sched = frame + ls->delay;
    if (sched >= 0 && sched < ls->cap) {
        ls->mine[sched] = state;
    }
}

void xbl_lockstep_pump(XblLockstep *ls, int frame)
{
    if (!ls || !ls->net) {
        return;
    }
    /* Send the window [frame .. frame+delay]. This both transmits the freshly
     * scheduled input (frame+delay) and retransmits the inputs the peer needs
     * for upcoming frames, giving every frame delay+1 chances against UDP loss.
     * It also guarantees the startup frames [0..delay-1] are sent (without this
     * the match would stall at frame 0, since neither side would ever transmit
     * those neutral frames). */
    int sched = frame + ls->delay;
    for (int k = 0; k <= ls->delay; k++) {
        int sf = sched - k; /* frame+delay down to frame */
        if (sf >= 0 && sf < ls->cap) {
            XblLsPkt p = { XBL_LS_MAGIC, sf, ls->mine[sf], 0 };
            xbl_net_send(ls->net, &p, sizeof(p));
        }
    }
    ls_drain(ls);
}

int xbl_lockstep_have(XblLockstep *ls, int frame)
{
    if (!ls || !ls->peerKnown || frame < 0 || frame >= ls->cap) {
        return 0;
    }
    return ls->peerKnown[frame] ? 1 : 0;
}

uint16_t xbl_lockstep_local(XblLockstep *ls, int frame)
{
    if (!ls || !ls->mine || frame < 0 || frame >= ls->cap) {
        return 0;
    }
    return ls->mine[frame];
}

void xbl_lockstep_inputs(XblLockstep *ls, int frame, uint32_t *s0, uint32_t *s1)
{
    uint32_t mine = 0, peer = 0;
    if (ls && frame >= 0 && frame < ls->cap) {
        mine = ls->mine[frame];
        peer = ls->peer[frame];
    }
    uint32_t host = (ls && ls->role == 0) ? mine : peer;
    uint32_t guest = (ls && ls->role == 0) ? peer : mine;
    if (s0) {
        *s0 = host;
    }
    if (s1) {
        *s1 = guest;
    }
}

/* ---- N-player host-relay lockstep (Live Mode, 2..8) ---- */

#define XBL_NLS_MAGIC 0x4E4C5350u /* 'NLSP' */
#define XBL_NLS_T_INPUT 0u        /* guest -> host: this guest's input */
#define XBL_NLS_T_AGG 1u          /* host -> guests: aggregated frame */
#define XBL_NLS_T_HELLO 2u        /* either way: keepalive / link prime */

typedef struct {
    uint32_t magic;
    int32_t frame; /* apply frame (-1 for hello) */
    uint8_t type;  /* XBL_NLS_T_* */
    uint8_t player; /* input: sender index; agg: playerCount */
    uint16_t n;     /* agg: number of valid state[] entries */
    uint16_t state[XBL_NET_MAX_PLAYERS];
} XblNPkt;

static int nls_index(const XblLockstepN *ls, int player, int frame)
{
    return player * ls->cap + frame;
}

int xbl_lockstep_n_init(XblLockstepN *ls, XblNet *net, int role, int playerCount, int delay,
                        int maxFrames)
{
    if (!ls || !net || delay < 0 || maxFrames <= 0) {
        return -1;
    }
    if (playerCount < 2) playerCount = 2;
    if (playerCount > XBL_NET_MAX_PLAYERS) playerCount = XBL_NET_MAX_PLAYERS;
    memset(ls, 0, sizeof(*ls));
    ls->net = net;
    ls->role = (role < 0) ? 0 : (role >= playerCount ? playerCount - 1 : role);
    ls->playerCount = playerCount;
    ls->isHost = (ls->role == 0) ? 1 : 0;
    ls->delay = delay;
    int cap = maxFrames + delay + 1;
    ls->in = (uint16_t *)calloc((size_t)cap * playerCount, sizeof(uint16_t));
    ls->known = (unsigned char *)calloc((size_t)cap * playerCount, 1);
    ls->ready = (unsigned char *)calloc((size_t)cap, 1);
    if (!ls->in || !ls->known || !ls->ready) {
        xbl_lockstep_n_free(ls);
        return -1;
    }
    ls->cap = cap;
    /* Warm-up frames [0..delay-1] are neutral and pre-known for the host's own
     * slot (the host never "receives" its own input); guests learn every frame
     * from the host's aggregate. */
    if (ls->isHost) {
        for (int f = 0; f < delay && f < cap; f++) {
            ls->known[nls_index(ls, 0, f)] = 1;
        }
    }
    return 0;
}

void xbl_lockstep_n_free(XblLockstepN *ls)
{
    if (!ls) {
        return;
    }
    free(ls->in);
    free(ls->known);
    free(ls->ready);
    ls->in = NULL;
    ls->known = NULL;
    ls->ready = NULL;
    ls->cap = 0;
}

/* Host: 1 once every player's input for `frame` is in hand. */
static int nls_host_complete(const XblLockstepN *ls, int frame)
{
    if (frame < 0 || frame >= ls->cap) {
        return 0;
    }
    for (int p = 0; p < ls->playerCount; p++) {
        if (!ls->known[nls_index(ls, p, frame)]) {
            return 0;
        }
    }
    return 1;
}

/* Drain incoming datagrams into the input buffers. */
static void nls_drain(XblLockstepN *ls)
{
    XblNPkt in;
    unsigned char src[16];
    int r;
    while ((r = xbl_net_recv_from(ls->net, &in, sizeof(in), src)) > 0) {
        if (r < (int)sizeof(in) || in.magic != XBL_NLS_MAGIC) {
            continue;
        }
        if (ls->isHost) {
            int p = in.player;
            if (p > 0 && p < ls->playerCount) {
                /* Learn / refresh this guest's real endpoint. */
                memcpy(ls->gpeer[p], src, 16);
                ls->gpeerSet[p] = 1;
            }
            if (in.type == XBL_NLS_T_INPUT && in.frame >= 0 && in.frame < ls->cap && p > 0 &&
                p < ls->playerCount) {
                ls->in[nls_index(ls, p, in.frame)] = in.state[0];
                ls->known[nls_index(ls, p, in.frame)] = 1;
            }
        } else {
            if (in.type == XBL_NLS_T_AGG && in.frame >= 0 && in.frame < ls->cap) {
                int n = in.n;
                if (n > ls->playerCount) n = ls->playerCount;
                for (int p = 0; p < n; p++) {
                    ls->in[nls_index(ls, p, in.frame)] = in.state[p];
                }
                ls->ready[in.frame] = 1;
            }
        }
    }
}

int xbl_lockstep_n_handshake(XblLockstepN *ls, int timeoutMs)
{
    if (!ls || !ls->net) {
        return 0;
    }
    DWORD start = GetTickCount();
    int ok = 0;
    while ((int)(GetTickCount() - start) < timeoutMs && !ok) {
        XblNPkt hello;
        memset(&hello, 0, sizeof(hello));
        hello.magic = XBL_NLS_MAGIC;
        hello.frame = -1;
        hello.type = XBL_NLS_T_HELLO;
        hello.player = (uint8_t)ls->role;
        if (ls->isHost) {
            /* Reply to every guest we've already heard from. */
            for (int p = 1; p < ls->playerCount; p++) {
                if (ls->gpeerSet[p]) {
                    xbl_net_send_to(ls->net, &hello, sizeof(hello), ls->gpeer[p]);
                }
            }
        } else {
            xbl_net_send(ls->net, &hello, sizeof(hello));
        }
        nls_drain(ls);
        if (ls->isHost) {
            int seen = 0;
            for (int p = 1; p < ls->playerCount; p++) {
                if (ls->gpeerSet[p]) seen++;
            }
            ok = (seen >= ls->playerCount - 1);
        } else {
            /* A guest is "linked" once it has received any aggregate/hello back;
             * we approximate that by seeing any ready frame (handshake also
             * exchanges nothing yet, so just give the link a moment). */
            ok = ls->ready[0];
        }
        Sleep(100);
    }
    return ok;
}

void xbl_lockstep_n_record_local(XblLockstepN *ls, int frame, uint16_t state)
{
    if (!ls || !ls->in) {
        return;
    }
    int sched = frame + ls->delay;
    if (sched >= 0 && sched < ls->cap) {
        ls->in[nls_index(ls, ls->role, sched)] = state;
        if (ls->isHost) {
            ls->known[nls_index(ls, 0, sched)] = 1;
        }
    }
}

void xbl_lockstep_n_pump(XblLockstepN *ls, int frame)
{
    if (!ls || !ls->net) {
        return;
    }
    int sched = frame + ls->delay;
    if (ls->isHost) {
        /* First absorb guest inputs, then relay every complete frame in the
         * window [frame .. frame+delay] to all known guests (delay+1 redundancy
         * against UDP loss). */
        nls_drain(ls);
        for (int k = ls->delay; k >= 0; k--) {
            int sf = sched - k; /* frame .. frame+delay */
            if (sf < 0 || sf >= ls->cap || !nls_host_complete(ls, sf)) {
                continue;
            }
            ls->ready[sf] = 1;
            XblNPkt agg;
            memset(&agg, 0, sizeof(agg));
            agg.magic = XBL_NLS_MAGIC;
            agg.frame = sf;
            agg.type = XBL_NLS_T_AGG;
            agg.player = (uint8_t)ls->playerCount;
            agg.n = (uint16_t)ls->playerCount;
            for (int p = 0; p < ls->playerCount; p++) {
                agg.state[p] = ls->in[nls_index(ls, p, sf)];
            }
            for (int p = 1; p < ls->playerCount; p++) {
                if (ls->gpeerSet[p]) {
                    xbl_net_send_to(ls->net, &agg, sizeof(agg), ls->gpeer[p]);
                }
            }
        }
    } else {
        /* Guest: send our scheduled inputs for the window to the host. */
        for (int k = 0; k <= ls->delay; k++) {
            int sf = sched - k;
            if (sf < 0 || sf >= ls->cap) {
                continue;
            }
            XblNPkt p;
            memset(&p, 0, sizeof(p));
            p.magic = XBL_NLS_MAGIC;
            p.frame = sf;
            p.type = XBL_NLS_T_INPUT;
            p.player = (uint8_t)ls->role;
            p.n = 1;
            p.state[0] = ls->in[nls_index(ls, ls->role, sf)];
            xbl_net_send(ls->net, &p, sizeof(p));
        }
        nls_drain(ls);
    }
}

int xbl_lockstep_n_have(XblLockstepN *ls, int frame)
{
    if (!ls || frame < 0 || frame >= ls->cap) {
        return 0;
    }
    return ls->isHost ? nls_host_complete(ls, frame) : (ls->ready[frame] ? 1 : 0);
}

void xbl_lockstep_n_inputs(XblLockstepN *ls, int frame, uint32_t *out, int max)
{
    if (!out) {
        return;
    }
    for (int p = 0; p < max; p++) {
        out[p] = 0;
    }
    if (!ls || frame < 0 || frame >= ls->cap) {
        return;
    }
    int n = ls->playerCount < max ? ls->playerCount : max;
    for (int p = 0; p < n; p++) {
        out[p] = ls->in[nls_index(ls, p, frame)];
    }
}

uint16_t xbl_lockstep_n_local(XblLockstepN *ls, int frame)
{
    if (!ls || !ls->in || frame < 0 || frame >= ls->cap) {
        return 0;
    }
    return ls->in[nls_index(ls, ls->role, frame)];
}
