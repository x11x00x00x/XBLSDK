/*
 * xbl_save.c - original-Xbox-format game saves for the xb.live Homebrew SDK.
 *
 * Retail Xbox games store saves under E:\UDATA\<TitleID>\<SaveFolder>\ with two
 * INI-style text files the dashboard reads:
 *   - E:\UDATA\<TitleID>\TitleMeta.xbx        ->  TitleName=<game name>
 *   - E:\UDATA\<TitleID>\<slot>\SaveMeta.xbx  ->  Name=<save label>
 * and arbitrary game data files beside SaveMeta.xbx. We mirror that exactly so a
 * TestGame save shows up in the dashboard and in save backup/restore tools.
 *
 * The Title ID is read at runtime from the running XBE certificate (it is fixed
 * at 0xFFFF0002 for stock-cxbe homebrew, but reading it keeps us correct if that
 * ever changes). The caller must have mounted E: (HDD partition 1) first; the
 * game already does this at startup.
 *
 * This module deliberately uses only Win32 file APIs (CreateDirectory/CreateFile/
 * WriteFile/ReadFile/FindFirstFile), which nxdk provides and the rest of the
 * project already links against.
 */
#include "xblsdk.h"

#include <stdio.h>
#include <string.h>

#include <windows.h>
#include <winnt.h> /* CURRENT_XBE_HEADER, XBE_CERTIFICATE_HEADER (nxdk) */

#define UDATA_ROOT "E:\\UDATA"

/* Builds "E:\UDATA\<TitleID>" into `out`. */
static void title_dir(char *out, size_t outsz)
{
    DWORD tid = CURRENT_XBE_HEADER->CertificateHeader->TitleID;
    snprintf(out, outsz, "%s\\%08X", UDATA_ROOT, (unsigned)tid);
}

/* Validates a slot folder name: 1..15 of [A-Za-z0-9_]. */
static int slot_ok(const char *slot)
{
    if (!slot || !slot[0]) {
        return 0;
    }
    size_t n = strlen(slot);
    if (n >= XBL_SAVE_SLOT_MAX) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char c = slot[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '_';
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

/* Writes `len` bytes of `data` to `path`, truncating any existing file. */
static int write_file(const char *path, const void *data, size_t len)
{
    HANDLE h = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return XBL_ERR_CONFIG;
    }
    int rc = XBL_OK;
    if (len) {
        DWORD wrote = 0;
        if (!WriteFile(h, data, (DWORD)len, &wrote, NULL) || wrote != (DWORD)len) {
            rc = XBL_ERR_CONFIG;
        }
    }
    CloseHandle(h);
    return rc;
}

int xbl_save_init(const char *title_name)
{
    CreateDirectory(UDATA_ROOT, NULL);
    char tdir[MAX_PATH];
    title_dir(tdir, sizeof(tdir));
    CreateDirectory(tdir, NULL);

    char meta[MAX_PATH];
    snprintf(meta, sizeof(meta), "%s\\TitleMeta.xbx", tdir);
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "TitleName=%s\r\n",
                     (title_name && title_name[0]) ? title_name : "TestGame");
    return write_file(meta, buf, (size_t)n);
}

int xbl_save_write(const char *slot, const char *name, const void *data, size_t len)
{
    if (!slot_ok(slot)) {
        return XBL_ERR_CONFIG;
    }
    char tdir[MAX_PATH];
    title_dir(tdir, sizeof(tdir));
    CreateDirectory(UDATA_ROOT, NULL);
    CreateDirectory(tdir, NULL);

    char sdir[MAX_PATH];
    snprintf(sdir, sizeof(sdir), "%s\\%s", tdir, slot);
    CreateDirectory(sdir, NULL);

    /* SaveMeta.xbx: the dashboard-visible label. */
    char meta[MAX_PATH];
    snprintf(meta, sizeof(meta), "%s\\SaveMeta.xbx", sdir);
    char mbuf[XBL_SAVE_NAME_MAX + 16];
    int mn = snprintf(mbuf, sizeof(mbuf), "Name=%s\r\n", (name && name[0]) ? name : slot);
    int rc = write_file(meta, mbuf, (size_t)mn);
    if (rc != XBL_OK) {
        return rc;
    }

    /* data.bin: the raw game payload. */
    char dpath[MAX_PATH];
    snprintf(dpath, sizeof(dpath), "%s\\data.bin", sdir);
    return write_file(dpath, data, len);
}

