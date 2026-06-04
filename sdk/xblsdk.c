/*
 * xblsdk.c - xb.live Homebrew SDK implementation (original Xbox / nxdk).
 * See xblsdk.h for the public API and docs/ for the protocol.
 *
 * The auth (QR device flow + session validation) is generalized from the
 * Insignia XboxQRCodeLogin / SaveBackup apps; the HTTPS transport is the shared
 * third_party/https_client (lwIP + mbed TLS).
 */
#include "xblsdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lwip/netif.h>
#include <nxdk/net.h>
#include <windows.h>

#include "../third_party/https_client.h"
#include "../third_party/qrcodegen.h"

extern struct netif *g_pnetif;

#define HTTP_BUF 16384

static XblConfig g_cfg;
static int g_inited = 0;

/* Per-launch instance token: identifies this run when claiming the single online
 * session, so a newer login elsewhere can back this one out. */
static char g_instance[24] = { 0 };

/* ---------- small helpers ---------- */

/* Fill g_instance with a random-ish hex token, unique per launch. */
static void instance_init(void)
{
    static const char hex[] = "0123456789abcdef";
    /* Mix a few weakly-correlated sources; uniqueness (not secrecy) is the goal. */
    unsigned int seed = (unsigned int)GetTickCount();
    seed ^= (unsigned int)(uintptr_t)&seed;
    seed ^= (unsigned int)(uintptr_t)g_instance;
    for (int i = 0; i < (int)sizeof(g_instance) - 1; i++) {
        seed = seed * 1103515245u + 12345u; /* LCG step */
        g_instance[i] = hex[(seed >> 16) & 0xF];
    }
    g_instance[sizeof(g_instance) - 1] = '\0';
}

static void status(const char *msg)
{
    if (g_cfg.on_status) {
        g_cfg.on_status(msg, g_cfg.ud);
    }
}

static const char *http_body(const char *resp)
{
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : resp;
}

static int http_status_ok(const char *resp)
{
    const char *sp = strchr(resp, ' ');
    return sp && sp[1] == '2';
}

static int json_str(const char *json, const char *key, char *out, size_t outsz)
{
    char prefix[80];
    snprintf(prefix, sizeof(prefix), "\"%s\":\"", key);
    const char *p = strstr(json, prefix);
    if (!p) {
        return -1;
    }
    p += strlen(prefix);
    size_t i = 0;
    while (*p && *p != '"' && i < outsz - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

static int json_ll(const char *json, const char *key, long long *out)
{
    char prefix[80];
    snprintf(prefix, sizeof(prefix), "\"%s\":", key);
    const char *p = strstr(json, prefix);
    if (!p) {
        return -1;
    }
    p += strlen(prefix);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    *out = strtoll(p, NULL, 10);
    return 0;
}

static int json_bool_true(const char *json, const char *key)
{
    char prefix[80];
    snprintf(prefix, sizeof(prefix), "\"%s\":", key);
    const char *p = strstr(json, prefix);
    if (!p) {
        return 0;
    }
    p += strlen(prefix);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return strncmp(p, "true", 4) == 0;
}

/* ---------- lifecycle ---------- */

int xbl_init(const XblConfig *cfg)
{
    if (!cfg || !cfg->host || !cfg->port || !cfg->game_id || !cfg->game_secret ||
        !cfg->session_path) {
        return XBL_ERR_CONFIG;
    }
    g_cfg = *cfg;
    if (!g_cfg.auth_host) {
        g_cfg.auth_host = "auth.insigniastats.live";
    }
    if (!g_cfg.auth_port) {
        g_cfg.auth_port = "443";
    }
    instance_init();
    g_inited = 1;
    return XBL_OK;
}

int xbl_net_up(void)
{
    if (!g_inited) {
        return XBL_ERR_CONFIG;
    }
    nxNetInit(NULL);
    const int ticksPerSecond = 10;
    const int totalTicks = 90 * ticksPerSecond;
    for (int t = 0; t < totalTicks; t++) {
        if (g_pnetif && netif_ip4_addr(g_pnetif)->addr != 0) {
            return XBL_OK;
        }
        status("Connecting to network...");
        Sleep(1000 / ticksPerSecond);
    }
    return XBL_ERR_NET;
}

/* ---------- session storage ---------- */

int xbl_session_load(XblSession *out)
{
    if (!g_inited || !out) {
        return XBL_ERR_CONFIG;
    }
    out->session_key[0] = '\0';
    out->username[0] = '\0';
    out->logged_in = 0;

    HANDLE h = CreateFile(g_cfg.session_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return XBL_ERR_AUTH;
    }
    char buf[1024];
    DWORD read = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &read, NULL);
    CloseHandle(h);
    if (!ok || read == 0) {
        return XBL_ERR_AUTH;
    }
    buf[read] = '\0';

    char *nl = strchr(buf, '\n');
    char *line2 = NULL;
    if (nl) {
        *nl = '\0';
        line2 = nl + 1;
    }
    size_t l1 = strlen(buf);
    if (l1 && buf[l1 - 1] == '\r') {
        buf[--l1] = '\0';
    }
    if (l1 == 0) {
        return XBL_ERR_AUTH;
    }
    strncpy(out->session_key, buf, XBL_SK_MAX - 1);
    out->session_key[XBL_SK_MAX - 1] = '\0';

    if (line2) {
        char *nl2 = strchr(line2, '\n');
        if (nl2) {
            *nl2 = '\0';
        }
        size_t l2 = strlen(line2);
        if (l2 && line2[l2 - 1] == '\r') {
            line2[l2 - 1] = '\0';
        }
        strncpy(out->username, line2, XBL_USER_MAX - 1);
        out->username[XBL_USER_MAX - 1] = '\0';
    }
    out->logged_in = 1;
    return XBL_OK;
}

int xbl_session_save(const XblSession *s)
{
    if (!g_inited || !s) {
        return XBL_ERR_CONFIG;
    }
    HANDLE h = CreateFile(g_cfg.session_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return XBL_ERR_CONFIG;
    }
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "%s\n%s\n", s->session_key, s->username);
    DWORD written = 0;
    BOOL ok = (n > 0) && WriteFile(h, buf, (DWORD)n, &written, NULL) && written == (DWORD)n;
    CloseHandle(h);
    return ok ? XBL_OK : XBL_ERR_CONFIG;
}

void xbl_session_clear(void)
{
    if (g_inited) {
        DeleteFile(g_cfg.session_path);
    }
}

int xbl_session_validate(XblSession *s)
{
    if (!g_inited || !s || !s->session_key[0]) {
        return XBL_ERR_CONFIG;
    }
    static char resp[HTTP_BUF];
    char authHeader[XBL_SK_MAX + 32];
    snprintf(authHeader, sizeof(authHeader), "X-Session-Key: %s", s->session_key);
    const char *headers[] = { authHeader };

    memset(resp, 0, sizeof(resp));
    if (https_request(g_cfg.auth_host, g_cfg.auth_port, "GET", "/api/auth/user", "application/json",
                      headers, 1, NULL, 0, resp, sizeof(resp), NULL, NULL) != 0) {
        return XBL_ERR_NET;
    }
    if (!http_status_ok(resp)) {
        return XBL_ERR_AUTH;
    }
    char un[XBL_USER_MAX];
    if (json_str(http_body(resp), "username", un, sizeof(un)) == 0) {
        strncpy(s->username, un, XBL_USER_MAX - 1);
        s->username[XBL_USER_MAX - 1] = '\0';
        s->logged_in = 1;
        return XBL_OK;
    }
    return XBL_ERR_AUTH;
}

/* ---------- QR device login ---------- */

static int device_login(XblSession *out)
{
    static char resp[HTTP_BUF];

    out->session_key[0] = '\0';
    out->username[0] = '\0';
    out->logged_in = 0;

    status("Requesting login code...");
    memset(resp, 0, sizeof(resp));
    if (https_post_json(g_cfg.auth_host, g_cfg.auth_port, "/api/auth/device", "{}", resp,
                        sizeof(resp)) != 0) {
        return XBL_ERR_NET;
    }
    const char *json = http_body(resp);
    char device_code[160];
    char verification_uri[512];
    long long interval = 5;
    if (json_str(json, "device_code", device_code, sizeof(device_code)) != 0) {
        return XBL_ERR_PARSE;
    }
    if (json_str(json, "verification_uri_complete", verification_uri, sizeof(verification_uri)) !=
        0) {
        return XBL_ERR_PARSE;
    }
    json_ll(json, "interval", &interval);
    if (interval < 1) {
        interval = 5;
    }

    /* Encode + hand the QR matrix to the game's UI. */
    if (g_cfg.on_qr) {
        static uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
        static uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
        if (qrcodegen_encodeText(verification_uri, temp, qrcode, qrcodegen_Ecc_LOW, 1, 40,
                                 qrcodegen_Mask_AUTO, true)) {
            g_cfg.on_qr(verification_uri, qrcode, qrcodegen_getSize(qrcode), g_cfg.ud);
        } else {
            g_cfg.on_qr(verification_uri, NULL, 0, g_cfg.ud);
        }
    }

    for (;;) {
        Sleep((DWORD)interval * 1000);

        char body[640];
        snprintf(body, sizeof(body),
                 "{\"grant_type\":\"urn:ietf:params:oauth:grant-type:device_code\",\"device_code\":"
                 "\"%s\"}",
                 device_code);
        memset(resp, 0, sizeof(resp));
        if (https_post_json(g_cfg.auth_host, g_cfg.auth_port, "/api/auth/device/token", body, resp,
                            sizeof(resp)) != 0) {
            continue;
        }
        json = http_body(resp);

        char err[64];
        if (json_str(json, "error", err, sizeof(err)) == 0) {
            if (strcmp(err, "authorization_pending") == 0) {
                continue;
            }
            if (strstr(json, "expired")) {
                status("Login code expired.");
                return XBL_ERR_AUTH;
            }
            continue;
        }
        if (json_str(json, "sessionKey", out->session_key, XBL_SK_MAX) == 0) {
            json_str(json, "username", out->username, XBL_USER_MAX);
            out->logged_in = 1;
            if (g_cfg.on_logged_in) {
                g_cfg.on_logged_in(out->username, g_cfg.ud);
            }
            return XBL_OK;
        }
    }
}

