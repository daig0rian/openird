// SPDX-License-Identifier: GPL-2.0-only
/*
 * ifd-irdcard: the pcsc-lite driver (IFD Handler API v3) for an OpenIRD
 * card reader, /dev/irdcard0 (ird_card_ioctl.h).
 *
 * Built into pcscd: pcsc-lite is compiled with PCSCLITE_STATIC_DRIVER and
 * IFDHANDLERv3, and pcscd calls these functions directly (the root
 * filesystem is static; there is no dlopen).  One reader, one slot.
 *
 * The reader runs the transmission protocol (T=1) itself, so APDUs go
 * through as they are; nothing here looks at what they say.
 * docs/specs/emma3sep-card-reader.md §4.
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <ifdhandler.h>
#include <reader.h>

#include "ird_card_ioctl.h"

#define DEFAULT_DEVICE	"/dev/irdcard0"
#define OPEN_TRIES	10		/* EAGAIN: the tuner is still starting */

static int fd = -1;
static UCHAR atr[MAX_ATR_SIZE];
static DWORD atr_len;
static unsigned int changes;		/* IRD_CARD_STATUS changes, last seen */
static int changes_known;

RESPONSECODE IFDHCreateChannelByName(DWORD Lun, LPSTR DeviceName)
{
	int i;

	(void)Lun;
	if (fd >= 0)
		return IFD_SUCCESS;
	for (i = 0; i < OPEN_TRIES; i++) {
		fd = open(DeviceName, O_RDWR);
		if (fd >= 0 || errno != EAGAIN)
			break;
		sleep(1);
	}
	if (fd < 0)
		return errno == ENOENT ? IFD_NO_SUCH_DEVICE : IFD_COMMUNICATION_ERROR;
	atr_len = 0;
	changes_known = 0;
	return IFD_SUCCESS;
}

RESPONSECODE IFDHCreateChannel(DWORD Lun, DWORD Channel)
{
	(void)Channel;
	return IFDHCreateChannelByName(Lun, (LPSTR)DEFAULT_DEVICE);
}

RESPONSECODE IFDHCloseChannel(DWORD Lun)
{
	(void)Lun;
	if (fd >= 0)
		close(fd);
	fd = -1;
	return IFD_SUCCESS;
}

static RESPONSECODE one_byte(PDWORD Length, PUCHAR Value, UCHAR v)
{
	if (*Length < 1)
		return IFD_ERROR_INSUFFICIENT_BUFFER;
	*Length = 1;
	Value[0] = v;
	return IFD_SUCCESS;
}

RESPONSECODE IFDHGetCapabilities(DWORD Lun, DWORD Tag, PDWORD Length,
	PUCHAR Value)
{
	(void)Lun;
	switch (Tag) {
	case TAG_IFD_ATR:
	case SCARD_ATTR_ATR_STRING:
		if (*Length < atr_len)
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		memcpy(Value, atr, atr_len);
		*Length = atr_len;
		return IFD_SUCCESS;
	case TAG_IFD_SIMULTANEOUS_ACCESS:
	case TAG_IFD_SLOTS_NUMBER:
		return one_byte(Length, Value, 1);
	case TAG_IFD_THREAD_SAFE:
	case TAG_IFD_SLOT_THREAD_SAFE:
		return one_byte(Length, Value, 0);
	}
	return IFD_ERROR_TAG;
}

RESPONSECODE IFDHSetCapabilities(DWORD Lun, DWORD Tag, DWORD Length,
	PUCHAR Value)
{
	(void)Lun; (void)Tag; (void)Length; (void)Value;
	return IFD_NOT_SUPPORTED;
}

RESPONSECODE IFDHSetProtocolParameters(DWORD Lun, DWORD Protocol, UCHAR Flags,
	UCHAR PTS1, UCHAR PTS2, UCHAR PTS3)
{
	(void)Lun; (void)Flags; (void)PTS1; (void)PTS2; (void)PTS3;
	/* the reader speaks T=1 with the card on its own */
	return Protocol == SCARD_PROTOCOL_T1 ? IFD_SUCCESS : IFD_PROTOCOL_NOT_SUPPORTED;
}

