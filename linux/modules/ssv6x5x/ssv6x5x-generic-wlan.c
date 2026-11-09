// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Module entry for the board (development record #293).
 *
 * The vendor's per-platform glue (rockchip_wifi_power / rockchip_wifi_set_carddetect from
 * net/rfkill/rfkill-wlan.c) switched the chip's power GPIO and rescanned the SDIO host. Mainline does that
 * before any driver loads: mmc-pwrseq-simple in the device tree (linux/dts/rk3128-wifi.dtsi) powers the chip when the
 * host starts and the core enumerates the card as a non-removable SDIO device. All that is left to do here is
 * register the driver halves, which then bind to the card that is already there.
 */

#include <linux/module.h>
#include <linux/version.h>

extern int tu_ssvdevice_init(void);
extern void tu_ssvdevice_exit(void);

static int __init ssv_generic_wifi_init_module(void)
{
	return tu_ssvdevice_init();
}

static void __exit ssv_generic_wifi_exit_module(void)
{
	tu_ssvdevice_exit();
}

module_init(ssv_generic_wifi_init_module);
module_exit(ssv_generic_wifi_exit_module);

MODULE_DESCRIPTION("iComm-semi SSV6006C (SSV6x5x) SDIO Wi-Fi, station mode, for the RK3128 reference");
MODULE_LICENSE("Dual BSD/GPL");