int xbl_save_read(const char *slot, void *buf, size_t bufsz, size_t *out_len)
{
    if (out_len) {
        *out_len = 0;
    }
    if (!slot_ok(slot) || !buf) {
        return XBL_ERR_CONFIG;
    }
    char tdir[MAX_PATH];
    title_dir(tdir, sizeof(tdir));
    char dpath[MAX_PATH];
    snprintf(dpath, sizeof(dpath), "%s\\%s\\data.bin", tdir, slot);

    HANDLE h = CreateFile(dpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return XBL_ERR_CONFIG;
    }
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)bufsz, &got, NULL);
    CloseHandle(h);
    if (!ok) {
        return XBL_ERR_CONFIG;
    }
    if (out_len) {
        *out_len = (size_t)got;
    }
    return XBL_OK;
}

/* Reads the Name= value from a SaveMeta.xbx (ASCII; first match). */
static void read_save_name(const char *sdir, char *out, size_t outsz)
{
    out[0] = '\0';
    char meta[MAX_PATH];
    snprintf(meta, sizeof(meta), "%s\\SaveMeta.xbx", sdir);
    HANDLE h = CreateFile(meta, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    char buf[256];
    DWORD got = 0;
    if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got) {
        buf[got] = '\0';
        const char *p = strstr(buf, "Name=");
        if (p) {
            p += 5;
            size_t i = 0;
            while (*p && *p != '\r' && *p != '\n' && i < outsz - 1) {
                out[i++] = *p++;
            }
            out[i] = '\0';
        }
    }
    CloseHandle(h);
}

int xbl_save_list(XblSaveInfo *out, int max, int *count)
{
    if (count) {
        *count = 0;
    }
    if (!out || max <= 0) {
        return XBL_ERR_CONFIG;
    }
    char tdir[MAX_PATH];
    title_dir(tdir, sizeof(tdir));
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", tdir);

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return XBL_OK; /* no saves yet */
    }
    int n = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }
        if (fd.cFileName[0] == '.') {
            continue;
        }
        if (n >= max) {
            break;
        }
        memset(&out[n], 0, sizeof(out[n]));
        strncpy(out[n].slot, fd.cFileName, XBL_SAVE_SLOT_MAX - 1);
        char sdir[MAX_PATH];
        snprintf(sdir, sizeof(sdir), "%s\\%s", tdir, fd.cFileName);
        read_save_name(sdir, out[n].name, XBL_SAVE_NAME_MAX);
        if (!out[n].name[0]) {
            strncpy(out[n].name, fd.cFileName, XBL_SAVE_NAME_MAX - 1);
        }
        n++;
    } while (FindNextFile(h, &fd));
    FindClose(h);
    if (count) {
        *count = n;
    }
    return XBL_OK;
}

int xbl_save_delete(const char *slot)
{
    if (!slot_ok(slot)) {
        return XBL_ERR_CONFIG;
    }
    char tdir[MAX_PATH];
    title_dir(tdir, sizeof(tdir));
    char sdir[MAX_PATH];
    snprintf(sdir, sizeof(sdir), "%s\\%s", tdir, slot);

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\data.bin", sdir);
    DeleteFile(path);
    snprintf(path, sizeof(path), "%s\\SaveMeta.xbx", sdir);
    DeleteFile(path);
    RemoveDirectory(sdir);
    return XBL_OK;
}
