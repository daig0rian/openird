# SPDX-License-Identifier: GPL-2.0-only
################################################################################
# ird-base-files — OpenIRD 共通の rootfs スケルトン（init・overlay 組み立て・ネットワーク）
#
# ダウンロードもビルドもせず files/ を配置するだけ。busybox の後に入れる必要がある
# （/sbin/init を busybox の symlink から preinit に置き換えるため）。
################################################################################
IRD_BASE_FILES_VERSION = 0.1.0
IRD_BASE_FILES_SOURCE =
IRD_BASE_FILES_LICENSE = GPL-2.0-only
IRD_BASE_FILES_DEPENDENCIES = busybox

define IRD_BASE_FILES_INSTALL_TARGET_CMDS
	$(call SYSTEM_RSYNC,$(IRD_BASE_FILES_PKGDIR)/files,$(TARGET_DIR))
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/overlay $(TARGET_DIR)/rom
	chmod 0755 $(TARGET_DIR)/sbin/openird-preinit $(TARGET_DIR)/etc/init.d/S40network \
		$(TARGET_DIR)/etc/init.d/S99ready $(TARGET_DIR)/usr/sbin/ird-led \
		$(TARGET_DIR)/sbin/sysupgrade $(TARGET_DIR)/lib/upgrade/upgraded
	rm -f $(TARGET_DIR)/sbin/init
	ln -s openird-preinit $(TARGET_DIR)/sbin/init
endef

$(eval $(generic-package))
