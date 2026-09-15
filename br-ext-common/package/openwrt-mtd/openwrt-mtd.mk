# SPDX-License-Identifier: GPL-2.0-only
################################################################################
# openwrt-mtd — OpenWrt の mtd ユーティリティ（package/system/mtd/src）
#
# OpenWrt 本体リポジトリ全体を取らず、必要なソースだけを commit 固定の URL から
# ファイル単位で取得する（全ファイルを openwrt-mtd.hash で検証）。libubox からは
# md5（md5_begin/md5_hash/md5_end/md5sum）だけを使うので md5.c/md5.h のみ取り込み、
# libubox の utils.h は endian 定義だけの shim で置き換える。
# ボード固有の trx/seama/wrg 等は取り込まない（mtd.h の weak シンボルが未解決=無効になる）。
################################################################################
OPENWRT_MTD_VERSION = dd62e3af151d4a92aa8251148ec9b88fa558be73
OPENWRT_MTD_SITE = https://raw.githubusercontent.com/openwrt/openwrt/$(OPENWRT_MTD_VERSION)/package/system/mtd/src
OPENWRT_MTD_SOURCE = mtd.c
OPENWRT_MTD_SRC_EXTRA = mtd.h jffs2.c jffs2.h crc32.c crc32.h fis.h

OPENWRT_MTD_LIBUBOX_VERSION = 1501e60e5554bd206c9b13532b7352e668508420
OPENWRT_MTD_LIBUBOX_SITE = https://raw.githubusercontent.com/openwrt/libubox/$(OPENWRT_MTD_LIBUBOX_VERSION)

OPENWRT_MTD_EXTRA_DOWNLOADS = \
	$(OPENWRT_MTD_SRC_EXTRA) \
	$(OPENWRT_MTD_LIBUBOX_SITE)/md5.c \
	$(OPENWRT_MTD_LIBUBOX_SITE)/md5.h

# mtd 本体: GPL-2.0+（OpenWrt package Makefile の PKG_LICENSE）。
# libubox md5.c: ISC（ラッパ）＋ public domain（Solar Designer の MD5 実装）。
OPENWRT_MTD_LICENSE = GPL-2.0+, ISC, Public Domain

define OPENWRT_MTD_EXTRACT_CMDS
	mkdir -p $(@D)/libubox
	cp $(addprefix $(OPENWRT_MTD_DL_DIR)/,$(OPENWRT_MTD_SOURCE) $(OPENWRT_MTD_SRC_EXTRA)) $(@D)/
	cp $(OPENWRT_MTD_DL_DIR)/md5.c $(OPENWRT_MTD_DL_DIR)/md5.h $(@D)/libubox/
	cp $(OPENWRT_MTD_PKGDIR)/utils.h $(@D)/libubox/utils.h
endef

define OPENWRT_MTD_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -I$(@D) $(TARGET_LDFLAGS) \
		-o $(@D)/mtd \
		$(@D)/mtd.c $(@D)/jffs2.c $(@D)/crc32.c $(@D)/libubox/md5.c
endef

define OPENWRT_MTD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/mtd $(TARGET_DIR)/sbin/mtd
endef

$(eval $(generic-package))
