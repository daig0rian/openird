// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host unit test for ird_pidalloc.c: the PID -> filter-table assignment that
 * keeps PAT/CAT/PMT on the tapped device and drops in a defined order when
 * the filters run out.  Pure logic, no device.
 */
#include <stdio.h>
#include <string.h>
#include <linux/types.h>
#include "ird_pidalloc.h"

static int fails;
#define CHECK(name, cond) do { \
	printf("  %s %s\n", (cond) ? "ok  " : "FAIL", name); \
	if (!(cond)) fails++; \
} while (0)

static struct ird_pidalloc a;

static int in_group(int g, unsigned pid)
{
	int i;
	for (i = 0; i < a.n[g]; i++)
		if (a.g[g][i] == pid)
			return 1;
	return 0;
}
static int in0(unsigned pid) { return in_group(0, pid); }
static int in1(unsigned pid) { return in_group(1, pid); }

static int bounded(int per)
{
	return a.n[0] >= 0 && a.n[0] <= per && a.n[1] >= 0 && a.n[1] <= per;
}

static void test_basic(void)
{
	u16 must[] = {0x0000, 0x0001, 0x1000};
	u16 heavy[] = {0x0100, 0x0110, 0x01f0};
	u16 light[] = {0x0010, 0x0011, 0x0900, 0x0901};

	printf("[pidalloc] basic: must on tapped, heavy on DMA, nothing dropped\n");
	ird_pidalloc_run(&a, 36, must, 3, heavy, 3, light, 4);
	CHECK("within bounds", bounded(36));
	CHECK("nothing dropped", a.dropped == 0 && a.musttap_dropped == 0);
	CHECK("PAT on tapped (group 0)", in0(0x0000));
	CHECK("CAT on tapped", in0(0x0001));
	CHECK("PMT on tapped", in0(0x1000));
	CHECK("video on DMA (group 1)", in1(0x0100));
	CHECK("audio on DMA", in1(0x0110));
	CHECK("PCR on DMA", in1(0x01f0));
	CHECK("light carried somewhere", in0(0x0900) || in1(0x0900));
}

static void test_dedup(void)
{
	/* ird_psi_pids() returns PAT/CAT/PMT inside `light' too; they must not
	 * be placed twice. */
	u16 must[] = {0x0000, 0x0001, 0x1000};
	u16 heavy[] = {0x0100};
	u16 light[] = {0x0000, 0x0001, 0x0010, 0x1000, 0x0900};
	int total;

	printf("[pidalloc] de-duplicates PIDs that appear in several inputs\n");
	ird_pidalloc_run(&a, 36, must, 3, heavy, 1, light, 5);
	total = a.n[0] + a.n[1];
	CHECK("no double placement (6 distinct PIDs)", total == 6);
	CHECK("PAT placed once on tapped only", in0(0x0000) && !in1(0x0000));
}

static void test_heavy_spill(void)
{
	u16 must[] = {0x0000, 0x0001};
	u16 heavy[] = {0x0100, 0x0101, 0x0102, 0x0103, 0x0104, 0x0105};
	int i, in_dma = 0;

	printf("[pidalloc] heavy overflow spills from DMA to the tapped device\n");
	ird_pidalloc_run(&a, 4, must, 2, heavy, 6, NULL, 0);
	CHECK("within bounds (per_group 4)", bounded(4));
	CHECK("nothing dropped (6 heavy + 2 must = 8 <= 8)", a.dropped == 0);
	CHECK("must still on tapped", in0(0x0000) && in0(0x0001));
	for (i = 0; i < 6; i++)
		in_dma += in1(0x0100 + i);
	CHECK("DMA group filled first (4 heavy on it)", in_dma == 4);
}

static void test_musttap_preserved(void)
{
	u16 must[] = {0x0000, 0x0001, 0x1000};
	u16 heavy[] = {0x0100, 0x0110, 0x01f0};
	u16 light[16];
	int i;

	printf("[pidalloc] a flood of light PIDs never pushes must-tap off the tapped device\n");
	for (i = 0; i < 16; i++)
		light[i] = 0x2000 + i;
	ird_pidalloc_run(&a, 4, must, 3, heavy, 3, light, 16);
	CHECK("within bounds", bounded(4));
	CHECK("all must-tap still on tapped device", in0(0x0000) && in0(0x0001) && in0(0x1000));
	CHECK("no must-tap was dropped", a.musttap_dropped == 0);
	CHECK("the overflow is counted as dropped", a.dropped > 0);
}

static void test_musttap_overflow(void)
{
	/* pathological: more must-tap PIDs than the tapped device can hold.
	 * It cannot happen on the real board, but the accounting must be honest. */
	u16 must[] = {0x0000, 0x0001, 0x1000, 0x1001};

	printf("[pidalloc] honest accounting when must-tap itself overflows\n");
	ird_pidalloc_run(&a, 2, must, 4, NULL, 0, NULL, 0);
	CHECK("tapped device holds exactly per_group", a.n[0] == 2);
	CHECK("two must-tap dropped", a.musttap_dropped == 2);
	CHECK("must-tap never leaks onto the DMA device", a.n[1] == 0);
}

int main(void)
{
	test_basic();
	test_dedup();
	test_heavy_spill();
	test_musttap_preserved();
	test_musttap_overflow();
	printf(fails ? "\nFAIL: %d check(s) failed\n" : "\nok: all checks passed\n", fails);
	return fails ? 1 : 0;
}