RESPONSECODE IFDHPowerICC(DWORD Lun, DWORD Action, PUCHAR Atr, PDWORD AtrLength)
{
	struct ird_card_atr a;

	(void)Lun;
	if (fd < 0)
		return IFD_NO_SUCH_DEVICE;
	switch (Action) {
	case IFD_POWER_DOWN:
		atr_len = 0;
		*AtrLength = 0;
		ioctl(fd, IRD_CARD_POWER_OFF);
		return IFD_SUCCESS;
	case IFD_POWER_UP:
	case IFD_RESET:
		atr_len = 0;
		*AtrLength = 0;
		memset(&a, 0, sizeof(a));
		if (ioctl(fd, IRD_CARD_RESET, &a) < 0)
			return errno == ETIMEDOUT ? IFD_RESPONSE_TIMEOUT
						  : IFD_ERROR_POWER_ACTION;
		if (!a.len || a.len > MAX_ATR_SIZE)
			return IFD_ERROR_POWER_ACTION;
		memcpy(atr, a.atr, a.len);
		atr_len = a.len;
		memcpy(Atr, atr, atr_len);
		*AtrLength = atr_len;
		return IFD_SUCCESS;
	}
	return IFD_NOT_SUPPORTED;
}

RESPONSECODE IFDHTransmitToICC(DWORD Lun, SCARD_IO_HEADER SendPci,
	PUCHAR TxBuffer, DWORD TxLength, PUCHAR RxBuffer, PDWORD RxLength,
	PSCARD_IO_HEADER RecvPci)
{
	static struct ird_card_xfer x;
	DWORD room = *RxLength;

	(void)Lun;
	*RxLength = 0;
	if (fd < 0)
		return IFD_NO_SUCH_DEVICE;
	if (!TxLength || TxLength > IRD_CARD_TX_MAX)
		return IFD_COMMUNICATION_ERROR;
	memset(&x, 0, sizeof(x));
	x.tx_len = TxLength;
	memcpy(x.tx, TxBuffer, TxLength);
	if (ioctl(fd, IRD_CARD_TRANSMIT, &x) < 0) {
		switch (errno) {
		case ENOMEDIUM:
			return IFD_ICC_NOT_PRESENT;
		case ETIMEDOUT:
			return IFD_RESPONSE_TIMEOUT;
		}
		return IFD_COMMUNICATION_ERROR;
	}
	if (x.rx_len > room)
		return IFD_ERROR_INSUFFICIENT_BUFFER;
	memcpy(RxBuffer, x.rx, x.rx_len);
	*RxLength = x.rx_len;
	if (RecvPci) {
		RecvPci->Protocol = SendPci.Protocol;
		RecvPci->Length = 0;
	}
	return IFD_SUCCESS;
}

RESPONSECODE IFDHControl(DWORD Lun, DWORD dwControlCode, PUCHAR TxBuffer,
	DWORD TxLength, PUCHAR RxBuffer, DWORD RxLength,
	LPDWORD pdwBytesReturned)
{
	(void)Lun; (void)dwControlCode; (void)TxBuffer; (void)TxLength;
	(void)RxBuffer; (void)RxLength;
	*pdwBytesReturned = 0;
	return IFD_ERROR_NOT_SUPPORTED;
}

RESPONSECODE IFDHICCPresence(DWORD Lun)
{
	struct ird_card_status s;
	int missed;

	(void)Lun;
	if (fd < 0)
		return IFD_NO_SUCH_DEVICE;
	if (ioctl(fd, IRD_CARD_STATUS, &s) < 0)
		return IFD_COMMUNICATION_ERROR;
	/* out and back in between two polls: say "absent" once, so that
	 * pcscd starts over with the card */
	missed = changes_known && s.changes != changes && s.present;
	changes = s.changes;
	changes_known = 1;
	if (missed || !s.present) {
		atr_len = 0;
		return IFD_ICC_NOT_PRESENT;
	}
	return IFD_ICC_PRESENT;
}
