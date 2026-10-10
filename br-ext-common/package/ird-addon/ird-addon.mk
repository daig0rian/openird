# SPDX-License-Identifier: GPL-2.0-only
################################################################################
# ird-addon — アドオン（実行時パッケージ）のインストーラ（docs/addon.md §5）
#
# ビルドはしない。files/ の sh スクリプトと、リポジトリを 1 つも持たない
# addons.conf を置くだけ。道具は BusyBox のアプレットだけを使うので、その
# アプレットが BusyBox に入っているかをここで確かめる（無いと機器の上で初めて
# 気づくことになる）。足すのは board の busybox.fragment。
#
# 機器の Arch id は board の /etc/openird/board.conf の ADDON_ARCH=（make sdk も
# 同じ値を SDK に渡す）。
################################################################################
IRD_ADDON_VERSION = 0.1.0
IRD_ADDON_SOURCE =
IRD_ADDON_LICENSE = GPL-2.0-only
IRD_ADDON_DEPENDENCIES = busybox

# FEATURE_WGET_HTTPS は Buildroot の既定では無効（取得は HTTPS・docs/addon.md §4）
IRD_ADDON_BUSYBOX_NEEDS = AWK FEATURE_WGET_HTTPS GUNZIP SHA256SUM TAR

define IRD_ADDON_INSTALL_TARGET_CMDS
	for o in $(IRD_ADDON_BUSYBOX_NEEDS); do \
		grep -q "^CONFIG_$$o=y" $(BUSYBOX_DIR)/.config || { \
			echo "ird-addon: BusyBox に CONFIG_$$o=y が要る（board の busybox.fragment に足すこと）"; \
			exit 1; }; \
	done
	$(INSTALL) -D -m 0755 $(IRD_ADDON_PKGDIR)/files/usr/sbin/ird-addon \
		$(TARGET_DIR)/usr/sbin/ird-addon
	$(INSTALL) -D -m 0644 $(IRD_ADDON_PKGDIR)/files/etc/openird/addons.conf \
		$(TARGET_DIR)/etc/openird/addons.conf
endef

$(eval $(generic-package))