int xbl_login(XblSession *out)
{
    if (!g_inited || !out) {
        return XBL_ERR_CONFIG;
    }

    /* Try a saved session first. */
    if (xbl_session_load(out) == XBL_OK) {
        status("Checking saved login...");
        if (xbl_session_validate(out) == XBL_OK) {
            xbl_account_remember(out);
            if (g_cfg.on_logged_in) {
                g_cfg.on_logged_in(out->username, g_cfg.ud);
            }
            return XBL_OK;
        }
        xbl_session_clear();
    }

    int r = device_login(out);
    if (r == XBL_OK) {
        xbl_session_save(out);
        xbl_account_remember(out);
    }
    return r;
}

/* ---------- multiple accounts (login picker) ---------- */

/* Path to the accounts store. By default this is the SHARED, cross-app store
 * (XBL_SHARED_ACCOUNTS_PATH) so a single Insignia sign-in is reusable by every
 * SDK app; XblConfig.accounts_path overrides it for per-app isolation. */
const char *xbl_accounts_store_path(void)
{
    if (g_cfg.accounts_path && g_cfg.accounts_path[0]) {
        return g_cfg.accounts_path;
    }
    return XBL_SHARED_ACCOUNTS_PATH;
}

static const char *accounts_path(void)
{
    return xbl_accounts_store_path();
}

/* Best-effort create of the directory that holds `path` (Xbox CreateFile won't
 * make parent dirs). Harmless if it already exists. */
static void ensure_parent_dir(const char *path)
{
    if (!path || !path[0]) {
        return;
    }
    char dir[260];
    int cut = -1;
    for (int i = 0; path[i] && i < (int)sizeof(dir) - 1; i++) {
        if (path[i] == '\\' || path[i] == '/') {
            cut = i;
        }
    }
    if (cut <= 0) {
        return; /* no directory component (or drive root) */
    }
    memcpy(dir, path, cut);
    dir[cut] = '\0';
    CreateDirectory(dir, NULL);
}

static int accounts_write(const XblAccount *acc, int n)
{
    ensure_parent_dir(accounts_path());
    HANDLE h = CreateFile(accounts_path(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return XBL_ERR_CONFIG;
    }
    BOOL ok = TRUE;
    for (int i = 0; i < n && ok; i++) {
        if (!acc[i].username[0] || !acc[i].session_key[0]) {
            continue;
        }
        char line[XBL_USER_MAX + XBL_SK_MAX + 4];
        int m = snprintf(line, sizeof(line), "%s\t%s\n", acc[i].username, acc[i].session_key);
        DWORD w = 0;
        ok = (m > 0) && WriteFile(h, line, (DWORD)m, &w, NULL) && w == (DWORD)m;
    }
    CloseHandle(h);
    return ok ? XBL_OK : XBL_ERR_CONFIG;
}

int xbl_accounts_load(XblAccount *out, int max, int *count)
{
    if (!g_inited || !out || max <= 0) {
        return XBL_ERR_CONFIG;
    }
    if (count) {
        *count = 0;
    }
    HANDLE h = CreateFile(accounts_path(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return XBL_ERR_AUTH; /* no accounts remembered yet */
    }
    static char buf[(XBL_USER_MAX + XBL_SK_MAX + 4) * XBL_ACCOUNTS_MAX + 16];
    DWORD read = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &read, NULL);
    CloseHandle(h);
    if (!ok || read == 0) {
        return XBL_ERR_AUTH;
    }
    buf[read] = '\0';

    int n = 0;
    char *line = buf;
    while (line && *line && n < max) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') {
            line[--ll] = '\0';
        }
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = '\0';
            const char *user = line;
            const char *key = tab + 1;
            if (user[0] && key[0]) {
                strncpy(out[n].username, user, XBL_USER_MAX - 1);
                out[n].username[XBL_USER_MAX - 1] = '\0';
                strncpy(out[n].session_key, key, XBL_SK_MAX - 1);
                out[n].session_key[XBL_SK_MAX - 1] = '\0';
                n++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    if (count) {
        *count = n;
    }
    return n > 0 ? XBL_OK : XBL_ERR_AUTH;
}

int xbl_account_remember(const XblSession *s)
{
    if (!g_inited || !s || !s->session_key[0] || !s->username[0]) {
        return XBL_ERR_CONFIG;
    }
    XblAccount prev[XBL_ACCOUNTS_MAX];
    int n = 0;
    xbl_accounts_load(prev, XBL_ACCOUNTS_MAX, &n); /* ok if empty (n stays 0) */

    XblAccount list[XBL_ACCOUNTS_MAX];
    /* New/updated account goes to the front. */
    strncpy(list[0].username, s->username, XBL_USER_MAX - 1);
    list[0].username[XBL_USER_MAX - 1] = '\0';
    strncpy(list[0].session_key, s->session_key, XBL_SK_MAX - 1);
    list[0].session_key[XBL_SK_MAX - 1] = '\0';
    int w = 1;
    for (int i = 0; i < n && w < XBL_ACCOUNTS_MAX; i++) {
        if (strcmp(prev[i].username, s->username) == 0) {
            continue; /* de-dupe: keep only the fresh copy at the front */
        }
        list[w++] = prev[i];
    }
    return accounts_write(list, w);
}

int xbl_account_forget(const char *username)
{
    if (!g_inited || !username || !username[0]) {
        return XBL_ERR_CONFIG;
    }
    XblAccount prev[XBL_ACCOUNTS_MAX];
    int n = 0;
    if (xbl_accounts_load(prev, XBL_ACCOUNTS_MAX, &n) != XBL_OK) {
        return XBL_OK; /* nothing to forget */
    }
    XblAccount list[XBL_ACCOUNTS_MAX];
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(prev[i].username, username) != 0) {
            list[w++] = prev[i];
        }
    }
    if (w == n) {
        return XBL_OK; /* not found */
    }
    if (w == 0) {
        DeleteFile(accounts_path());
        return XBL_OK;
    }
    return accounts_write(list, w);
}

int xbl_session_resume(const XblAccount *a, XblSession *out)
{
    if (!g_inited || !a || !out || !a->session_key[0]) {
        return XBL_ERR_CONFIG;
    }
    memset(out, 0, sizeof(*out));
    strncpy(out->session_key, a->session_key, XBL_SK_MAX - 1);
    out->session_key[XBL_SK_MAX - 1] = '\0';
    strncpy(out->username, a->username, XBL_USER_MAX - 1);
    out->username[XBL_USER_MAX - 1] = '\0';
    out->logged_in = 1;

    status("Checking saved login...");
    int r = xbl_session_validate(out);
    if (r != XBL_OK) {
        return r; /* XBL_ERR_AUTH = stale key; transport errors bubble up */
    }
    xbl_session_save(out);     /* this account becomes the active session */
    xbl_account_remember(out); /* refresh + bump to front */
    if (g_cfg.on_logged_in) {
        g_cfg.on_logged_in(out->username, g_cfg.ud);
    }
    return XBL_OK;
}

int xbl_login_new(XblSession *out)
{
    if (!g_inited || !out) {
        return XBL_ERR_CONFIG;
    }
    int r = device_login(out);
    if (r == XBL_OK) {
        xbl_session_save(out);
        xbl_account_remember(out);
    }
    return r;
}

/* ---------- runs + scores ---------- */

int xbl_run_begin_map(const XblSession *s, int map_id, XblRun *out)
{
    if (!g_inited || !s || !out || !s->session_key[0]) {
        return XBL_ERR_CONFIG;
    }
    memset(out, 0, sizeof(*out));

    char sigmsg[256];
    snprintf(sigmsg, sizeof(sigmsg), "start|%s", g_cfg.game_id);
    char sig[65];
    if (xbl_hmac_sha256_hex(g_cfg.game_secret, (const unsigned char *)sigmsg, strlen(sigmsg),
                            sig) != XBL_OK) {
        return XBL_ERR_CONFIG;
    }

    char body[512];
    snprintf(body, sizeof(body), "{\"game_id\":\"%s\",\"map_id\":%d,\"sig\":\"%s\"}", g_cfg.game_id,
             map_id, sig);

    char skHeader[XBL_SK_MAX + 32];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", s->session_key);
    const char *headers[] = { skHeader };

    static char resp[HTTP_BUF];
    memset(resp, 0, sizeof(resp));
    if (https_request(g_cfg.host, g_cfg.port, "POST", "/api/hb/run/start", "application/json",
                      headers, 1, body, strlen(body), resp, sizeof(resp), NULL, NULL) != 0) {
        return XBL_ERR_NET;
    }
    if (!http_status_ok(resp)) {
        return XBL_ERR_HTTP;
    }
    const char *json = http_body(resp);
    if (json_str(json, "run_id", out->run_id, XBL_RUNID_MAX) != 0 ||
        json_str(json, "nonce", out->nonce, XBL_NONCE_MAX) != 0) {
        return XBL_ERR_PARSE;
    }
    long long seed = 0, st = 0, mid = 0;
    json_ll(json, "seed", &seed);
    json_ll(json, "server_time", &st);
    json_ll(json, "map_id", &mid);
    out->seed = (uint32_t)seed;
    out->map_id = (int)mid;
    out->server_time = (unsigned long long)st;
    return XBL_OK;
}

int xbl_run_begin(const XblSession *s, XblRun *out)
{
    return xbl_run_begin_map(s, 0, out);
}

