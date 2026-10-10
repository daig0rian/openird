# SPDX-License-Identifier: GPL-2.0-only
################################################################################
# ird-tuner-mxl683 — MaxLinear MXL683（ISDB-T 復調器）
#
# src/   カーネル側（冷起動・ファーム転送・選局・ロック・C/N）。drivers/ird/ へ重ねる。
# tools/ ird-mbin = ファイルを走査して MBIN 容器を取り出す（docs/specs/mbin-firmware-container.md）。
################################################################################
IRD_TUNER_MXL683_VERSION = 0.1.0
IRD_TUNER_MXL683_SITE = $(IRD_TUNER_MXL683_PKGDIR)
IRD_TUNER_MXL683_SITE_METHOD = local
IRD_TUNER_MXL683_LICENSE = GPL-2.0-only

define IRD_TUNER_MXL683_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall $(TARGET_LDFLAGS) \
		-o $(@D)/ird-mbin $(@D)/tools/ird-mbin.c
endef

define IRD_TUNER_MXL683_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/ird-mbin $(TARGET_DIR)/usr/sbin/ird-mbin
endef

$(eval $(generic-package))
