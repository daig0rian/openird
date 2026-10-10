# SPDX-License-Identifier: GPL-2.0-only
################################################################################
# ird-tuner-core — px4_drv 互換 chardev ABI（契約の本体）
#
# src/   カーネル側（chardev・PSI から PID を列挙する部品）。Buildroot では建てず、
#        scripts/kernel-overlay.sh がカーネルツリーの drivers/ird/ へ重ねる。
# tools/ ユーザランド側。ird-rec = chardev を開いて選局し、TS を読み出す確認用クライアント。
################################################################################
IRD_TUNER_CORE_VERSION = 0.2.0
IRD_TUNER_CORE_SITE = $(IRD_TUNER_CORE_PKGDIR)
IRD_TUNER_CORE_SITE_METHOD = local
IRD_TUNER_CORE_LICENSE = GPL-2.0-only

define IRD_TUNER_CORE_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -I$(@D)/src $(TARGET_LDFLAGS) \
		-o $(@D)/ird-rec $(@D)/tools/ird-rec.c -lm
endef

define IRD_TUNER_CORE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/ird-rec $(TARGET_DIR)/usr/bin/ird-rec
endef

$(eval $(generic-package))
