// SPDX-License-Identifier: GPL-2.0-only
/*
 * The protocol control information of the PC/SC API (SCARD_PCI_T0, _T1,
 * _RAW), for statically linked clients.
 *
 * pcsc-lite 2.x defines these only in its libpcsclite, which redirects to
 * the real client library (libpcsclite_real) through dlopen - unusable in a
 * static program.  OpenIRD's static libpcsclite.a is libpcsclite_real plus
 * pcsc-lite's error.o plus this.  docs/specs/emma3sep-card-reader.md §4.
 */

#include <winscard.h>

const SCARD_IO_REQUEST g_rgSCardT0Pci = { SCARD_PROTOCOL_T0, sizeof(SCARD_IO_REQUEST) };
const SCARD_IO_REQUEST g_rgSCardT1Pci = { SCARD_PROTOCOL_T1, sizeof(SCARD_IO_REQUEST) };
const SCARD_IO_REQUEST g_rgSCardRawPci = { SCARD_PROTOCOL_RAW, sizeof(SCARD_IO_REQUEST) };
