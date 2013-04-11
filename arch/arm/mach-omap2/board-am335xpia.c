/*
 * Code for piA-AM335X board.
 *
 * Copyright (C) 2013 pironex GmbH. - http://www.pironex.de/
 * (based on board-am335xevm.c)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/if_ether.h>
#include <linux/i2c/at24.h>
#include <linux/mfd/tps65910.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/partitions.h>
#include <linux/mfd/ti_tscadc.h>
#include <linux/pwm/pwm.h>
#include <linux/reboot.h>
#include <linux/platform_data/leds-pca9633.h>

#include <mach/hardware.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/hardware/asp.h>

#include <plat/omap_device.h>
#include <plat/irqs.h>
#include <plat/board.h>
#include <plat/common.h>
#include <plat/usb.h>
#include <plat/mmc.h>
#include <plat/nand.h>

#include "board-flash.h"
#include "common.h"
#include "cpuidle33xx.h"
#include "devices.h"
#include "mux.h"
#include "hsmmc.h"

/** BOARD CONFIG storage */
static struct omap_board_config_kernel pia335x_config[] __initdata = {
};

/* fallback mac addresses */
static char am335x_mac_addr[2][ETH_ALEN];

/* Convert GPIO signal to GPIO pin number */
#define GPIO_TO_PIN(bank, gpio) (32 * (bank) + (gpio))

/*
* EVM Config held in On-Board eeprom device.
*
* Header Format
*
*  Name			Size	Contents
*			(Bytes)
*-------------------------------------------------------------
*  Header		4	0xAA, 0x55, 0x33, 0xEE
*
*  Board Name		8	Name for board in ASCII.
*				example "A33515BB" = "AM335X
				Low Cost EVM board"
*
*  Version		4	Hardware version code for board in
*				in ASCII. "1.0A" = rev.01.0A
*
*  Serial Number	12	Serial number of the board. This is a 12
*				character string which is WWYY4P16nnnn, where
*				WW = 2 digit week of the year of production
*				YY = 2 digit year of production
*				nnnn = incrementing board number
*
*  Configuration option	32	Codes(TBD) to show the configuration
*				setup on this board.
*
*  Available		32720	Available space for other non-volatile
*				data.
*/
struct pia335x_eeprom_config {
	u32	header;
	u8	name[8];
	char	version[4];
	u8	serial[12];
	u8	opt[32];
};
static struct pia335x_eeprom_config config;


/** PINMUX **/
struct pinmux_config {
	const char *string_name; /* signal name format */
	int val; /* Options for the mux register value */
};

/*
* @pin_mux - single module pin-mux structure which defines pin-mux
*			details for all its pins.
*/
static void setup_pin_mux(struct pinmux_config *pin_mux)
{
	int i;

	for (i = 0; pin_mux->string_name != NULL; pin_mux++)
		omap_mux_init_signal(pin_mux->string_name, pin_mux->val);

}

#ifdef CONFIG_OMAP_MUX
static struct omap_board_mux board_mux[] __initdata = {
	/* I2C0 */
	AM33XX_MUX(I2C0_SDA, OMAP_MUX_MODE0 | AM33XX_SLEWCTRL_SLOW |
			AM33XX_INPUT_EN | AM33XX_PIN_OUTPUT),
	AM33XX_MUX(I2C0_SCL, OMAP_MUX_MODE0 | AM33XX_SLEWCTRL_SLOW |
			AM33XX_INPUT_EN | AM33XX_PIN_OUTPUT),
	/* I2C1*/
	AM33XX_MUX(UART0_CTSN, OMAP_MUX_MODE3 | AM33XX_SLEWCTRL_SLOW |
			AM33XX_INPUT_EN | AM33XX_PIN_OUTPUT),
	AM33XX_MUX(UART0_RTSN, OMAP_MUX_MODE3 | AM33XX_SLEWCTRL_SLOW |
			AM33XX_INPUT_EN | AM33XX_PIN_OUTPUT),
	/* RS485 / UART3 */
	AM33XX_MUX(MII1_RXD2, OMAP_MUX_MODE1 | AM33XX_PULL_ENBL),
	AM33XX_MUX(MII1_RXD3, OMAP_MUX_MODE1 | AM33XX_INPUT_EN),
	{ .reg_offset = OMAP_MUX_TERMINATOR },
};
#else
#define	board_mux	NULL
#endif

