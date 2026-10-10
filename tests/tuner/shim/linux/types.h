/* SPDX-License-Identifier: GPL-2.0-only */
/* Host stand-in for the kernel header of the same name (tests/tuner). */
#ifndef SHIM_LINUX_TYPES_H
#define SHIM_LINUX_TYPES_H
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
#endif