int xbl_score_submit(const XblSession *s, const XblRun *run, const char *board_id, long long score,
                     uint32_t seed, const char *input_log, int *accepted, char *msg, size_t msgsz)
{
    if (!g_inited || !s || !run || !board_id) {
        return XBL_ERR_CONFIG;
    }
    if (accepted) {
        *accepted = 0;
    }
    if (msg && msgsz) {
        msg[0] = '\0';
    }
    const char *log = input_log ? input_log : "";

    char input_hash[65];
    if (xbl_sha256_hex((const unsigned char *)log, strlen(log), input_hash) != XBL_OK) {
        return XBL_ERR_CONFIG;
    }

    char sigmsg[512];
    snprintf(sigmsg, sizeof(sigmsg), "score|%s|%s|%s|%s|%lld|%u|%s", g_cfg.game_id, run->run_id,
             run->nonce, board_id, score, seed, input_hash);
    char sig[65];
    if (xbl_hmac_sha256_hex(g_cfg.game_secret, (const unsigned char *)sigmsg, strlen(sigmsg),
                            sig) != XBL_OK) {
        return XBL_ERR_CONFIG;
    }

    size_t cap = strlen(log) + 768;
    char *body = (char *)malloc(cap);
    if (!body) {
        return XBL_ERR_NOMEM;
    }
    int n = snprintf(body, cap,
                     "{\"game_id\":\"%s\",\"run_id\":\"%s\",\"nonce\":\"%s\",\"board_id\":\"%s\","
                     "\"score\":%lld,\"seed\":%u,\"input_hash\":\"%s\",\"sig\":\"%s\","
                     "\"input_log\":\"%s\"}",
                     g_cfg.game_id, run->run_id, run->nonce, board_id, score, seed, input_hash, sig,
                     log);
    if (n < 0 || (size_t)n >= cap) {
        free(body);
        return XBL_ERR_NOMEM;
    }

    char skHeader[XBL_SK_MAX + 32];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", s->session_key);
    const char *headers[] = { skHeader };

    static char resp[HTTP_BUF];
    memset(resp, 0, sizeof(resp));
    int r = https_request(g_cfg.host, g_cfg.port, "POST", "/api/hb/score", "application/json",
                          headers, 1, body, (size_t)n, resp, sizeof(resp), NULL, NULL);
    free(body);
    if (r != 0) {
        return XBL_ERR_NET;
    }

    const char *json = http_body(resp);
    if (http_status_ok(resp)) {
        if (accepted) {
            *accepted = 1;
        }
        if (msg && msgsz) {
            char m[XBL_MSG_MAX];
            if (json_str(json, "message", m, sizeof(m)) == 0) {
                strncpy(msg, m, msgsz - 1);
                msg[msgsz - 1] = '\0';
            }
        }
        return XBL_OK;
    }

    /* Non-2xx: a definitive rejection (verification/anomaly) if 4xx. */
    if (msg && msgsz) {
        char m[XBL_MSG_MAX];
        if (json_str(json, "error", m, sizeof(m)) == 0 ||
            json_str(json, "message", m, sizeof(m)) == 0) {
            strncpy(msg, m, msgsz - 1);
            msg[msgsz - 1] = '\0';
        }
    }
    const char *sp = strchr(resp, ' ');
    if (sp && sp[1] == '4') {
        return XBL_ERR_REJECTED;
    }
    return XBL_ERR_HTTP;
}

/* ---------- leaderboard ---------- */

int xbl_leaderboard_fetch(const char *board_id, int top_n, XblEntry *out, int *count)
{
    if (!g_inited || !board_id || !out || top_n <= 0) {
        return XBL_ERR_CONFIG;
    }
    if (count) {
        *count = 0;
    }

    char path[256];
    snprintf(path, sizeof(path), "/api/hb/leaderboard/%s/%s?limit=%d", g_cfg.game_id, board_id,
             top_n);

    static char resp[HTTP_BUF];
    memset(resp, 0, sizeof(resp));
    if (https_request(g_cfg.host, g_cfg.port, "GET", path, "application/json", NULL, 0, NULL, 0,
                      resp, sizeof(resp), NULL, NULL) != 0) {
        return XBL_ERR_NET;
    }
    if (!http_status_ok(resp)) {
        return XBL_ERR_HTTP;
    }

    const char *p = strstr(http_body(resp), "\"entries\"");
    if (!p) {
        return XBL_ERR_PARSE;
    }
    p = strchr(p, '[');
    if (!p) {
        return XBL_ERR_PARSE;
    }
    p++;

    int n = 0;
    while (n < top_n) {
        const char *obj = strchr(p, '{');
        if (!obj) {
            break;
        }
        const char *end = strchr(obj, '}');
        if (!end) {
            break;
        }
        size_t objlen = (size_t)(end - obj) + 1;
        char buf[512];
        if (objlen >= sizeof(buf)) {
            objlen = sizeof(buf) - 1;
        }
        memcpy(buf, obj, objlen);
        buf[objlen] = '\0';

        long long rank = 0, sc = 0;
        json_ll(buf, "rank", &rank);
        json_ll(buf, "score", &sc);
        out[n].rank = (int)rank;
        out[n].score = sc;
        out[n].verified = json_bool_true(buf, "verified");
        out[n].name[0] = '\0';
        json_str(buf, "name", out[n].name, XBL_USER_MAX);
        n++;
        p = end + 1;
    }
    if (count) {
        *count = n;
    }
    return XBL_OK;
}

/* ---------- achievements ---------- */

static const XblAchievementDef *g_ach_defs = NULL;
static int g_ach_count = 0;
static XblAchievementToast g_ach_toast = NULL;
static void *g_ach_ud = NULL;
static unsigned char g_ach_awarded[XBL_ACH_MAX];

