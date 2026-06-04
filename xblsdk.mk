# xblsdk.mk - drop-in source list for an nxdk project that links the xb.live SDK.
#
# Usage from your project's Makefile (before `include $(NXDK_DIR)/Makefile`):
#
#   XBLSDK_DIR := $(CURDIR)/XBLSDK        # path to this folder
#   include $(XBLSDK_DIR)/xblsdk.mk
#   SRCS += $(XBLSDK_SRCS)
#   CFLAGS += $(XBLSDK_CFLAGS)
#   NXDK_CFLAGS += $(XBLSDK_NXDK_CFLAGS)
#
# You must also build mbed TLS (TLS transport + HMAC/SHA-256) and point
# MBEDTLS_DIR at its checkout, and shadow nxdk's lwipopts.h with the bundled
# lwip_override/lwipopts.h (see README.md "Build / integrate").

XBLSDK_DIR ?= $(CURDIR)/XBLSDK

XBLSDK_SRCS := \
	$(XBLSDK_DIR)/sdk/xblsdk.c \
	$(XBLSDK_DIR)/sdk/xbl_net.c \
	$(XBLSDK_DIR)/sdk/xbl_save.c \
	$(XBLSDK_DIR)/sdk/xbl_crypto.c \
	$(XBLSDK_DIR)/sdk/base64.c \
	$(XBLSDK_DIR)/third_party/https_client.c \
	$(XBLSDK_DIR)/third_party/qrcodegen.c \
	$(XBLSDK_DIR)/third_party/nxdk_entropy.c \
	$(XBLSDK_DIR)/third_party/nxdk_mbedtls_time.c

XBLSDK_CFLAGS := -I$(XBLSDK_DIR)/sdk -I$(XBLSDK_DIR)/third_party

# mbed TLS config for the Xbox (weak entropy, no filesystem).
XBLSDK_NXDK_CFLAGS := \
	-I$(XBLSDK_DIR)/third_party \
	-DMBEDTLS_CONFIG_FILE=\"mbedtls_nxdk_config.h\"
