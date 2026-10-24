// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>

#include "xradio.h"
#include "debug.h"
#include "sdio.h"

MODULE_AUTHOR("XRadioTech");
MODULE_DESCRIPTION("XR829 SDIO 2.4 GHz wireless station driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("xr829/boot_xr829.bin");
MODULE_FIRMWARE("xr829/fw_xr829.bin");
MODULE_FIRMWARE("xr829/sdd_xr829.bin");

/* Init Module function -> Called by insmod */
static int __init xradio_core_entry(void)
{
	int ret = 0;
	ret = xradio_sdio_register();
	return ret;
}

/* Called at Driver Unloading */
static void __exit xradio_core_exit(void)
{
	xradio_sdio_unregister();
}

module_init(xradio_core_entry);
module_exit(xradio_core_exit);