/* Module pin mux for mmc0 */
static struct pinmux_config mmc0_pin_mux[] = {
	{"mmc0_dat3.mmc0_dat3",	OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	{"mmc0_dat2.mmc0_dat2",	OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	{"mmc0_dat1.mmc0_dat1",	OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	{"mmc0_dat0.mmc0_dat0",	OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	{"mmc0_clk.mmc0_clk",	OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	{"mmc0_cmd.mmc0_cmd",	OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	/* write protect */
	{"mii1_txclk.gpio3_9", AM33XX_PIN_INPUT_PULLUP},
	/* card detect */
	{"mii1_txd2.gpio0_17",  OMAP_MUX_MODE7 | AM33XX_PIN_INPUT_PULLUP},
	{NULL, 0},
};

/* Module pin mux for mii2 */
static struct pinmux_config mii2_pin_mux[] = {
	/*
	{"gpmc_wpn.mii2_rxerr", OMAP_MUX_MODE1 | AM33XX_PIN_INPUT_PULLDOWN},
	*/
	{"gpmc_a0.mii2_txen", OMAP_MUX_MODE1 | AM33XX_PIN_OUTPUT},
	{"gpmc_a1.mii2_rxdv", OMAP_MUX_MODE1 | AM33XX_PIN_INPUT_PULLDOWN},
	{"gpmc_a2.mii2_txd3", OMAP_MUX_MODE1 | AM33XX_PIN_OUTPUT},
	{"gpmc_a3.mii2_txd2", OMAP_MUX_MODE1 | AM33XX_PIN_OUTPUT},
	{"gpmc_a4.mii2_txd1", OMAP_MUX_MODE1 | AM33XX_PIN_OUTPUT},
	{"gpmc_a5.mii2_txd0", OMAP_MUX_MODE1 | AM33XX_PIN_OUTPUT},
	{"gpmc_a6.mii2_txclk", OMAP_MUX_MODE1 | AM33XX_PIN_INPUT_PULLDOWN},
	{"gpmc_a7.mii2_rxclk", OMAP_MUX_MODE1 | AM33XX_PIN_INPUT_PULLDOWN},
	{"gpmc_a8.mii2_rxd3", OMAP_MUX_MODE1 | AM33XX_PIN_INPUT_PULLDOWN},
	{"gpmc_a9.mii2_rxd2", OMAP_MUX_MODE1 | AM33XX_PIN_INPUT_PULLDOWN},
	{"gpmc_a10.mii2_rxd1", OMAP_MUX_MODE1 | AM33XX_PIN_INPUT_PULLDOWN},
	{"gpmc_a11.mii2_rxd0", OMAP_MUX_MODE1 | AM33XX_PIN_INPUT_PULLDOWN},
	{"mdio_data.mdio_data", OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	{"mdio_clk.mdio_clk", OMAP_MUX_MODE0 | AM33XX_PIN_OUTPUT_PULLUP},
	{NULL, 0},
};

/* Module pin mux for nand */
static struct pinmux_config nand_pin_mux[] = {
	{"gpmc_ad0.gpmc_ad0",	  OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	{"gpmc_ad1.gpmc_ad1",	  OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	{"gpmc_ad2.gpmc_ad2",	  OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	{"gpmc_ad3.gpmc_ad3",	  OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	{"gpmc_ad4.gpmc_ad4",	  OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	{"gpmc_ad5.gpmc_ad5",	  OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	{"gpmc_ad6.gpmc_ad6",	  OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	{"gpmc_ad7.gpmc_ad7",	  OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	{"gpmc_wait0.gpmc_wait0", OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	{"gpmc_wpn.gpmc_wpn",	  OMAP_MUX_MODE7 | AM33XX_PIN_INPUT_PULLUP},
	{"gpmc_csn0.gpmc_csn0",	  OMAP_MUX_MODE0 | AM33XX_PULL_DISA},
	{"gpmc_advn_ale.gpmc_advn_ale",  OMAP_MUX_MODE0 | AM33XX_PULL_DISA},
	{"gpmc_oen_ren.gpmc_oen_ren",	 OMAP_MUX_MODE0 | AM33XX_PULL_DISA},
	{"gpmc_wen.gpmc_wen",     OMAP_MUX_MODE0 | AM33XX_PULL_DISA},
	{"gpmc_ben0_cle.gpmc_ben0_cle",	 OMAP_MUX_MODE0 | AM33XX_PULL_DISA},
	{NULL, 0},
};
/* pinmux for usb0 */
static struct pinmux_config usb0_pin_mux[] = {
	/*{"usb0_drvvbus.usb0_drvvbus",    OMAP_MUX_MODE0 | AM33XX_PIN_OUTPUT},*/
	{NULL, 0},
};

/* pinmux for usb1 */
static struct pinmux_config usb1_pin_mux[] = {
	/* other usb pins are not muxable */
	{"usb1_drvvbus.usb1_drvvbus", OMAP_MUX_MODE0 | AM33XX_PIN_OUTPUT},
	{"gmii1_rxd1.gpio2_20",     OMAP_MUX_MODE7 | AM33XX_PIN_INPUT_PULLUP },
	{NULL, 0},
};

/* pinmux for led drivers */
static struct pinmux_config km_e2_leds_pin_mux[] = {
	/* enable input to allow readback of status */
	{"mcasp0_ahclkr.gpio3_17", OMAP_MUX_MODE0 | AM33XX_PIN_INPUT_PULLUP},
	{NULL, 0},
};

static struct pinmux_config km_e2_rs485_pin_mux[] = {
	/* signal not implemented in mux33xx.c
	{"mii1_rxd2.uart3_txd", OMAP_MUX_MODE1 | AM33XX_PIN_INPUT_PULLUP},
	{"mii1_rxd3.uart3_rxd", OMAP_MUX_MODE1 | AM33XX_PULL_ENBL},*/
	{"lcd_data11.gpio2_17", OMAP_MUX_MODE7 | AM33XX_PIN_INPUT_PULLUP},
	{NULL, 0},
};

/* NAND partition information */
static struct mtd_partition pia335x_nand_partitions[] = {
/* All the partition sizes are listed in terms of NAND block size */
	{
		.name           = "SPL",
		.offset         = 0,			/* Offset = 0x0 */
		.size           = SZ_128K,
	},
	{
		.name           = "SPL.backup1",
		.offset         = MTDPART_OFS_APPEND,	/* Offset = 0x20000 */
		.size           = SZ_128K,
	},
	{
		.name           = "SPL.backup2",
		.offset         = MTDPART_OFS_APPEND,	/* Offset = 0x40000 */
		.size           = SZ_128K,
	},
	{
		.name           = "SPL.backup3",
		.offset         = MTDPART_OFS_APPEND,	/* Offset = 0x60000 */
		.size           = SZ_128K,
	},
	{
		.name           = "U-Boot",
		.offset         = MTDPART_OFS_APPEND,   /* Offset = 0x80000 */
		.size           = 15 * SZ_128K,
	},
	{
		.name           = "U-Boot Env",
		.offset         = MTDPART_OFS_APPEND,   /* Offset = 0x260000 */
		.size           = 1 * SZ_128K,
	},
	{
		.name           = "Kernel",
		.offset         = MTDPART_OFS_APPEND,   /* Offset = 0x280000 */
		.size           = 40 * SZ_128K,
	},
	{
		.name           = "File System",
		.offset         = MTDPART_OFS_APPEND,   /* Offset = 0x780000 */
		.size           = MTDPART_SIZ_FULL,
	},
};

/* taken from ti evm */
static struct gpmc_timings pia335x_nand_timings = {
	.sync_clk = 0,

	.cs_on = 0,
	.cs_rd_off = 44,
	.cs_wr_off = 44,

	.adv_on = 6,
	.adv_rd_off = 34,
	.adv_wr_off = 44,
	.we_off = 40,
	.oe_off = 54,

	.access = 64,
	.rd_cycle = 82,
	.wr_cycle = 82,

	.wr_access = 40,
	.wr_data_mux_bus = 0,
};

static void nand_init(void)
{
	struct omap_nand_platform_data *pdata;
	struct gpmc_devices_info gpmc_device[2] = {
		{ NULL, 0 },
		/*{ NULL, 0 },*/
	};

	setup_pin_mux(nand_pin_mux);
	pdata = omap_nand_init(pia335x_nand_partitions,
		ARRAY_SIZE(pia335x_nand_partitions), 0, 0,
		&pia335x_nand_timings);
	if (!pdata)
		return;
	pdata->ecc_opt =OMAP_ECC_BCH8_CODE_HW;
	pdata->elm_used = true; /* Error Locator Module */
	gpmc_device[0].pdata = pdata;
	gpmc_device[0].flag = GPMC_DEVICE_NAND;

	omap_init_gpmc(gpmc_device, sizeof(gpmc_device));
	omap_init_elm();
}

/* USB0 device */
static void usb0_init(void)
{
	setup_pin_mux(usb0_pin_mux);
}

/* USB1 host */
static void usb1_init(void)
{
	setup_pin_mux(usb1_pin_mux);
}

/* MII2 */
static void mii2_init(void)
{
	pr_info("piA335x: %s\n", __func__);
	setup_pin_mux(mii2_pin_mux);
}

static struct omap2_hsmmc_info pia335x_mmc[] __initdata = {
	{
		.mmc            = 1,
		.caps           = MMC_CAP_4_BIT_DATA,
		.gpio_cd        = GPIO_TO_PIN(0, 17),
		.gpio_wp        = GPIO_TO_PIN(3, 9),
		.ocr_mask       = MMC_VDD_32_33 | MMC_VDD_33_34, /* 3V3 */
	},
	{
		.mmc            = 0,	/* will be set at runtime */
	},
	{
		.mmc            = 0,	/* will be set at runtime */
	},
	{}      /* Terminator */
};

static void mmc0_init(void)
{
	pr_info("piA335x: %s\n", __func__);
	setup_pin_mux(mmc0_pin_mux);

	omap2_hsmmc_init(pia335x_mmc);
	return;
}

/** I2C1 */
static struct led_info km_e2_leds1_config[] = {
	{
		.name = "led9",
		.default_trigger = "none",
	},
	{
		.name = "pbled1",
		.default_trigger = "none",
	},
	{
		.name = "pbled3",
		.default_trigger = "none",
	},
	{
		.name = "null",
		.default_trigger = "none",
	},
	{
		.name = "pbled2",
		.default_trigger = "default-on",
	},
};
static struct pca9633_platform_data km_e2_leds1_data = {
	.leds = {
		.num_leds = 5,
		.leds = km_e2_leds1_config,
	},
	.outdrv = PCA9633_OPEN_DRAIN,
};

static struct led_info km_e2_leds2_config[] = {
	{
		.name = "led1",
		.default_trigger = "heartbeat",
	},
	{
		.name = "led2",
		.default_trigger = "none",
	},
	{
		.name = "led3",
		.default_trigger = "none",
	},
	{
		.name = "led4",
		.default_trigger = "none",
	},
	{
		.name = "led5",
		.default_trigger = "none",
	},
	{
		.name = "led6",
		.default_trigger = "none",
	},
	{
		.name = "led7",
		.default_trigger = "none",
	},
	{
		.name = "led8",
		.default_trigger = "none",
	},
};
static struct pca9633_platform_data km_e2_leds2_data = {
	.leds = {
		.num_leds = 8,
		.leds = km_e2_leds2_config,
	},
	.outdrv = PCA9633_OPEN_DRAIN,
};

static void km_e2_leds_init(void)
{
	int gpio = GPIO_TO_PIN(3, 17);
	setup_pin_mux(nand_pin_mux);
	if (gpio_request(gpio, "led_oe") < 0) {
		pr_err("Failed to request gpio for led_oe");
		return;
	}

	pr_info("Configure LEDs...\n");
	gpio_direction_output(gpio, 0);
	gpio_export(gpio, 0);
}

/* FRAM is similar to at24 eeproms without write delay and page limits */
static struct at24_platform_data e2_km_fram_info = {
	.byte_len       = (256*1024) / 8,
	.page_size      = (256*1024) / 8, /* no sequencial rw limit */
	.flags          = AT24_FLAG_ADDR16,
	.context        = (void *)NULL,
};

static struct i2c_board_info km_e2_i2c1_boardinfo[] = {
	{
		I2C_BOARD_INFO("pca9634", 0x22),
		.platform_data = &km_e2_leds1_data,
	},
	{
		I2C_BOARD_INFO("pca9634", 0x23),
		.platform_data = &km_e2_leds2_data,
	},
	{
		I2C_BOARD_INFO("tmp422", 0x4C),
	},
	{	I2C_BOARD_INFO("24c256", 0x52),
		.platform_data = &e2_km_fram_info,
	}
};

static void km_e2_i2c2_init(void)
{
	setup_pin_mux(km_e2_leds_pin_mux);
	km_e2_leds_init();
	omap_register_i2c_bus(2, 400, km_e2_i2c1_boardinfo,
			ARRAY_SIZE(km_e2_i2c1_boardinfo));
}
#define KM_E2_RS485_DE_GPIO	GPIO_TO_PIN(2, 17)
static void km_e2_rs485_init(void)
{
	setup_pin_mux(km_e2_rs485_pin_mux);
	/* use GPIO for RS485 Driver Enable signal
	 * Auto-RTS functionality (MUX MODE 6) cannot be used, because it
	 * doesn't de-assert RTS if a transmission is active, which would
	 * be required to provide a driver disable functionality.
	 *
	 * Instead it disables RTS only if RX FIFO is full, which means
	 * during normal operation RTS will always be active and so would the
	 * driver, while the receiver would be deactivated most of the time.
	 *
	 * For Half-Duplex RS485 we need the receiver to be enabled whenever
	 * no transmission is active, as Tranceiver and Receiver must never be
	 * active at the same time.
	 */
	if (gpio_request(KM_E2_RS485_DE_GPIO, "te_reg") < 0) {
		pr_err("Failed to request gpio for led_oe");
		return;
	}

	pr_info("Configure RS485 TE GPIO\n");
	/* enable receiver by default */
	gpio_direction_output(KM_E2_RS485_DE_GPIO, 0);
	gpio_export(KM_E2_RS485_DE_GPIO, 0);
}

/**
 * AM33xx internal RTC
 */
#include <linux/rtc/rtc-omap.h>
static struct omap_rtc_pdata pia335x_rtc_info = {
	.pm_off		= false,
	.wakeup_capable	= 0,
};

static int pia335x_rtc_init(void)
{
	void __iomem *base;
	struct clk *clk;
	struct omap_hwmod *oh;
	struct platform_device *pdev;
	char *dev_name = "am33xx-rtc";

	clk = clk_get(NULL, "rtc_fck");
	if (IS_ERR(clk)) {
		pr_err("rtc : Failed to get RTC clock\n");
		return -1;
	}

	if (clk_enable(clk)) {
		pr_err("rtc: Clock Enable Failed\n");
		return -1;
	}

	base = ioremap(AM33XX_RTC_BASE, SZ_4K);

	if (WARN_ON(!base))
		return -1;

	/* Unlock the rtc's registers */
	writel(0x83e70b13, base + 0x6c);
	writel(0x95a4f1e0, base + 0x70);

	/*
	 * Enable the 32K OSc
	 * TODO: Need a better way to handle this
	 * Since we want the clock to be running before mmc init
	 * we need to do it before the rtc probe happens
	 *
	 * pia: we don't really need the 32k external OSC
	 */
	writel(0x48, base + 0x54);

	iounmap(base);

	// TODO check pia335x_rtc_info.pm_off = true;

	clk_disable(clk);
	clk_put(clk);

	if (omap_rev() == AM335X_REV_ES2_0)
		pia335x_rtc_info.wakeup_capable = 1;

	oh = omap_hwmod_lookup("rtc");
	if (!oh) {
		pr_err("could not look up %s\n", "rtc");
		return -1;
	}

	pdev = omap_device_build(dev_name, -1, oh, &pia335x_rtc_info,
			sizeof(struct omap_rtc_pdata), NULL, 0, 0);
	WARN(IS_ERR(pdev), "Can't build omap_device for %s:%s.\n",
			dev_name, oh->name);

	return 0;
}

static void setup_e2(void)
{
	pr_info("piA335x: Setup KM E2.\n");
	/* EVM - Starter Kit */
/*	static struct evm_dev_cfg evm_sk_dev_cfg[] = {
		{mmc1_wl12xx_init,	DEV_ON_BASEBOARD, PROFILE_ALL},
		{enable_ecap2,     DEV_ON_BASEBOARD, PROFILE_ALL},
		{mfd_tscadc_init,	DEV_ON_BASEBOARD, PROFILE_ALL},
		{gpio_keys_init,  DEV_ON_BASEBOARD, PROFILE_ALL},
		{lis331dlh_init, DEV_ON_BASEBOARD, PROFILE_ALL},
		{mcasp1_init,   DEV_ON_BASEBOARD, PROFILE_ALL},
		{uart1_wl12xx_init, DEV_ON_BASEBOARD, PROFILE_ALL},
		{wl12xx_init,       DEV_ON_BASEBOARD, PROFILE_ALL},
		{gpio_ddr_vtt_enb_init,	DEV_ON_BASEBOARD, PROFILE_ALL},
		{NULL, 0, 0},
	};*/
	pia335x_rtc_init();
	km_e2_i2c2_init(); /* second i2c bus */
	mmc0_init();
	mii2_init();
	usb0_init();
	usb1_init();
	nand_init();
	km_e2_rs485_init();

	pr_info("piA335x: cpsw_init\n");
	am33xx_cpsw_init(AM33XX_CPSW_MODE_MII, "0:1e", "0:00");
}

void am33xx_cpsw_macidfillup(char *eeprommacid0, char *eeprommacid1);
static void pia335x_setup(struct memory_accessor *mem_acc, void *context)
{
	/* generic board detection triggered by eeprom init */
	int ret;
	char tmp[10];

	pr_info("piA335x: setup\n");
	/* from evm code
	 * 1st get the MAC address from EEPROM */
	ret = mem_acc->read(mem_acc, (char *)&am335x_mac_addr,
		0x60, sizeof(am335x_mac_addr));

	if (ret != sizeof(am335x_mac_addr)) {
		pr_warning("AM335X: EVM Config read fail: %d\n", ret);
		return;
	}

	/* Fillup global mac id */
	am33xx_cpsw_macidfillup(&am335x_mac_addr[0][0],
				&am335x_mac_addr[1][0]);

	/* get board specific data */
	ret = mem_acc->read(mem_acc, (char *)&config, 0, sizeof(config));
	if (ret != sizeof(config)) {
		pr_err("piA335x config read fail, read %d bytes\n", ret);
		goto out;
	}

	if (config.header != 0xEE3355AA) { // header magic number
		pr_err("piA335x: wrong header 0x%x, expected 0x%x\n",
			config.header, 0xEE3355AA);
		goto out;
	}

	if (strncmp("PIA335", config.name, 6)) {
		pr_err("Board %s\ndoesn't look like a PIA335 board\n",
			config.name);
		goto out;
	}

	snprintf(tmp, sizeof(config.name) + 1, "%s", config.name);
	pr_info("Board name: %s\n", tmp);
	snprintf(tmp, sizeof(config.version) + 1, "%s", config.version);
	pr_info("Board version: %s\n", tmp);

	if (!strncmp("PIA335E2", config.name, 8)) {
		//daughter_brd_detected = false;
		if(!strncmp("0.01", config.version, 4)) {
			setup_e2();
		} else {
			pr_info("piA335x: Unknown board revision %.4s\n",
					config.version);
		}
	}

	return;

out:
	/*
	 * If the EEPROM hasn't been programed or an incorrect header
	 * or board name are read then the hardware details are unknown.
	 * Notify the user and call machine_halt to stop the boot process.
	 */
	pr_err("PIA335x: Board identification failed... Halting...\n");
	machine_halt();
}

static struct omap_musb_board_data musb_board_data = {
	.interface_type	= MUSB_INTERFACE_ULPI,
	/*
	 * mode[0:3] = USB0PORT's mode
	 * mode[4:7] = USB1PORT's mode
	 */
	.mode           = (MUSB_HOST << 4) | MUSB_OTG,
	.power		= 500,
	.instances	= 1,
};

/**
 * I2C devices
 */
#define PIA335X_EEPROM_I2C_ADDR 0x50
static struct at24_platform_data pia335x_eeprom_info = {
	.byte_len       = 128,
	.page_size      = 8,
	.flags          = AT24_FLAG_TAKE8ADDR,
	.setup          = pia335x_setup,
	.context        = (void *)NULL,
};

static struct regulator_init_data pia335x_tps_dummy = {
	.constraints.always_on	= true,
};

static struct tps65910_board pia335x_tps65910_info = {
	.tps65910_pmic_init_data[TPS65910_REG_VRTC]	= &pia335x_tps_dummy,
	.tps65910_pmic_init_data[TPS65910_REG_VIO]	= &pia335x_tps_dummy,
	.tps65910_pmic_init_data[TPS65910_REG_VDD1]	= &pia335x_tps_dummy,
	.tps65910_pmic_init_data[TPS65910_REG_VDD2]	= &pia335x_tps_dummy,
	.tps65910_pmic_init_data[TPS65910_REG_VDD3]	= &pia335x_tps_dummy,
	.tps65910_pmic_init_data[TPS65910_REG_VDIG1]	= &pia335x_tps_dummy,
	.tps65910_pmic_init_data[TPS65910_REG_VDIG2]	= &pia335x_tps_dummy,
	.tps65910_pmic_init_data[TPS65910_REG_VPLL]	= &pia335x_tps_dummy,
	.tps65910_pmic_init_data[TPS65910_REG_VDAC]	= &pia335x_tps_dummy,
	.tps65910_pmic_init_data[TPS65910_REG_VAUX1]	= &pia335x_tps_dummy,
	.tps65910_pmic_init_data[TPS65910_REG_VAUX2]	= &pia335x_tps_dummy,
	.tps65910_pmic_init_data[TPS65910_REG_VAUX33]	= &pia335x_tps_dummy,
	.tps65910_pmic_init_data[TPS65910_REG_VMMC]	= &pia335x_tps_dummy,
};


static struct i2c_board_info __initdata pia335x_i2c0_boardinfo[] = {
	{
		/* Daughter Board EEPROM */
		I2C_BOARD_INFO("24c00", PIA335X_EEPROM_I2C_ADDR),
		.platform_data  = &pia335x_eeprom_info,
	},
	{
		I2C_BOARD_INFO("tps65910", TPS65910_I2C_ID1),
		.platform_data  = &pia335x_tps65910_info,
	},
};

static void __init pia335x_i2c_init(void)
{
	/* I2C1 must be muxed in u-boot */
	pr_info("piA335x: %s", __func__);
	omap_register_i2c_bus(1, 100, pia335x_i2c0_boardinfo,
				ARRAY_SIZE(pia335x_i2c0_boardinfo));
}

#ifdef CONFIG_MACH_AM335XEVM
/* FIXME for some reason board specific stuff is called from mach code
 * e.g.
 * pm33xx.c depends on definitions from board-am33xevm.c
 * or
 * multiple AM335x board definitions must conflict with each other
 * either way, this is a hack to prevent multiple definitions for now
 */
extern void __iomem * am33xx_emif_base;
extern void __iomem * __init am33xx_get_mem_ctlr(void);
#else
void __iomem *am33xx_emif_base;

void __iomem * __init am33xx_get_mem_ctlr(void)
{
	am33xx_emif_base = ioremap(AM33XX_EMIF0_BASE, SZ_32K);

	if (!am33xx_emif_base)
		pr_warning("%s: Unable to map DDR2 controller",	__func__);

	return am33xx_emif_base;
}

void __iomem *am33xx_get_ram_base(void)
{
	return am335xx_emif_base;
}
#endif


static struct resource am33xx_cpuidle_resources[] = {
	{
		.start		= AM33XX_EMIF0_BASE,
		.end		= AM33XX_EMIF0_BASE + SZ_32K - 1,
		.flags		= IORESOURCE_MEM,
	},
};

/* AM33XX devices support DDR2 power down */
static struct am33xx_cpuidle_config pia335x_cpuidle_pdata = {
	.ddr2_pdown	= 1,
};

static struct platform_device pia335x_cpuidle_device = {
	.name			= "cpuidle-am33xx",
	.num_resources		= ARRAY_SIZE(am33xx_cpuidle_resources),
	.resource		= am33xx_cpuidle_resources,
	.dev = {
		.platform_data	= &pia335x_cpuidle_pdata,
	},
};

static void __init pia335x_cpuidle_init(void)
{
	int ret;

	pr_info("piA335x: %s\n", __func__);

	pia335x_cpuidle_pdata.emif_base = am33xx_get_mem_ctlr();

	ret = platform_device_register(&pia335x_cpuidle_device);

	if (ret)
		pr_warning("AM33XX cpuidle registration failed\n");

}

#include <mach/board-am335xevm.h>
static void __init pia335x_init(void)
{
	pia335x_cpuidle_init();
	am33xx_mux_init(board_mux);
	omap_serial_init();
	pr_info("piA335x: i2c_init\n");
	pia335x_i2c_init();
	pr_info("piA335x: sdrc_init\n");
	omap_sdrc_init(NULL, NULL);
	pr_info("piA335x: musb_init\n");
	usb_musb_init(&musb_board_data);

	/* XXX what for? */
	omap_board_config = pia335x_config;
	omap_board_config_size = ARRAY_SIZE(pia335x_config);
}

static void __init pia335x_map_io(void)
{
	omap2_set_globals_am33xx();
	omapam33xx_map_common_io();
}


MACHINE_START(PIA_AM335X, "am335xpia")
	/* Maintainer: pironex */
	.atag_offset	= 0x100,
	.map_io		= pia335x_map_io,
	.init_early	= am33xx_init_early,
	.init_irq	= ti81xx_init_irq,
	.handle_irq     = omap3_intc_handle_irq,
	.timer		= &omap3_am33xx_timer,
	.init_machine	= pia335x_init,
MACHINE_END
