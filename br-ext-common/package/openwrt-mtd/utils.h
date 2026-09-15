/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * libubox の utils.h の代替（OpenIRD）。
 * openwrt-mtd が取り込む libubox md5.c は utils.h から __BYTE_ORDER /
 * __LITTLE_ENDIAN だけを使うので、それを定義する <endian.h> だけを読む。
 */
#ifndef OPENIRD_LIBUBOX_UTILS_SHIM_H
#define OPENIRD_LIBUBOX_UTILS_SHIM_H

#include <endian.h>

#endif
