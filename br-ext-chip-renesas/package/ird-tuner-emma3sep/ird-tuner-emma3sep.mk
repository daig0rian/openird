# SPDX-License-Identifier: GPL-2.0-only
################################################################################
# ird-tuner-emma3sep — EMMA3SEP の TS 取り出しと機種ごとのフロントエンド
#
# src/   カーネル側（コマンド経路・FIFO ドレイナー・共通のチューナ・機種ごとのフロントエンド）。
#        drivers/ird/ へ重ねる。
# files/ rootfs に置く起動スクリプト。
################################################################################
IRD_TUNER_EMMA3SEP_VERSION = 0.1.0
IRD_TUNER_EMMA3SEP_SOURCE =
IRD_TUNER_EMMA3SEP_LICENSE = GPL-2.0-only
IRD_TUNER_EMMA3SEP_DEPENDENCIES = busybox

define IRD_TUNER_EMMA3SEP_INSTALL_TARGET_CMDS
	$(call SYSTEM_RSYNC,$(IRD_TUNER_EMMA3SEP_PKGDIR)/files,$(TARGET_DIR))
	chmod 0755 $(TARGET_DIR)/etc/init.d/S30tuner
endef

$(eval $(generic-package))
