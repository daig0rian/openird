// SPDX-License-Identifier: GPL-2.0-only
/*
 * ird-pcsc: look at the card readers pcscd knows, through the PC/SC API.
 *
 *   ird-pcsc                 the readers, whether a card is in, its ATR
 *   ird-pcsc -x <hex>        send one APDU to the card in the first reader
 *                            and print the response as it comes back
 *   ird-pcsc -r              have the card in the first reader reset (warm)
 *
 * A check that the reader works; it does not interpret what a card says.
 * docs/specs/emma3sep-card-reader.md §4.  Messages are ASCII only.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <winscard.h>

static void hex(const char *label, const unsigned char *b, unsigned long n)
{
	unsigned long i;

	printf("%s", label);
	for (i = 0; i < n; i++)
		printf(" %02X", b[i]);
	printf("\n");
}

static int fail(const char *what, LONG rv)
{
	fprintf(stderr, "ird-pcsc: %s: %s (0x%08lX)\n", what,
		pcsc_stringify_error(rv), (unsigned long)rv);
	return 1;
}

static int parse_hex(const char *s, unsigned char *b, unsigned long max,
		     unsigned long *n)
{
	unsigned int v;

	*n = 0;
	while (*s) {
		if (isspace((unsigned char)*s) || *s == ':') {
			s++;
			continue;
		}
		if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1]) ||
		    *n == max || sscanf(s, "%2x", &v) != 1)
			return -1;
		b[(*n)++] = v;
		s += 2;
	}
	return *n ? 0 : -1;
}

int main(int argc, char **argv)
{
	SCARDCONTEXT ctx;
	SCARDHANDLE card;
	char *readers = NULL, *r;
	DWORD len = SCARD_AUTOALLOCATE, proto;
	SCARD_READERSTATE st;
	unsigned char apdu[261], resp[258];
	unsigned long apdu_len = 0;
	DWORD resp_len;
	LONG rv;
	int ret = 0;
	int warm = argc == 2 && !strcmp(argv[1], "-r");

	if (warm) {
		apdu_len = 0;
	} else if (argc == 3 && !strcmp(argv[1], "-x")) {
		if (parse_hex(argv[2], apdu, sizeof(apdu), &apdu_len)) {
			fprintf(stderr, "ird-pcsc: not an APDU in hex: %s\n", argv[2]);
			return 2;
		}
	} else if (argc != 1) {
		fprintf(stderr, "usage: ird-pcsc [-x <APDU in hex> | -r]\n");
		return 2;
	}

	rv = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &ctx);
	if (rv != SCARD_S_SUCCESS)
		return fail("cannot talk to pcscd", rv);
	rv = SCardListReaders(ctx, NULL, (LPSTR)&readers, &len);
	if (rv != SCARD_S_SUCCESS) {
		ret = fail("no readers", rv);
		goto out;
	}

	if (!apdu_len && !warm) {
		for (r = readers; *r; r += strlen(r) + 1) {
			memset(&st, 0, sizeof(st));
			st.szReader = r;
			st.dwCurrentState = SCARD_STATE_UNAWARE;
			rv = SCardGetStatusChange(ctx, 0, &st, 1);
			if (rv != SCARD_S_SUCCESS) {
				ret = fail(r, rv);
				continue;
			}
			printf("%s: %s\n", r, st.dwEventState & SCARD_STATE_PRESENT ?
			       "card present" : "no card");
			if (st.dwEventState & SCARD_STATE_PRESENT && st.cbAtr)
				hex("  ATR:", st.rgbAtr, st.cbAtr);
		}
		goto out;
	}

	rv = SCardConnect(ctx, readers, SCARD_SHARE_SHARED, SCARD_PROTOCOL_T1,
			  &card, &proto);
	if (rv != SCARD_S_SUCCESS) {
		ret = fail("cannot connect to the card", rv);
		goto out;
	}
	if (warm) {
		rv = SCardReconnect(card, SCARD_SHARE_SHARED, SCARD_PROTOCOL_T1,
				    SCARD_RESET_CARD, &proto);
		if (rv == SCARD_S_SUCCESS)
			printf("reset\n");
		else
			ret = fail("reset", rv);
		SCardDisconnect(card, SCARD_LEAVE_CARD);
		goto out;
	}
	hex("> ", apdu, apdu_len);
	resp_len = sizeof(resp);
	rv = SCardTransmit(card, SCARD_PCI_T1, apdu, apdu_len, NULL, resp, &resp_len);
	if (rv == SCARD_S_SUCCESS)
		hex("< ", resp, resp_len);
	else
		ret = fail("transmit", rv);
	SCardDisconnect(card, SCARD_LEAVE_CARD);
out:
	if (readers)
		SCardFreeMemory(ctx, readers);
	SCardReleaseContext(ctx);
	return ret;
}
