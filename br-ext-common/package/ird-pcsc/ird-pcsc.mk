# SPDX-License-Identifier: GPL-2.0-only
################################################################################
# ird-pcsc — カードリーダー（/dev/irdcard0）を PC/SC で使う: 静的な pcsc-lite
#
# pcsc-lite 2.1.0（Buildroot の pcsc-lite と同じ版・同じ tarball）を静的ドライバの
# 作り（-DPCSCLITE_STATIC_DRIVER -DIFDHANDLERv3）でビルドし、OpenIRD の IFD
# ハンドラ src/ifd_irdcard.c を pcscd にリンクする。rootfs は静的リンクで dlopen が
# 無いため（Buildroot の pcsc-lite は静的な構成を受け付けない）。pcsc-lite の
# ソースには手を入れない。docs/specs/emma3sep-card-reader.md §4。
#
# src/   IFD ハンドラ（pcscd に組み込む）と、確かめ用の ird-pcsc。
# files/ pcscd の起動スクリプト・reader.conf・LIBPATH の目印。
################################################################################
IRD_PCSC_VERSION = 2.1.0
IRD_PCSC_SOURCE = pcsc-lite-$(IRD_PCSC_VERSION).tar.bz2
IRD_PCSC_SITE = https://pcsclite.apdu.fr/files
# Buildroot の pcsc-lite と同じ置き場（ミラー sources.buildroot.net の場所も同じ）
IRD_PCSC_DL_SUBDIR = pcsc-lite
IRD_PCSC_INSTALL_STAGING = YES
IRD_PCSC_DEPENDENCIES = host-pkgconf host-flex
IRD_PCSC_LICENSE = BSD-2-Clause (auth), BSD-3-Clause, GPL-3.0+ (spy, tests), \
	ISC (simclist), GPL-2.0-only (ifd-irdcard, ird-pcsc)
IRD_PCSC_LICENSE_FILES = COPYING GPL-3.0.txt

IRD_PCSC_CONF_ENV = CFLAGS="$(TARGET_CFLAGS) -DPCSCLITE_STATIC_DRIVER -DIFDHANDLERv3"
IRD_PCSC_CONF_OPTS = \
	--disable-strict \
	--disable-libudev \
	--disable-libusb \
	--disable-usb \
	--disable-libsystemd \
	--disable-polkit

IRD_PCSC_CORE_SRC = $(BR2_EXTERNAL_IRD_COMMON_PATH)/package/ird-tuner-core/src
IRD_PCSC_INCLUDES = -I$(@D)/src/PCSC -I$(@D)/src -I$(IRD_PCSC_CORE_SRC)

# pcscd が呼ぶ IFDH* を、pcscd のリンクに足す
IRD_PCSC_LINK = pcscd_LDADD='$$(PTHREAD_LIBS) $(@D)/libifd_irdcard.a'
IRD_PCSC_MAKE_OPTS = $(IRD_PCSC_LINK)
IRD_PCSC_INSTALL_STAGING_OPTS = DESTDIR=$(STAGING_DIR) $(IRD_PCSC_LINK) install
IRD_PCSC_INSTALL_TARGET_OPTS = DESTDIR=$(TARGET_DIR) $(IRD_PCSC_LINK) install

define IRD_PCSC_BUILD_DRIVER
	$(TARGET_CC) $(TARGET_CFLAGS) -DIFDHANDLERv3 -Wall $(IRD_PCSC_INCLUDES) \
		-c -o $(@D)/ifd_irdcard.o $(IRD_PCSC_PKGDIR)/src/ifd_irdcard.c
	rm -f $(@D)/libifd_irdcard.a
	$(TARGET_AR) rcs $(@D)/libifd_irdcard.a $(@D)/ifd_irdcard.o
endef
IRD_PCSC_PRE_BUILD_HOOKS += IRD_PCSC_BUILD_DRIVER

# pcsc-lite 2.x の libpcsclite は、本体 libpcsclite_real を dlopen で読むリダイレクタで、
# 静的な構成では使えない。そのうえ pcsc_stringify_error と SCARD_PCI_* はリダイレクタの側に
# しか無い。そこで静的な使う側のために、本体 ＋ pcsc-lite の error.o ＋ src/pcsc_static.c
# （SCARD_PCI_* の 3 つ）を 1 つの libpcsclite.a にまとめる
define IRD_PCSC_BUILD_CLIENT_LIB
	rm -rf $(@D)/ird-client && mkdir -p $(@D)/ird-client
	cd $(@D)/ird-client && $(TARGET_AR) x $(@D)/src/.libs/libpcsclite.a error.o
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall $(IRD_PCSC_INCLUDES) \
		-c -o $(@D)/ird-client/pcsc_static.o $(IRD_PCSC_PKGDIR)/src/pcsc_static.c
	cp $(@D)/src/.libs/libpcsclite_real.a $(@D)/ird-client/libpcsclite.a
	$(TARGET_AR) rcs $(@D)/ird-client/libpcsclite.a \
		$(@D)/ird-client/error.o $(@D)/ird-client/pcsc_static.o
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall $(IRD_PCSC_INCLUDES) \
		-o $(@D)/ird-pcsc $(IRD_PCSC_PKGDIR)/src/ird-pcsc.c \
		$(@D)/ird-client/libpcsclite.a -lpthread
endef
IRD_PCSC_POST_BUILD_HOOKS += IRD_PCSC_BUILD_CLIENT_LIB

# SDK（staging）の -lpcsclite はこのまとめたもの（pkg-config の libpcsclite.pc のまま使える）
define IRD_PCSC_STAGING_CLIENT_LIB
	cp -f $(@D)/ird-client/libpcsclite.a $(STAGING_DIR)/usr/lib/libpcsclite.a
endef
IRD_PCSC_POST_INSTALL_STAGING_HOOKS += IRD_PCSC_STAGING_CLIENT_LIB

define IRD_PCSC_INSTALL_OPENIRD
	$(INSTALL) -D -m 0755 $(@D)/ird-pcsc $(TARGET_DIR)/usr/bin/ird-pcsc
	$(INSTALL) -D -m 0755 $(IRD_PCSC_PKGDIR)/files/etc/init.d/S31pcscd \
		$(TARGET_DIR)/etc/init.d/S31pcscd
	$(INSTALL) -D -m 0644 $(IRD_PCSC_PKGDIR)/files/etc/reader.conf.d/irdcard.conf \
		$(TARGET_DIR)/etc/reader.conf.d/irdcard.conf
	$(INSTALL) -D -m 0644 $(IRD_PCSC_PKGDIR)/files/usr/lib/pcsc/drivers/irdcard.builtin \
		$(TARGET_DIR)/usr/lib/pcsc/drivers/irdcard.builtin
endef
IRD_PCSC_POST_INSTALL_TARGET_HOOKS += IRD_PCSC_INSTALL_OPENIRD

$(eval $(autotools-package))
