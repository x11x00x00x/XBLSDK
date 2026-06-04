# xblsdk — xb.live Homebrew SDK

# Play the Test Game to see full feature set in action. Download in the release section --->>>

A reusable **C / [nxdk](https://github.com/XboxDev/nxdk)** SDK for the original Xbox
that lets homebrew games sign in to an **Insignia / xb.live** account via on-screen
**QR code**, manage online sessions, and talk to the xb.live homebrew backend:
**verified leaderboards**, **verified achievements**, **cloud saves**, **presence**,
**friends**, and **multiplayer** — over HMAC-signed HTTPS.

Scores and achievements are **proven, not trusted**: the client uploads a run's
server-issued seed plus a compact input log, and the server re-runs the identical
deterministic simulation before accepting anything. See
[`docs/anti-cheat.md`](docs/anti-cheat.md).

> Not affiliated with Microsoft or Insignia. Networking, QR, session storage, and
> framebuffer/font helpers are generalized from the OG-XBL Cloud Save Backup app.

## What's here

| Path | Description |
|------|-------------|
| `sdk/xblsdk.{c,h}` | Public API: init, QR login, sessions, run start, HMAC-signed score submit, leaderboard fetch, input-log recorder, achievements. |
| `sdk/xbl_net.{c,h}` | Networking helpers (lwIP sockets). |
| `sdk/xbl_save.c` | Cloud save read/write + XBE title/cert helpers. |
| `sdk/xbl_crypto.c` | HMAC-SHA-256 signing (mbed TLS). |
| `sdk/base64.{c,h}` | Base64 encode/decode. |
| `third_party/https_client.{c,h}` | HTTPS client (lwIP + mbed TLS). |
| `third_party/qrcodegen.{c,h}` | QR code generation (login QR rendering). |
| `third_party/mbedtls_nxdk_config.h` | mbed TLS build config for the Xbox (weak entropy, no filesystem). |
| `third_party/nxdk_entropy.c` | mbed TLS entropy source for nxdk. |
| `third_party/nxdk_mbedtls_time.c` | mbed TLS time shim for nxdk. |
| `lwip_override/lwipopts.h` | Shadows nxdk's lwipopts so lwIP doesn't draw debug text over the QR code. |
| `docs/` | SDK reference, REST/HMAC spec, anti-cheat write-up, feature & integration guides. |

## External dependencies (not bundled)

These are large upstream toolchain/library checkouts; clone them yourself:

- **[nxdk](https://github.com/XboxDev/nxdk)** — the original-Xbox homebrew toolchain
  (provides `windows.h`, `lwip/*`, `hal/*`, `nxdk/net.h`, the build system).
- **[mbed TLS](https://github.com/Mbed-TLS/mbedtls)** — TLS transport plus
  HMAC/SHA-256 for score signing. Exclude files needing a host filesystem / POSIX
  sockets (`net_sockets.c`, `psa_its_file.c`, `psa_crypto_storage.c`).

## Build / integrate

This SDK compiles into your game's XBE as part of an nxdk build. From your
project's `Makefile`, before `include $(NXDK_DIR)/Makefile`:

```make
XBLSDK_DIR := $(CURDIR)/XBLSDK
include $(XBLSDK_DIR)/xblsdk.mk

SRCS        += $(XBLSDK_SRCS)
CFLAGS      += $(XBLSDK_CFLAGS)
NXDK_CFLAGS += $(XBLSDK_NXDK_CFLAGS)

# mbed TLS sources (exclude host-only files)
MBEDTLS_DIR ?= /path/to/mbedtls
MBEDTLS_SRCS := $(filter-out %/net_sockets.c %/psa_its_file.c %/psa_crypto_storage.c,\
	$(wildcard $(MBEDTLS_DIR)/library/*.c))
SRCS        += $(MBEDTLS_SRCS)
NXDK_CFLAGS += -I$(MBEDTLS_DIR)/include
NXDK_NET = y
```

After `include $(NXDK_DIR)/Makefile`, shadow nxdk's lwipopts and undefine
`_WIN32` for two mbed TLS files that would otherwise pull in Windows-only headers:

```make
NXDK_CFLAGS := -I$(XBLSDK_DIR)/lwip_override $(NXDK_CFLAGS)
NXDK_CFLAGS += -DNXDK_REAL_LWIPOPTS='"$(NXDK_DIR)/lib/net/nforceif/include/lwipopts.h"'
$(MBEDTLS_DIR)/library/platform_util.obj: NXDK_CFLAGS += -U_WIN32
$(MBEDTLS_DIR)/library/x509_crt.obj:      NXDK_CFLAGS += -U_WIN32
```

A complete, working example of all of the above lives in the Test Game repo's
top-level `Makefile`.

## Quick start (API)

See [`docs/integration-quickstart.md`](docs/integration-quickstart.md) and the
full [`docs/sdk-reference.md`](docs/sdk-reference.md). In short:

1. `xbl_init()` with your game id, signing secret, and status callback.
2. `xbl_login_*()` to show the QR and poll until the device flow completes.
3. `xbl_run_start()` to get a seed + nonce, play a deterministic run, record inputs.
4. `xbl_score_submit()` to upload the HMAC-signed run for server-side replay
   verification, then `xbl_leaderboard_fetch()` to display the board.

## Documentation

- [SDK reference](docs/sdk-reference.md) — full API.
- [REST / HMAC spec](docs/rest-api.md) — wire protocol for any toolchain.
- [Anti-cheat](docs/anti-cheat.md) — how replay verification works (and its limits).
- [Achievements](docs/achievements.md), [Saves](docs/saves.md),
  [Presence](docs/presence.md), [Friends](docs/friends.md),
  [Multiplayer](docs/multiplayer.md), [Single-player](docs/single-player.md),
  [Story mode](docs/story-mode.md).
- [Approval workflow](docs/approval-workflow.md) — register a game and get a secret.

## License

No license file is included yet. Add one before publishing publicly. Note that
bundled `third_party/` sources (e.g. `qrcodegen`) carry their own upstream
licenses — preserve their headers.
