// SPDX-License-Identifier: GPL-2.0-only
/*
 * AirTuner boards (Renesas EMMA3SEP): what the vendor's kernel leaves out.
 *
 * The GPL kernel knows the SoC but not these boards.  It has no LEDs, and
 * its restart jumps to the reset vector, where a board that boots from NAND
 * has nothing mapped - the jump only raises an exception and the system
 * keeps running.  (The watchdog is no way out either: it raises an NMI, and
 * that goes to the same vector.)
 *
 * The wiring is the one the boards' GPL boot loader documents, the same on
 * the five boards it covers (docs/specs/emma3sep-airtuner-gpio.md §2):
 *
 *   GPIO 19  LED1_A  status LED, green  \
 *   GPIO 20  LED1_B  status LED, red     | low = lit, both = orange
 *   GPIO 21  LED2_A  power LED, green    |
 *   GPIO 22  LED2_B  power LED, red     /
 *   GPIO 30  SELF_RST_EMMA_L   low resets the whole board, the AV sub-CPU
 *                              included; the boot loader leaves it high
 *
 * PIX_AIRTUNER_BR310L (SB-TV06-WRIP) is not in the GPL sources.  On it the
 * two LED pairs are the other way round, and nothing in the GPL kernel
 * switches its USB power on: GPIO 36, which the GPL EHCI driver raises only
 * for the boards it names (seen on the device, §7).
 *
 * LEDs appear as /sys/class/leds/ird:<colour>:<power|status>.  The boot
 * loader leaves the power LED red; it stays red here until userland says the
 * system is up (ird-led).
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/platform_device.h>
#include <linux/leds.h>
#include <linux/gpio.h>
#include <linux/delay.h>

#include <asm/reboot.h>

#define AIRTUNER_GPIO_SELF_RST	30

/* LED order in the tables: status green, status red, power green, power red */
struct airtuner_board {
	const char *name;	/* what the boot loader passes in PRODUCT_FWIDS= */
	int led[4];
	int usb_power;		/* < 0: the GPL kernel's EHCI driver does it */
};

static const struct airtuner_board airtuner_boards[] __initdata = {
	{ "PIX_AIRTUNER_LITE",   { 19, 20, 21, 22 }, -1 },
	{ "PIX_AIRTUNER_R",      { 19, 20, 21, 22 }, -1 },
	{ "PIX_AIRTUNER_W",      { 19, 20, 21, 22 }, -1 },
	{ "NEC_AIRTUNER_R",      { 19, 20, 21, 22 }, -1 },
	{ "TOSHIBA_AIRTUNER_R",  { 19, 20, 21, 22 }, -1 },
	{ "PIX_AIRTUNER_BR310L", { 21, 22, 19, 20 }, 36 },
};

/*
 * Keep the names within 19 characters: this kernel cuts a device name there,
 * and a cut name is one userland does not find.
 */
/* the pins come from the board's table */
static struct gpio_led airtuner_leds[] = {
	{ .name = "ird:green:status", .active_low = 1 },
	{ .name = "ird:red:status",   .active_low = 1 },
	{ .name = "ird:green:power",  .active_low = 1 },
	{
		.name = "ird:red:power", .active_low = 1,
		/* registering an LED turns it off: put this one back on */
		.default_trigger = "default-on",
	},
};

static struct gpio_led_platform_data airtuner_leds_pdata = {
	.num_leds	= ARRAY_SIZE(airtuner_leds),
	.leds		= airtuner_leds,
};

static struct platform_device airtuner_leds_device = {
	.name	= "leds-gpio",
	.id	= -1,
	.dev	= {
		.platform_data = &airtuner_leds_pdata,
	},
};

static void (*airtuner_soc_restart)(char *command);

static void airtuner_restart(char *command)
{
	printk(KERN_INFO "AirTuner restart (self-reset line).\n");
	gpio_direction_output(AIRTUNER_GPIO_SELF_RST, 0);
	mdelay(1000);

	/* still running: the line is not wired on this board after all */
	printk(KERN_ERR "airtuner: the self-reset line did nothing\n");
	if (airtuner_soc_restart)
		airtuner_soc_restart(command);
}

static const struct airtuner_board * __init airtuner_board_named(void)
{
	static const char key[] = "PRODUCT_FWIDS=";
	const char *p = strstr(saved_command_line, key);
	size_t n;
	int i;

	if (!p)
		return NULL;
	p += sizeof(key) - 1;
	n = strcspn(p, " \t\n");
	for (i = 0; i < ARRAY_SIZE(airtuner_boards); i++)
		if (strlen(airtuner_boards[i].name) == n &&
		    !strncmp(p, airtuner_boards[i].name, n))
			return &airtuner_boards[i];
	return NULL;
}

static int __init airtuner_init(void)
{
	const struct airtuner_board *board = airtuner_board_named();
	int err, i;

	if (!board)
		return 0;

	if (board->usb_power >= 0) {
		err = gpio_request(board->usb_power, "usb-power");
		if (!err)
			err = gpio_direction_output(board->usb_power, 1);
		if (err)
			printk(KERN_WARNING "airtuner: USB power not switched on "
			       "(%d)\n", err);
	}

	/*
	 * Tell gpiolib the pin is an output that is high - which it already
	 * is.  It must never be driven low here.
	 */
	err = gpio_request(AIRTUNER_GPIO_SELF_RST, "self-reset");
	if (!err)
		err = gpio_direction_output(AIRTUNER_GPIO_SELF_RST, 1);
	if (err) {
		printk(KERN_WARNING "airtuner: no self-reset line (%d), "
		       "restart stays as it was\n", err);
	} else {
		airtuner_soc_restart = _machine_restart;
		_machine_restart = airtuner_restart;
	}

	for (i = 0; i < ARRAY_SIZE(airtuner_leds); i++)
		airtuner_leds[i].gpio = board->led[i];
	err = platform_device_register(&airtuner_leds_device);
	if (err)
		printk(KERN_WARNING "airtuner: LEDs not registered (%d)\n", err);

	return 0;
}
late_initcall(airtuner_init);