static int ach_index(const char *id)
{
    if (!id) {
        return -1;
    }
    for (int i = 0; i < g_ach_count; i++) {
        if (g_ach_defs[i].id && strcmp(g_ach_defs[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

int xbl_ach_init(const XblAchievementDef *defs, int count, XblAchievementToast on_toast, void *ud)
{
    if (!defs || count < 0) {
        return XBL_ERR_CONFIG;
    }
    if (count > XBL_ACH_MAX) {
        count = XBL_ACH_MAX;
    }
    g_ach_defs = defs;
    g_ach_count = count;
    g_ach_toast = on_toast;
    g_ach_ud = ud;
    memset(g_ach_awarded, 0, sizeof(g_ach_awarded));
    return XBL_OK;
}

void xbl_ach_reset(void)
{
    memset(g_ach_awarded, 0, sizeof(g_ach_awarded));
}

void xbl_ach_award(const char *id)
{
    int i = ach_index(id);
    if (i < 0 || g_ach_awarded[i]) {
        return;
    }
    g_ach_awarded[i] = 1;
    if (g_ach_toast) {
        g_ach_toast(&g_ach_defs[i], g_ach_ud);
    }
}

int xbl_ach_was_awarded(const char *id)
{
    int i = ach_index(id);
    return (i >= 0 && g_ach_awarded[i]) ? 1 : 0;
}

/* Mark an id as awarded WITHOUT firing a toast. Used to seed the awarded set
 * from the server's already-unlocked list so previously earned achievements
 * never pop up again on a later run or launch. */
static void ach_mark_silent(const char *id)
{
    int i = ach_index(id);
    if (i >= 0) {
        g_ach_awarded[i] = 1;
    }
}

/* Marks every id inside a response's "all":[ "id", ... ] string array as awarded
 * silently (the server's full unlocked set for the caller). */
static void ach_mark_all_silent(const char *json)
{
    const char *p = strstr(json, "\"all\"");
    if (!p) {
        return;
    }
    const char *arr = strchr(p, '[');
    if (!arr) {
        return;
    }
    const char *end = strchr(arr, ']');
    if (!end) {
        return;
    }
    const char *cur = arr;
    while (cur < end) {
        const char *q = strchr(cur, '"');
        if (!q || q > end) {
            break;
        }
        q++;
        char id[XBL_ACH_ID_MAX];
        size_t k = 0;
        while (q < end && *q && *q != '"' && k < sizeof(id) - 1) {
            id[k++] = *q++;
        }
        id[k] = '\0';
        if (id[0]) {
            ach_mark_silent(id);
        }
        cur = q + 1;
    }
}

/* Toasts each "id" found inside the response's "unlocked":[...] array. Reusing
 * xbl_ach_award means anything already toasted live this run won't toast twice,
 * while server-only (cumulative) unlocks get shown for the first time. */
static void ach_toast_unlocked(const char *json)
{
    const char *p = strstr(json, "\"unlocked\"");
    if (!p) {
        return;
    }
    const char *arr = strchr(p, '[');
    if (!arr) {
        return;
    }
    const char *end = strchr(arr, ']');
    if (!end) {
        return;
    }
    const char *cur = arr;
    while (cur < end) {
        const char *idp = strstr(cur, "\"id\":\"");
        if (!idp || idp > end) {
            break;
        }
        idp += 6;
        char id[XBL_ACH_ID_MAX];
        size_t k = 0;
        while (idp < end && *idp && *idp != '"' && k < sizeof(id) - 1) {
            id[k++] = *idp++;
        }
        id[k] = '\0';
        if (id[0]) {
            xbl_ach_award(id);
        }
        cur = idp;
    }
}

int xbl_ach_report(const XblSession *s, const XblRun *run, long long score, uint32_t seed,
                   const char *input_log, char *msg, size_t msgsz)
{
    (void)score;
    if (!g_inited || !s || !run) {
        return XBL_ERR_CONFIG;
    }
    if (msg && msgsz) {
        msg[0] = '\0';
    }
    if (g_ach_count == 0) {
        return XBL_OK; /* no catalog registered; nothing to report */
    }
    const char *log = input_log ? input_log : "";

    char input_hash[65];
    if (xbl_sha256_hex((const unsigned char *)log, strlen(log), input_hash) != XBL_OK) {
        return XBL_ERR_CONFIG;
    }

    char sigmsg[512];
    snprintf(sigmsg, sizeof(sigmsg), "ach|%s|%s|%u|%s", g_cfg.game_id, run->run_id, seed,
             input_hash);
    char sig[65];
    if (xbl_hmac_sha256_hex(g_cfg.game_secret, (const unsigned char *)sigmsg, strlen(sigmsg),
                            sig) != XBL_OK) {
        return XBL_ERR_CONFIG;
    }

    size_t cap = strlen(log) + 640;
    char *body = (char *)malloc(cap);
    if (!body) {
        return XBL_ERR_NOMEM;
    }
    int n = snprintf(body, cap,
                     "{\"game_id\":\"%s\",\"run_id\":\"%s\",\"seed\":%u,\"input_hash\":\"%s\","
                     "\"sig\":\"%s\",\"input_log\":\"%s\"}",
                     g_cfg.game_id, run->run_id, seed, input_hash, sig, log);
    if (n < 0 || (size_t)n >= cap) {
        free(body);
        return XBL_ERR_NOMEM;
    }

    char skHeader[XBL_SK_MAX + 32];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", s->session_key);
    const char *headers[] = { skHeader };

    static char resp[HTTP_BUF];
    memset(resp, 0, sizeof(resp));
    int r = https_request(g_cfg.host, g_cfg.port, "POST", "/api/hb/achievements", "application/json",
                          headers, 1, body, (size_t)n, resp, sizeof(resp), NULL, NULL);
    free(body);
    if (r != 0) {
        return XBL_ERR_NET;
    }
    if (!http_status_ok(resp)) {
        if (msg && msgsz) {
            char m[XBL_MSG_MAX];
            if (json_str(http_body(resp), "error", m, sizeof(m)) == 0) {
                strncpy(msg, m, msgsz - 1);
                msg[msgsz - 1] = '\0';
            }
        }
        return XBL_ERR_HTTP;
    }

    ach_toast_unlocked(http_body(resp));
    /* Keep the local awarded set in lockstep with the server's full unlocked set
     * so nothing already earned can toast again on a later run. */
    ach_mark_all_silent(http_body(resp));
    if (msg && msgsz) {
        strncpy(msg, "Achievements synced", msgsz - 1);
        msg[msgsz - 1] = '\0';
    }
    return XBL_OK;
}

int xbl_ach_sync(const XblSession *s)
{
    if (!g_inited || !s) {
        return XBL_ERR_CONFIG;
    }
    if (g_ach_count == 0) {
        return XBL_OK;
    }
    char path[160];
    snprintf(path, sizeof(path), "/api/hb/games/%s/achievements", g_cfg.game_id);
    char skHeader[XBL_SK_MAX + 32];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", s->session_key);
    const char *headers[] = { skHeader };

    static char resp[HTTP_BUF];
    memset(resp, 0, sizeof(resp));
    if (https_request(g_cfg.host, g_cfg.port, "GET", path, "application/json", headers, 1, NULL, 0,
                      resp, sizeof(resp), NULL, NULL) != 0) {
        return XBL_ERR_NET;
    }
    if (!http_status_ok(resp)) {
        return XBL_ERR_HTTP;
    }

    /* Walk the "achievements":[ { "id":"..","unlocked":true/false,.. }, .. ] list
     * and silently mark every already-unlocked entry so it can't toast again. */
    const char *body = http_body(resp);
    const char *arr = strstr(body, "\"achievements\"");
    if (!arr) {
        return XBL_OK;
    }
    const char *cur = strchr(arr, '[');
    if (!cur) {
        return XBL_OK;
    }
    for (;;) {
        const char *idp = strstr(cur, "\"id\":\"");
        if (!idp) {
            break;
        }
        idp += 6;
        char id[XBL_ACH_ID_MAX];
        size_t k = 0;
        while (*idp && *idp != '"' && k < sizeof(id) - 1) {
            id[k++] = *idp++;
        }
        id[k] = '\0';
        /* The unlocked flag belongs to this object only if it appears before the
         * next "id" (and "unlocked_count" never matches "unlocked":). */
        const char *nextId = strstr(idp, "\"id\":\"");
        const char *unl = strstr(idp, "\"unlocked\":");
        if (id[0] && unl && (!nextId || unl < nextId)) {
            unl += 11; /* past "unlocked": */
            while (*unl == ' ') {
                unl++;
            }
            if (strncmp(unl, "true", 4) == 0) {
                ach_mark_silent(id);
            }
        }
        cur = idp;
    }
    return XBL_OK;
}

/* ---------- input log ---------- */

void xbl_inputlog_init(XblInputLog *l)
{
    l->buf = NULL;
    l->len = 0;
    l->cap = 0;
    l->last_state = -1;
    l->count = 0;
}

int xbl_inputlog_record(XblInputLog *l, int frame, int state)
{
    if (!l) {
        return XBL_ERR_CONFIG;
    }
    /* Keep all defined input bits (the dodgers + 3D runner set only LEFT/RIGHT).
     * Masking to the full set is backward compatible. Shooter Test is a
     * trust-based free-roam FPS and does NOT use this log at all. */
    state &= XBL_IN_MASK;
    if (state == l->last_state) {
        return XBL_OK; /* sparse: only record changes */
    }
    char piece[32];
    int pn = snprintf(piece, sizeof(piece), "%s%d:%d", l->count ? "," : "", frame, state);
    if (pn < 0) {
        return XBL_ERR_NOMEM;
    }
    if (l->len + (size_t)pn + 1 > l->cap) {
        size_t newcap = l->cap ? l->cap * 2 : 256;
        while (newcap < l->len + (size_t)pn + 1) {
            newcap *= 2;
        }
        char *grown = (char *)realloc(l->buf, newcap);
        if (!grown) {
            return XBL_ERR_NOMEM;
        }
        l->buf = grown;
        l->cap = newcap;
    }
    memcpy(l->buf + l->len, piece, (size_t)pn);
    l->len += (size_t)pn;
    l->buf[l->len] = '\0';
    l->last_state = state;
    l->count++;
    return XBL_OK;
}

const char *xbl_inputlog_str(const XblInputLog *l)
{
    return (l && l->buf) ? l->buf : "";
}

void xbl_inputlog_free(XblInputLog *l)
{
    if (l && l->buf) {
        free(l->buf);
    }
    if (l) {
        l->buf = NULL;
        l->len = 0;
        l->cap = 0;
        l->count = 0;
        l->last_state = -1;
    }
}

/* ---------- maps ---------- */

int xbl_maps_fetch(XblMapInfo *out, int max, int *count)
{
    if (!g_inited || !out || max <= 0) {
        return XBL_ERR_CONFIG;
    }
    if (count) {
        *count = 0;
    }
    char path[256];
    snprintf(path, sizeof(path), "/api/hb/games/%s/maps", g_cfg.game_id);

    static char resp[HTTP_BUF];
    memset(resp, 0, sizeof(resp));
    if (https_request(g_cfg.host, g_cfg.port, "GET", path, "application/json", NULL, 0, NULL, 0, resp,
                      sizeof(resp), NULL, NULL) != 0) {
        return XBL_ERR_NET;
    }
    if (!http_status_ok(resp)) {
        return XBL_ERR_HTTP;
    }
    const char *p = strstr(http_body(resp), "\"maps\"");
    if (!p || !(p = strchr(p, '['))) {
        return XBL_ERR_PARSE;
    }
    p++;
    int n = 0;
    while (n < max) {
        const char *obj = strchr(p, '{');
        if (!obj) {
            break;
        }
        const char *end = strchr(obj, '}');
        if (!end) {
            break;
        }
        size_t objlen = (size_t)(end - obj) + 1;
        char buf[160];
        if (objlen >= sizeof(buf)) {
            objlen = sizeof(buf) - 1;
        }
        memcpy(buf, obj, objlen);
        buf[objlen] = '\0';
        long long mid = 0;
        json_ll(buf, "map_id", &mid);
        out[n].map_id = (int)mid;
        out[n].name[0] = '\0';
        json_str(buf, "name", out[n].name, XBL_MAPNAME_MAX);
        n++;
        p = end + 1;
    }
    if (count) {
        *count = n;
    }
    return XBL_OK;
}

/* ---------- online presence ---------- */

int xbl_presence_ping_ex(const XblSession *s, const char *status, int claim, int *online_out,
                         int *superseded_out)
{
    if (online_out) {
        *online_out = 0;
    }
    if (superseded_out) {
        *superseded_out = 0;
    }
    if (!g_inited || !s || !s->session_key[0]) {
        return XBL_ERR_CONFIG;
    }
    char sig[65];
    char sigmsg[160];
    snprintf(sigmsg, sizeof(sigmsg), "presence|%s", g_cfg.game_id);
    if (xbl_hmac_sha256_hex(g_cfg.game_secret, (const unsigned char *)sigmsg, strlen(sigmsg), sig) !=
        XBL_OK) {
        return XBL_ERR_CONFIG;
    }
    /* `instance` identifies this launch; `claim` means "make me the online session
     * now" (a new login here backs out any other place). */
    char body[320];
    snprintf(body, sizeof(body),
             "{\"game_id\":\"%s\",\"status\":\"%s\",\"instance\":\"%s\",\"claim\":%s,\"sig\":\"%s\"}",
             g_cfg.game_id, status && status[0] ? status : "online", g_instance,
             claim ? "true" : "false", sig);

    char skHeader[XBL_SK_MAX + 32];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", s->session_key);
    const char *headers[] = { skHeader };

    static char resp[HTTP_BUF];
    memset(resp, 0, sizeof(resp));
    if (https_request(g_cfg.host, g_cfg.port, "POST", "/api/hb/presence", "application/json", headers,
                      1, body, strlen(body), resp, sizeof(resp), NULL, NULL) != 0) {
        return XBL_ERR_NET;
    }
    if (!http_status_ok(resp)) {
        return XBL_ERR_HTTP;
    }
    const char *bdy = http_body(resp);
    if (online_out) {
        long long n = 0;
        json_ll(bdy, "online_count", &n);
        *online_out = (int)n;
    }
    if (superseded_out) {
        *superseded_out = json_bool_true(bdy, "superseded");
    }
    return XBL_OK;
}

int xbl_presence_ping(const XblSession *s, const char *status, int *online_out)
{
    return xbl_presence_ping_ex(s, status, 0, online_out, NULL);
}

/* Shared GET of /api/hb/games/<id>/presence into the static response buffer. */
static int presence_get(char *resp, size_t resplen)
{
    char path[256];
    snprintf(path, sizeof(path), "/api/hb/games/%s/presence", g_cfg.game_id);
    if (https_request(g_cfg.host, g_cfg.port, "GET", path, "application/json", NULL, 0, NULL, 0, resp,
                      resplen, NULL, NULL) != 0) {
        return XBL_ERR_NET;
    }
    if (!http_status_ok(resp)) {
        return XBL_ERR_HTTP;
    }
    return XBL_OK;
}

int xbl_presence_count(int *online_out)
{
    if (online_out) {
        *online_out = 0;
    }
    if (!g_inited) {
        return XBL_ERR_CONFIG;
    }
    static char resp[HTTP_BUF];
    memset(resp, 0, sizeof(resp));
    int rc = presence_get(resp, sizeof(resp));
    if (rc != XBL_OK) {
        return rc;
    }
    long long n = 0;
    json_ll(http_body(resp), "online_count", &n);
    if (online_out) {
        *online_out = (int)n;
    }
    return XBL_OK;
}

int xbl_presence_fetch(XblPresence *out, int max, int *count)
{
    if (count) {
        *count = 0;
    }
    if (!g_inited || !out || max <= 0) {
        return XBL_ERR_CONFIG;
    }
    static char resp[HTTP_BUF];
    memset(resp, 0, sizeof(resp));
    int rc = presence_get(resp, sizeof(resp));
    if (rc != XBL_OK) {
        return rc;
    }
    const char *p = strstr(http_body(resp), "\"players\"");
    if (!p || !(p = strchr(p, '['))) {
        return XBL_ERR_PARSE;
    }
    p++;
    int n = 0;
    while (n < max) {
        const char *obj = strchr(p, '{');
        if (!obj) {
            break;
        }
        const char *end = strchr(obj, '}');
        if (!end) {
            break;
        }
        size_t objlen = (size_t)(end - obj) + 1;
        char buf[160];
        if (objlen >= sizeof(buf)) {
            objlen = sizeof(buf) - 1;
        }
        memcpy(buf, obj, objlen);
        buf[objlen] = '\0';
        memset(&out[n], 0, sizeof(out[n]));
        json_str(buf, "name", out[n].name, XBL_USER_MAX);
        json_str(buf, "status", out[n].status, XBL_PRESENCE_STATUS_MAX);
        long long idle = 0;
        json_ll(buf, "idle_sec", &idle);
        out[n].idle_sec = (int)idle;
        n++;
        p = end + 1;
    }
    if (count) {
        *count = n;
    }
    return XBL_OK;
}

/* ---------- multiplayer ---------- */

/* Parse one player object "{...}" into P. */
static void parse_player(const char *buf, XblPlayer *P)
{
    long long sc = 0, fr = 0, px = 0;
    P->name[0] = '\0';
    json_str(buf, "name", P->name, XBL_USER_MAX);
    json_ll(buf, "score", &sc);
    json_ll(buf, "frame", &fr);
    json_ll(buf, "px", &px);
    P->score = sc;
    P->frame = (int)fr;
    P->px = (int)px;
    P->alive = json_bool_true(buf, "alive");
    P->finished = json_bool_true(buf, "finished");
    P->cleared = json_bool_true(buf, "cleared");
    P->verified = json_bool_true(buf, "verified");
    long long w = 0, l = 0, d = 0, st = 0;
    json_ll(buf, "wins", &w);
    json_ll(buf, "losses", &l);
    json_ll(buf, "draws", &d);
    json_ll(buf, "streak", &st);
    P->wins = (int)w;
    P->losses = (int)l;
    P->draws = (int)d;
    P->streak = (int)st;
}

/* Parse the "lobby" object out of a response into L. */
static int parse_lobby(const char *json, XblLobby *L)
{
    if (!L) {
        return XBL_OK; /* caller doesn't want it */
    }
    memset(L, 0, sizeof(*L));
    const char *lob = strstr(json, "\"lobby\"");
    const char *obj = lob ? strchr(lob, '{') : strchr(json, '{');
    if (!obj) {
        return XBL_ERR_PARSE;
    }
    /* Scalars live in the header before the players array. */
    const char *pl = strstr(obj, "\"players\"");
    size_t hlen = pl ? (size_t)(pl - obj) : strlen(obj);
    char hdr[512];
    if (hlen >= sizeof(hdr)) {
        hlen = sizeof(hdr) - 1;
    }
    memcpy(hdr, obj, hlen);
    hdr[hlen] = '\0';

    json_str(hdr, "code", L->code, XBL_CODE_MAX);
    json_str(hdr, "host", L->host, XBL_USER_MAX);
    json_str(hdr, "name", L->name, XBL_LOBBY_NAME_MAX);
    json_str(hdr, "map_name", L->map_name, XBL_MAPNAME_MAX);
    json_str(hdr, "mode", L->mode, XBL_MODE_MAX);
    json_str(hdr, "status", L->status, sizeof(L->status));
    json_str(hdr, "winner", L->winner, XBL_USER_MAX);
    long long mid = 0, maxp = 0, seed = 0, pc = 0, tgt = 0, ac = 0;
    json_ll(hdr, "map_id", &mid);
    json_ll(hdr, "target", &tgt);
    json_ll(hdr, "max_players", &maxp);
    json_ll(hdr, "seed", &seed);
    json_ll(hdr, "player_count", &pc);
    json_ll(hdr, "alive_count", &ac);
    L->map_id = (int)mid;
    L->target = (int)tgt;
    L->max_players = (int)maxp;
    L->seed = (uint32_t)seed;
    L->player_count = (int)pc;
    L->alive_count = (int)ac;
    L->win_stays = json_bool_true(hdr, "win_stays");
    L->friends_only = json_bool_true(hdr, "friends_only");
    if (!L->mode[0]) {
        snprintf(L->mode, sizeof(L->mode), "%s", XBL_MODE_SURVIVAL);
    }

    if (pl) {
        const char *p = strchr(pl, '[');
        if (p) {
            p++;
            int n = 0;
            while (n < XBL_MP_MAX_PLAYERS) {
                const char *po = strchr(p, '{');
                if (!po) {
                    break;
                }
                const char *end = strchr(po, '}');
                if (!end) {
                    break;
                }
                size_t olen = (size_t)(end - po) + 1;
                char buf[256];
                if (olen >= sizeof(buf)) {
                    olen = sizeof(buf) - 1;
                }
                memcpy(buf, po, olen);
                buf[olen] = '\0';
                parse_player(buf, &L->players[n]);
                n++;
                p = end + 1;
                /* stop if the array closes before the next object */
                const char *closeBracket = strchr(p, ']');
                const char *nextObj = strchr(p, '{');
                if (closeBracket && (!nextObj || closeBracket < nextObj)) {
                    break;
                }
            }
            if (n > L->player_count) {
                L->player_count = n;
            }
        }
    }
    return XBL_OK;
}

/* Sign "mp|game_id" (lobby create/join/leave/map/start/progress auth). */
static int mp_app_sig(char *sig)
{
    char sigmsg[160];
    snprintf(sigmsg, sizeof(sigmsg), "mp|%s", g_cfg.game_id);
    return xbl_hmac_sha256_hex(g_cfg.game_secret, (const unsigned char *)sigmsg, strlen(sigmsg),
                               sig);
}

/* Shared session-key POST helper for the lobby endpoints; parses lobby on 2xx. */
static int mp_post(const XblSession *s, const char *path, const char *body, XblLobby *out)
{
    if (!g_inited || !s || !s->session_key[0]) {
        return XBL_ERR_CONFIG;
    }
    char skHeader[XBL_SK_MAX + 32];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", s->session_key);
    const char *headers[] = { skHeader };

    static char resp[HTTP_BUF];
    memset(resp, 0, sizeof(resp));
    if (https_request(g_cfg.host, g_cfg.port, "POST", path, "application/json", headers, 1, body,
                      body ? strlen(body) : 0, resp, sizeof(resp), NULL, NULL) != 0) {
        return XBL_ERR_NET;
    }
    if (!http_status_ok(resp)) {
        const char *sp = strchr(resp, ' ');
        return (sp && sp[1] == '4') ? XBL_ERR_REJECTED : XBL_ERR_HTTP;
    }
    return parse_lobby(http_body(resp), out);
}

int xbl_mp_create(const XblSession *s, int map_id, const char *name, XblLobby *out)
{
    char sig[65];
    if (mp_app_sig(sig) != XBL_OK) {
        return XBL_ERR_CONFIG;
    }
    char body[512];
    if (name && name[0]) {
        snprintf(body, sizeof(body),
                 "{\"game_id\":\"%s\",\"map_id\":%d,\"name\":\"%s\",\"sig\":\"%s\"}", g_cfg.game_id,
                 map_id, name, sig);
    } else {
        snprintf(body, sizeof(body), "{\"game_id\":\"%s\",\"map_id\":%d,\"sig\":\"%s\"}",
                 g_cfg.game_id, map_id, sig);
    }
    return mp_post(s, "/api/hb/mp/lobby/create", body, out);
}

int xbl_mp_list(XblLobbyInfo *out, int max, int *count)
{
    if (!g_inited || !out || max <= 0) {
        return XBL_ERR_CONFIG;
    }
    if (count) {
        *count = 0;
    }
    char path[256];
    /* status=all so in-progress lobbies show up too (joinable as a spectator who
     * is dealt into the next round). */
    snprintf(path, sizeof(path), "/api/hb/mp/lobbies?game_id=%s&status=all", g_cfg.game_id);

    static char resp[HTTP_BUF];
    memset(resp, 0, sizeof(resp));
    if (https_request(g_cfg.host, g_cfg.port, "GET", path, "application/json", NULL, 0, NULL, 0, resp,
                      sizeof(resp), NULL, NULL) != 0) {
        return XBL_ERR_NET;
    }
    if (!http_status_ok(resp)) {
        return XBL_ERR_HTTP;
    }
    const char *p = strstr(http_body(resp), "\"lobbies\"");
    if (!p || !(p = strchr(p, '['))) {
        return XBL_ERR_PARSE;
    }
    p++;
    int n = 0;
    while (n < max) {
        const char *obj = strchr(p, '{');
        if (!obj) {
            break;
        }
        const char *end = strchr(obj, '}');
        if (!end) {
            break;
        }
        size_t objlen = (size_t)(end - obj) + 1;
        char buf[384];
        if (objlen >= sizeof(buf)) {
            objlen = sizeof(buf) - 1;
        }
        memcpy(buf, obj, objlen);
        buf[objlen] = '\0';
        memset(&out[n], 0, sizeof(out[n]));
        json_str(buf, "code", out[n].code, XBL_CODE_MAX);
        json_str(buf, "host", out[n].host, XBL_USER_MAX);
        json_str(buf, "name", out[n].name, XBL_LOBBY_NAME_MAX);
        json_str(buf, "map_name", out[n].map_name, XBL_MAPNAME_MAX);
        json_str(buf, "mode", out[n].mode, XBL_MODE_MAX);
        if (!out[n].mode[0]) snprintf(out[n].mode, XBL_MODE_MAX, "%s", XBL_MODE_SURVIVAL);
        json_str(buf, "status", out[n].status, sizeof(out[n].status));
        long long mid = 0, pc = 0, mp = 0;
        json_ll(buf, "map_id", &mid);
        json_ll(buf, "player_count", &pc);
        json_ll(buf, "max_players", &mp);
        out[n].map_id = (int)mid;
        out[n].player_count = (int)pc;
        out[n].max_players = (int)mp;
        n++;
        p = end + 1;
    }
    if (count) {
        *count = n;
    }
    return XBL_OK;
}

int xbl_mp_join(const XblSession *s, const char *code, XblLobby *out)
{
    char sig[65];
    if (!code || mp_app_sig(sig) != XBL_OK) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s/join", code);
    char body[128];
    snprintf(body, sizeof(body), "{\"sig\":\"%s\"}", sig);
    return mp_post(s, path, body, out);
}

int xbl_mp_leave(const XblSession *s, const char *code)
{
    char sig[65];
    if (!code || mp_app_sig(sig) != XBL_OK) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s/leave", code);
    char body[128];
    snprintf(body, sizeof(body), "{\"sig\":\"%s\"}", sig);
    return mp_post(s, path, body, NULL);
}

int xbl_mp_return(const XblSession *s, const char *code, XblLobby *out)
{
    if (!code) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s/return", code);
    return mp_post(s, path, "{}", out);
}

int xbl_mp_get(const char *code, XblLobby *out)
{
    if (!g_inited || !code || !out) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s", code);
    static char resp[HTTP_BUF];
    memset(resp, 0, sizeof(resp));
    if (https_request(g_cfg.host, g_cfg.port, "GET", path, "application/json", NULL, 0, NULL, 0, resp,
                      sizeof(resp), NULL, NULL) != 0) {
        return XBL_ERR_NET; /* transient: leave `out` untouched so the caller keeps last state */
    }
    if (!http_status_ok(resp)) {
        const char *sp = strchr(resp, ' ');
        if (sp && sp[1] == '4') {
            /* The lobby is gone (e.g. disbanded). Clear `out` so callers that poll
             * with `out->code[0]` reliably detect it, instead of rendering a stale
             * lobby forever. 5xx stays transient (out untouched). */
            memset(out, 0, sizeof(*out));
            return XBL_ERR_REJECTED;
        }
        return XBL_ERR_HTTP;
    }
    return parse_lobby(http_body(resp), out);
}

int xbl_mp_poll(const XblSession *s, const char *code, XblLobby *out)
{
    /* Authenticated variant of xbl_mp_get: same lobby state, but sending the
     * session lets the server refresh THIS member's liveness (last_seen) while
     * we sit in the room. That's how an abandoned "waiting" lobby is told apart
     * from one with players actually in it, so ghosts get reaped and live lobbies
     * don't. Falls back to the public GET when there's no session. */
    if (!s || !s->session_key[0]) {
        return xbl_mp_get(code, out);
    }
    if (!g_inited || !code || !out) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s", code);
    char skHeader[XBL_SK_MAX + 32];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", s->session_key);
    const char *headers[] = { skHeader };
    static char resp[HTTP_BUF];
    memset(resp, 0, sizeof(resp));
    if (https_request(g_cfg.host, g_cfg.port, "GET", path, "application/json", headers, 1, NULL, 0,
                      resp, sizeof(resp), NULL, NULL) != 0) {
        return XBL_ERR_NET; /* transient: leave `out` untouched */
    }
    if (!http_status_ok(resp)) {
        const char *sp = strchr(resp, ' ');
        if (sp && sp[1] == '4') {
            memset(out, 0, sizeof(*out));
            return XBL_ERR_REJECTED;
        }
        return XBL_ERR_HTTP;
    }
    return parse_lobby(http_body(resp), out);
}

int xbl_mp_set_map(const XblSession *s, const char *code, int map_id, XblLobby *out)
{
    if (!code) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s/map", code);
    char body[64];
    snprintf(body, sizeof(body), "{\"map_id\":%d}", map_id);
    return mp_post(s, path, body, out);
}

int xbl_mp_set_mode(const XblSession *s, const char *code, const char *mode, XblLobby *out)
{
    if (!code || !mode) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s/mode", code);
    char body[64];
    snprintf(body, sizeof(body), "{\"mode\":\"%s\"}", mode);
    return mp_post(s, path, body, out);
}

int xbl_mp_set_win_stays(const XblSession *s, const char *code, int on, XblLobby *out)
{
    if (!code) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s/option", code);
    char body[48];
    snprintf(body, sizeof(body), "{\"win_stays\":%s}", on ? "true" : "false");
    return mp_post(s, path, body, out);
}

int xbl_mp_set_max_players(const XblSession *s, const char *code, int max_players, XblLobby *out)
{
    if (!code) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s/maxplayers", code);
    char body[48];
    snprintf(body, sizeof(body), "{\"max_players\":%d}", max_players);
    return mp_post(s, path, body, out);
}

int xbl_mp_set_friends_only(const XblSession *s, const char *code, int on, XblLobby *out)
{
    if (!code) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s/option", code);
    char body[48];
    snprintf(body, sizeof(body), "{\"friends_only\":%s}", on ? "true" : "false");
    return mp_post(s, path, body, out);
}

int xbl_mp_kick(const XblSession *s, const char *code, const char *username, XblLobby *out)
{
    if (!code || !username || !username[0]) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s/kick", code);
    char body[XBL_USER_MAX + 24];
    snprintf(body, sizeof(body), "{\"username\":\"%s\"}", username);
    return mp_post(s, path, body, out);
}

/* ---- no-code matchmaking ---- */

int xbl_mp_find(const char *mode, XblLobbyInfo *out, int max, int *count)
{
    if (count) {
        *count = 0;
    }
    if (!g_inited || !out || max <= 0) {
        return XBL_ERR_CONFIG;
    }
    /* Pull the open list, then keep only joinable rooms (waiting + a free slot)
     * matching `mode` (if given). xbl_mp_list uses its own static response buffer;
     * this temp just holds the parsed rows before filtering. */
    static XblLobbyInfo tmp[32];
    int n = 0;
    int r = xbl_mp_list(tmp, (int)(sizeof(tmp) / sizeof(tmp[0])), &n);
    if (r != XBL_OK) {
        return r;
    }
    int kept = 0;
    for (int i = 0; i < n && kept < max; i++) {
        if (strcmp(tmp[i].status, "waiting") != 0) {
            continue;
        }
        int cap = tmp[i].max_players ? tmp[i].max_players : XBL_MP_MAX_PLAYERS;
        if (tmp[i].player_count >= cap) {
            continue;
        }
        if (mode && mode[0] && strcmp(tmp[i].mode, mode) != 0) {
            continue;
        }
        out[kept++] = tmp[i];
    }
    if (count) {
        *count = kept;
    }
    return XBL_OK;
}

int xbl_mp_create_mode(const XblSession *s, int map_id, const char *mode, const char *name,
                       XblLobby *out)
{
    XblLobby tmp;
    int r = xbl_mp_create(s, map_id, name, &tmp);
    if (r != XBL_OK) {
        return r;
    }
    if (mode && mode[0] && strcmp(tmp.mode, mode) != 0) {
        r = xbl_mp_set_mode(s, tmp.code, mode, &tmp);
        if (r != XBL_OK) {
            /* Don't leave a half-configured room lingering. */
            xbl_mp_leave(s, tmp.code);
            return r;
        }
    }
    if (out) {
        *out = tmp;
    }
    return XBL_OK;
}

int xbl_mp_quick_match(const XblSession *s, const char *mode, int map_id, XblLobby *out, int *is_host)
{
    if (is_host) {
        *is_host = 0;
    }
    if (!g_inited || !s) {
        return XBL_ERR_CONFIG;
    }
    /* Try to slot into an existing open match first. */
    static XblLobbyInfo found[32];
    int n = 0;
    if (xbl_mp_find(mode, found, (int)(sizeof(found) / sizeof(found[0])), &n) == XBL_OK) {
        for (int i = 0; i < n; i++) {
            if (xbl_mp_join(s, found[i].code, out) == XBL_OK) {
                return XBL_OK; /* joined a waiting opponent */
            }
            /* The slot may have filled between list and join — try the next. */
        }
    }
    /* Nothing joinable: host a new room in this mode and wait. */
    int r = xbl_mp_create_mode(s, map_id, mode, NULL, out);
    if (r == XBL_OK && is_host) {
        *is_host = 1;
    }
    return r;
}

/* POST `path` (session-auth) and parse the shared round info (run/seed/map_id) +
 * the lobby. Used by both /start and /me. */
static int mp_round_post(const XblSession *s, const char *path, XblRun *run, uint32_t *seed,
                         int *map_id, XblLobby *out)
{
    if (!g_inited || !s || !s->session_key[0]) {
        return XBL_ERR_CONFIG;
    }
    char skHeader[XBL_SK_MAX + 32];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", s->session_key);
    const char *headers[] = { skHeader };

    static char resp[HTTP_BUF];
    memset(resp, 0, sizeof(resp));
    if (https_request(g_cfg.host, g_cfg.port, "POST", path, "application/json", headers, 1, "{}", 2,
                      resp, sizeof(resp), NULL, NULL) != 0) {
        return XBL_ERR_NET;
    }
    if (!http_status_ok(resp)) {
        const char *sp = strchr(resp, ' ');
        return (sp && sp[1] == '4') ? XBL_ERR_REJECTED : XBL_ERR_HTTP;
    }
    const char *json = http_body(resp);
    long long sd = 0, mid = 0, st = 0;
    json_ll(json, "seed", &sd);
    json_ll(json, "map_id", &mid);
    json_ll(json, "server_time", &st);
    if (run) {
        memset(run, 0, sizeof(*run));
        json_str(json, "run_id", run->run_id, XBL_RUNID_MAX);
        json_str(json, "nonce", run->nonce, XBL_NONCE_MAX);
        run->seed = (uint32_t)sd;
        run->map_id = (int)mid;
        run->server_time = (unsigned long long)st;
    }
    if (seed) {
        *seed = (uint32_t)sd;
    }
    if (map_id) {
        *map_id = (int)mid;
    }
    return parse_lobby(json, out);
}

int xbl_mp_start(const XblSession *s, const char *code, XblRun *run, uint32_t *seed, int *map_id,
                 XblLobby *out)
{
    if (!code) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s/start", code);
    return mp_round_post(s, path, run, seed, map_id, out);
}

int xbl_mp_round(const XblSession *s, const char *code, XblRun *run, uint32_t *seed, int *map_id,
                 XblLobby *out)
{
    if (!code) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s/me", code);
    return mp_round_post(s, path, run, seed, map_id, out);
}

int xbl_mp_progress(const XblSession *s, const char *code, int frame, long long score, int alive,
                    int px, XblLobby *out)
{
    if (!code) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s/progress", code);
    char body[128];
    snprintf(body, sizeof(body), "{\"frame\":%d,\"score\":%lld,\"px\":%d,\"alive\":%s}", frame, score,
             px, alive ? "true" : "false");
    return mp_post(s, path, body, out);
}

int xbl_mp_finish(const XblSession *s, const char *code, const XblRun *run, long long score,
                  uint32_t seed, const char *input_log, int *accepted, XblLobby *out)
{
    if (!g_inited || !s || !code || !run || !s->session_key[0]) {
        return XBL_ERR_CONFIG;
    }
    if (accepted) {
        *accepted = 0;
    }
    const char *log = input_log ? input_log : "";
    char input_hash[65];
    if (xbl_sha256_hex((const unsigned char *)log, strlen(log), input_hash) != XBL_OK) {
        return XBL_ERR_CONFIG;
    }
    /* Same canonical shape as a score, with the fixed "mp" board id. */
    char sigmsg[512];
    snprintf(sigmsg, sizeof(sigmsg), "score|%s|%s|%s|mp|%lld|%u|%s", g_cfg.game_id, run->run_id,
             run->nonce, score, seed, input_hash);
    char sig[65];
    if (xbl_hmac_sha256_hex(g_cfg.game_secret, (const unsigned char *)sigmsg, strlen(sigmsg), sig) !=
        XBL_OK) {
        return XBL_ERR_CONFIG;
    }

    size_t cap = strlen(log) + 768;
    char *body = (char *)malloc(cap);
    if (!body) {
        return XBL_ERR_NOMEM;
    }
    int n = snprintf(body, cap,
                     "{\"score\":%lld,\"seed\":%u,\"input_hash\":\"%s\",\"sig\":\"%s\","
                     "\"input_log\":\"%s\"}",
                     score, seed, input_hash, sig, log);
    if (n < 0 || (size_t)n >= cap) {
        free(body);
        return XBL_ERR_NOMEM;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s/finish", code);

    char skHeader[XBL_SK_MAX + 32];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", s->session_key);
    const char *headers[] = { skHeader };

    static char resp[HTTP_BUF];
    memset(resp, 0, sizeof(resp));
    int r = https_request(g_cfg.host, g_cfg.port, "POST", path, "application/json", headers, 1, body,
                          (size_t)n, resp, sizeof(resp), NULL, NULL);
    free(body);
    if (r != 0) {
        return XBL_ERR_NET;
    }
    if (!http_status_ok(resp)) {
        const char *sp = strchr(resp, ' ');
        return (sp && sp[1] == '4') ? XBL_ERR_REJECTED : XBL_ERR_HTTP;
    }
    const char *json = http_body(resp);
    if (accepted) {
        *accepted = json_bool_true(json, "verified") || 1; /* stored even if unverified */
    }
    return parse_lobby(json, out);
}

/* ---------- Live Battle: signaling + verified finish ---------- */

/* Session-authed request into the shared static response buffer; on a 2xx,
 * `*jsonOut` points at the JSON body. */
/* Session-authenticated request into a caller-provided response buffer. Lets the
 * big, count-scaling responses (e.g. the full friends roster) use a buffer large
 * enough to hold the whole list instead of the shared 16K one, which would
 * otherwise truncate it (and silently cap the list). `jsonOut` points into `buf`. */
static int sess_request_into(const XblSession *s, const char *method, const char *path,
                             const char *body, char *buf, size_t buflen, const char **jsonOut)
{
    if (!g_inited || !s || !s->session_key[0] || !buf || buflen == 0) {
        return XBL_ERR_CONFIG;
    }
    char skHeader[XBL_SK_MAX + 32];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", s->session_key);
    const char *headers[] = { skHeader };

    memset(buf, 0, buflen);
    if (https_request(g_cfg.host, g_cfg.port, method, path, "application/json", headers, 1,
                      body ? body : NULL, body ? strlen(body) : 0, buf, buflen, NULL,
                      NULL) != 0) {
        return XBL_ERR_NET;
    }
    if (!http_status_ok(buf)) {
        const char *sp = strchr(buf, ' ');
        return (sp && sp[1] == '4') ? XBL_ERR_REJECTED : XBL_ERR_HTTP;
    }
    if (jsonOut) {
        *jsonOut = http_body(buf);
    }
    return XBL_OK;
}

static int sess_request(const XblSession *s, const char *method, const char *path, const char *body,
                        const char **jsonOut)
{
    static char resp[HTTP_BUF];
    return sess_request_into(s, method, path, body, resp, sizeof(resp), jsonOut);
}

/* Parse a flat live-battle match/signaling object into `m`. */
static void parse_net_match(const char *json, XblNetMatch *m)
{
    if (!m) {
        return;
    }
    memset(m, 0, sizeof(*m));
    long long role = 0, seed = 0, mid = 0, port = 0, pc = 0;
    json_ll(json, "role", &role);
    json_ll(json, "seed", &seed);
    json_ll(json, "map_id", &mid);
    json_ll(json, "peer_port", &port);
    json_ll(json, "player_count", &pc);
    m->role = (int)role;
    m->seed = (uint32_t)seed;
    m->map_id = (int)mid;
    m->peer_port = (int)port;
    m->player_count = (int)pc < 2 ? 2 : (int)pc;
    m->peer_ready = json_bool_true(json, "peer_ready");
    json_str(json, "peer_name", m->peer_name, XBL_USER_MAX);
    json_str(json, "peer_ip", m->peer_ip, XBL_IP_MAX);
    json_str(json, "peer_wan_ip", m->peer_wan_ip, XBL_IP_MAX);
}

int xbl_mp_net_register(const XblSession *s, const char *code, const char *local_ip, int local_port,
                        XblNetMatch *out)
{
    if (!code) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s/net", code);
    char body[128];
    snprintf(body, sizeof(body), "{\"ip\":\"%s\",\"port\":%d}", local_ip ? local_ip : "", local_port);
    const char *json = NULL;
    int r = sess_request(s, "POST", path, body, &json);
    if (r != XBL_OK) {
        return r;
    }
    parse_net_match(json, out);
    return XBL_OK;
}

int xbl_mp_net_poll(const XblSession *s, const char *code, XblNetMatch *out)
{
    if (!code) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s/net", code);
    const char *json = NULL;
    int r = sess_request(s, "GET", path, NULL, &json);
    if (r != XBL_OK) {
        return r;
    }
    parse_net_match(json, out);
    return XBL_OK;
}

int xbl_mp_battle_finish(const XblSession *s, const char *code, const XblRun *run, uint32_t seed,
                         const char *input_log, int *result, char *winner_name, size_t wnsz)
{
    if (!g_inited || !s || !code || !run || !s->session_key[0]) {
        return XBL_ERR_CONFIG;
    }
    if (result) {
        *result = -1;
    }
    if (winner_name && wnsz) {
        winner_name[0] = '\0';
    }
    const char *log = input_log ? input_log : "";
    char input_hash[65];
    if (xbl_sha256_hex((const unsigned char *)log, strlen(log), input_hash) != XBL_OK) {
        return XBL_ERR_CONFIG;
    }
    /* Canonical shape mirrors a score with the fixed "livebattle" board id and a
     * score field of 0 (the winner is decided by the server's 2P replay, not a
     * client-claimed score). */
    char sigmsg[512];
    snprintf(sigmsg, sizeof(sigmsg), "score|%s|%s|%s|livebattle|0|%u|%s", g_cfg.game_id, run->run_id,
             run->nonce, seed, input_hash);
    char sig[65];
    if (xbl_hmac_sha256_hex(g_cfg.game_secret, (const unsigned char *)sigmsg, strlen(sigmsg), sig) !=
        XBL_OK) {
        return XBL_ERR_CONFIG;
    }

    size_t cap = strlen(log) + 768;
    char *body = (char *)malloc(cap);
    if (!body) {
        return XBL_ERR_NOMEM;
    }
    int n = snprintf(body, cap,
                     "{\"seed\":%u,\"input_hash\":\"%s\",\"sig\":\"%s\",\"input_log\":\"%s\"}", seed,
                     input_hash, sig, log);
    if (n < 0 || (size_t)n >= cap) {
        free(body);
        return XBL_ERR_NOMEM;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/mp/lobby/%s/battle", code);

    char skHeader[XBL_SK_MAX + 32];
    snprintf(skHeader, sizeof(skHeader), "X-Session-Key: %s", s->session_key);
    const char *headers[] = { skHeader };

    static char resp[HTTP_BUF];
    memset(resp, 0, sizeof(resp));
    int r = https_request(g_cfg.host, g_cfg.port, "POST", path, "application/json", headers, 1, body,
                          (size_t)n, resp, sizeof(resp), NULL, NULL);
    free(body);
    if (r != 0) {
        return XBL_ERR_NET;
    }
    if (!http_status_ok(resp)) {
        const char *sp = strchr(resp, ' ');
        return (sp && sp[1] == '4') ? XBL_ERR_REJECTED : XBL_ERR_HTTP;
    }
    const char *json = http_body(resp);
    if (result) {
        long long res = -1;
        json_ll(json, "result", &res);
        *result = (int)res;
    }
    if (winner_name && wnsz) {
        json_str(json, "winner", winner_name, wnsz);
    }
    return XBL_OK;
}

/* ---------- friends + invites ---------- */

static int invite_app_sig(char *sig)
{
    char sigmsg[160];
    snprintf(sigmsg, sizeof(sigmsg), "invite|%s", g_cfg.game_id);
    return xbl_hmac_sha256_hex(g_cfg.game_secret, (const unsigned char *)sigmsg, strlen(sigmsg), sig);
}

int xbl_friends_fetch(const XblSession *s, XblFriend *out, int max, int *count)
{
    if (count) {
        *count = 0;
    }
    if (!g_inited || !s || !out || max <= 0) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/friends?game_id=%s", g_cfg.game_id);
    /* Dedicated buffer big enough for the WHOLE roster: each friend object can be
     * a few hundred bytes, so the shared 16K buffer would truncate the JSON for a
     * large list and silently cap how many we parse. Sized for XBL_FRIENDS_MAX
     * friends (worst-case ~640 bytes each) plus response headers. */
    static char resp[XBL_FRIENDS_MAX * 640 + 4096];
    const char *json = NULL;
    int r = sess_request_into(s, "GET", path, NULL, resp, sizeof(resp), &json);
    if (r != XBL_OK) {
        return r;
    }
    const char *p = strstr(json, "\"friends\"");
    if (!p || !(p = strchr(p, '['))) {
        return XBL_ERR_PARSE;
    }
    p++;
    int n = 0;
    while (n < max) {
        const char *obj = strchr(p, '{');
        if (!obj) {
            break;
        }
        const char *end = strchr(obj, '}');
        if (!end) {
            break;
        }
        size_t objlen = (size_t)(end - obj) + 1;
        /* Big enough for a full friend object (names/game ids up to XBL_USER_MAX
         * etc.) so trailing fields like "playing"/"online_any" aren't truncated. */
        char buf[640];
        if (objlen >= sizeof(buf)) {
            objlen = sizeof(buf) - 1;
        }
        memcpy(buf, obj, objlen);
        buf[objlen] = '\0';
        memset(&out[n], 0, sizeof(out[n]));
        json_str(buf, "name", out[n].name, XBL_USER_MAX);
        json_str(buf, "invite_to", out[n].invite_to, XBL_USER_MAX);
        out[n].online = json_bool_true(buf, "online");
        out[n].in_this_game = json_bool_true(buf, "in_this_game");
        json_str(buf, "hb_status", out[n].status, XBL_PRESENCE_STATUS_MAX);
        json_str(buf, "hb_game_id", out[n].game_id, XBL_GAMEID_MAX);
        json_str(buf, "lobby_code", out[n].lobby_code, XBL_CODE_MAX);
        out[n].xbox_online = json_bool_true(buf, "xbox_online");
        out[n].online_any = json_bool_true(buf, "online_any");
        json_str(buf, "playing", out[n].playing, XBL_GAMENAME_MAX);
        n++;
        p = end + 1;
    }
    if (count) {
        *count = n;
    }
    return XBL_OK;
}

int xbl_invite_send(const XblSession *s, const char *to_username, const char *lobby_code)
{
    if (!g_inited || !s || !s->session_key[0] || !to_username || !lobby_code) {
        return XBL_ERR_CONFIG;
    }
    char sig[65];
    if (invite_app_sig(sig) != XBL_OK) {
        return XBL_ERR_CONFIG;
    }
    char body[256];
    snprintf(body, sizeof(body),
             "{\"game_id\":\"%s\",\"to\":\"%s\",\"lobby_code\":\"%s\",\"sig\":\"%s\"}", g_cfg.game_id,
             to_username, lobby_code, sig);
    return sess_request(s, "POST", "/api/hb/invites/send", body, NULL);
}

int xbl_invites_fetch(const XblSession *s, XblInvite *out, int max, int *count)
{
    if (count) {
        *count = 0;
    }
    if (!g_inited || !s || !out || max <= 0) {
        return XBL_ERR_CONFIG;
    }
    char path[128];
    snprintf(path, sizeof(path), "/api/hb/invites?game_id=%s", g_cfg.game_id);
    const char *json = NULL;
    int r = sess_request(s, "GET", path, NULL, &json);
    if (r != XBL_OK) {
        return r;
    }
    const char *p = strstr(json, "\"invites\"");
    if (!p || !(p = strchr(p, '['))) {
        return XBL_ERR_PARSE;
    }
    p++;
    int n = 0;
    while (n < max) {
        const char *obj = strchr(p, '{');
        if (!obj) {
            break;
        }
        const char *end = strchr(obj, '}');
        if (!end) {
            break;
        }
        size_t objlen = (size_t)(end - obj) + 1;
        char buf[384];
        if (objlen >= sizeof(buf)) {
            objlen = sizeof(buf) - 1;
        }
        memcpy(buf, obj, objlen);
        buf[objlen] = '\0';
        memset(&out[n], 0, sizeof(out[n]));
        long long id = 0, mid = 0, pc = 0, mp = 0, age = 0;
        json_ll(buf, "id", &id);
        out[n].id = id;
        json_str(buf, "from", out[n].from, XBL_USER_MAX);
        json_str(buf, "lobby_code", out[n].lobby_code, XBL_CODE_MAX);
        json_str(buf, "status", out[n].status, sizeof(out[n].status));
        json_str(buf, "mode", out[n].mode, XBL_MODE_MAX);
        if (!out[n].mode[0]) snprintf(out[n].mode, XBL_MODE_MAX, "%s", XBL_MODE_SURVIVAL);
        json_str(buf, "map_name", out[n].map_name, XBL_MAPNAME_MAX);
        json_ll(buf, "map_id", &mid);
        json_ll(buf, "player_count", &pc);
        json_ll(buf, "max_players", &mp);
        json_ll(buf, "age_sec", &age);
        out[n].map_id = (int)mid;
        out[n].player_count = (int)pc;
        out[n].max_players = (int)mp;
        out[n].age_sec = (int)age;
        n++;
        p = end + 1;
    }
    if (count) {
        *count = n;
    }
    return XBL_OK;
}

int xbl_invite_accept(const XblSession *s, long long id, char *lobby_code_out, size_t sz)
{
    if (lobby_code_out && sz) {
        lobby_code_out[0] = '\0';
    }
    if (!g_inited || !s || !s->session_key[0] || id <= 0) {
        return XBL_ERR_CONFIG;
    }
    char path[96];
    snprintf(path, sizeof(path), "/api/hb/invites/%lld/accept", id);
    const char *json = NULL;
    int r = sess_request(s, "POST", path, "{}", &json);
    if (r != XBL_OK) {
        return r;
    }
    if (lobby_code_out && sz) {
        json_str(json, "lobby_code", lobby_code_out, sz);
    }
    return XBL_OK;
}

int xbl_invite_decline(const XblSession *s, long long id)
{
    if (!g_inited || !s || !s->session_key[0] || id <= 0) {
        return XBL_ERR_CONFIG;
    }
    char path[96];
    snprintf(path, sizeof(path), "/api/hb/invites/%lld/decline", id);
    return sess_request(s, "POST", path, "{}", NULL);
}
