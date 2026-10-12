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
 * every board named below (docs/specs/emma3sep-airtuner-gpio.md):
 *
 *   GPIO 19  LED1_A  status LED, green  \
 *   GPIO 20  LED1_B  status LED, red     | low = lit, both = orange
 *   GPIO 21  LED2_A  power LED, green    |
 *   GPIO 22  LED2_B  power LED, red     /
 *   GPIO 30  SELF_RST_EMMA_L   low resets the whole board, the AV sub-CPU
 *                              included; the boot loader leaves it high
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

/* the names the boot loader passes in PRODUCT_FWIDS= */
static const char *airtuner_boards[] __initdata = {
	"PIX_AIRTUNER_LITE",
	"PIX_AIRTUNER_R",
	"PIX_AIRTUNER_W",
	"NEC_AIRTUNER_R",
	"TOSHIBA_AIRTUNER_R",
};

/*
 * Keep the names within 19 characters: this kernel cuts a device name there,
 * and a cut name is one userland does not find.
 */
static struct gpio_led airtuner_leds[] = {
	{ .name = "ird:green:status", .gpio = 19, .active_low = 1 },
	{ .name = "ird:red:status",   .gpio = 20, .active_low = 1 },
	{ .name = "ird:green:power",  .gpio = 21, .active_low = 1 },
	{
		.name = "ird:red:power", .gpio = 22, .active_low = 1,
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

static int __init airtuner_board_named(void)
{
	static const char key[] = "PRODUCT_FWIDS=";
	const char *p = strstr(saved_command_line, key);
	size_t n;
	int i;

	if (!p)
		return 0;
	p += sizeof(key) - 1;
	n = strcspn(p, " \t\n");
	for (i = 0; i < ARRAY_SIZE(airtuner_boards); i++)
		if (strlen(airtuner_boards[i]) == n &&
		    !strncmp(p, airtuner_boards[i], n))
			return 1;
	return 0;
}

static int __init airtuner_init(void)
{
	int err;

	if (!airtuner_board_named())
		return 0;

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

	err = platform_device_register(&airtuner_leds_device);
	if (err)
		printk(KERN_WARNING "airtuner: LEDs not registered (%d)\n", err);

	return 0;
}
late_initcall(airtuner_init);
