/*
 * SDIO driver for XRadio drivers
 *
 * Copyright (c) 2013, XRadio
 * Author: XRadio
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/firmware.h>
#include <linux/unaligned.h>
#include <linux/interrupt.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/card.h>
#include <linux/mmc/sdio.h>
#include <linux/of.h>
#include <linux/of_irq.h>

#include "xradio.h"
#include "sdio.h"
#include "main.h"
#include "recovery.h"
#include "sdio-reset.h"

/* sdio vendor id and device id*/
static const struct sdio_device_id xradio_sdio_ids[] = {
 { SDIO_DEVICE(0x0a9e, 0x2282) }, { }
};

/* sbus_ops implemetation */
int sdio_data_read(struct xradio_common* self, unsigned int addr,
                          void *dst, int count)
{
	int ret;

	switch (count) {
	case 4:
		*((u32 *)dst) = sdio_readl(self->sdio_func, addr, &ret);
		break;
	default:
		ret = sdio_memcpy_fromio(self->sdio_func, dst, addr, count);
		break;
	}

	return ret;
}

int sdio_data_write(struct xradio_common* self, unsigned int addr,
                           const void *src, int count)
{
	int ret;

	switch (count) {
	case 4:
		sdio_writel(self->sdio_func, *((u32 *)src), addr, &ret);
		break;
	default:
		ret = sdio_memcpy_toio(self->sdio_func, addr, (void *)src, count);
		break;
	}

	return ret;
}

void sdio_lock(struct xradio_common* self)
{
	sdio_claim_host(self->sdio_func);
}

void sdio_unlock(struct xradio_common *self)
{
	sdio_release_host(self->sdio_func);
}

size_t sdio_align_len(struct xradio_common *self, size_t size)
{
	return sdio_align_size(self->sdio_func, size);
}

int sdio_set_blk_size(struct xradio_common *self, size_t size)
{
	return sdio_set_block_size(self->sdio_func, size);
}

extern void xradio_irq_handler(struct xradio_common*);

static void xr829_sdio_irq(struct sdio_func *func)
{
 struct xradio_common *self = sdio_get_drvdata(func);
 if (self) xradio_irq_handler(self);
}

int xrs_sdio_irq_claim(struct xradio_common *hw)
{
 int ret;
 sdio_claim_host(hw->sdio_func);
 ret=sdio_claim_irq(hw->sdio_func,xr829_sdio_irq);
 if(!ret)hw->irq_claimed=true;
 sdio_release_host(hw->sdio_func);
 return ret;
}
void xrs_sdio_irq_release(struct xradio_common *hw)
{
 sdio_claim_host(hw->sdio_func);
 if(hw->irq_claimed) {
  sdio_release_irq(hw->sdio_func);
  /* MMC removes the callback before its best-effort CCCR write. */
  hw->irq_claimed=hw->sdio_func->irq_handler!=NULL;
 }
 sdio_release_host(hw->sdio_func);
}


int sdio_pm(struct xradio_common *self, bool  suspend)
{
	int ret = 0;
	if (suspend) {
		/* Notify SDIO that XRADIO will remain powered during suspend */
		ret = sdio_set_host_pm_flags(self->sdio_func, MMC_PM_KEEP_POWER);
		if (ret)
			dev_dbg(&self->sdio_func->dev, "Error setting SDIO pm flags: %i\n", ret);
	}

	return ret;
}

/* Scope this research test to the independently audited stock triplet.
 * These are exact fixture sizes, not a claim about silicon RAM limits.
 */
static int xr829_preflight(struct device *dev)
{
 static const struct { const char *name; size_t size; } files[] = {
  { "xr829/boot_xr829.bin", 2184 },
  { "xr829/fw_xr829.bin", 175384 },
  { "xr829/sdd_xr829.bin", 744 },
 };
 const struct firmware *fw;
 int i, ret;
 for (i = 0; i < ARRAY_SIZE(files); i++) {
  ret = request_firmware(&fw, files[i].name, dev);
  if (ret) return ret;
  ret = fw->size == files[i].size ? 0 : -EINVAL;
  if (!ret && i == 2) {
   size_t off = 0;
   bool clock = false;
   while (off < fw->size) {
    const u8 *e = fw->data + off;
    if (fw->size - off < 2 || e[1] > fw->size - off - 2) {
     ret = -EINVAL; break;
    }
    if (e[0] == 0xc5) {
     if (e[1] != 2 || get_unaligned_le16(e + 2) != 24000) {
      ret = -EINVAL; break;
     }
     clock = true;
    } else if (e[0] == 0xeb && e[1] < 4) {
     ret = -EINVAL; break;
    }
    off += 2 + e[1];
   }
   if (!clock) ret = -EINVAL;
  }
  release_firmware(fw);
  if (ret) return dev_err_probe(dev, ret, "unexpected XR829 stock file %s\n",
                                 files[i].name);
 }
 return 0;
}

static int sdio_probe(struct sdio_func *func, const struct sdio_device_id *id)
{
 bool enabled = false;
 int ret;

 if (func->num != 1) return -ENODEV;
 ret = xr829_preflight(&func->dev);
 if (ret) return ret;
 func->card->quirks |= MMC_QUIRK_BROKEN_BYTE_MODE_512;
 /* An unbound SDIO function can still contain running WLAN firmware.
  * Establish a fresh hardware state before starting IRQs or downloading it.
  */
 ret = xrs_sdio_power_reset(func, &enabled);
 if (ret)
  return dev_err_probe(&func->dev, ret, "WLAN reset before firmware load\n");
 ret = xradio_core_init(func);
 if (!ret) return 0;
 sdio_claim_host(func);
 sdio_disable_func(func);
 sdio_release_host(func);
 return ret;
}

static void sdio_remove(struct sdio_func *func)
{
 xradio_core_deinit(func);
}

/* No WoWLAN contract is advertised. cfg80211 disconnects before its child
 * suspends; MMC may then power off WLAN through the board power sequence.
 */
static int sdio_prepare(struct device *dev)
{
	struct xradio_common *hw = sdio_get_drvdata(dev_to_sdio_func(dev));

	return xrs_pm_prepare(hw);
}

static int sdio_suspend(struct device *dev)
{
	struct xradio_common *hw = sdio_get_drvdata(dev_to_sdio_func(dev));
	int ret;

	wiphy_lock(hw->hw->wiphy);
	ret = xrs_pm_stop_locked(hw);
	wiphy_unlock(hw->hw->wiphy);
	return ret;
}

static int sdio_resume(struct device *dev)
{
	struct xradio_common *hw = sdio_get_drvdata(dev_to_sdio_func(dev));
	int ret;

	wiphy_lock(hw->hw->wiphy);
	ret = xrs_pm_restore_locked(hw);
	wiphy_unlock(hw->hw->wiphy);
	return ret;
}

static void sdio_complete(struct device *dev)
{
	xrs_pm_complete(sdio_get_drvdata(dev_to_sdio_func(dev)));
}

static const struct dev_pm_ops sdio_pm_ops = {
	.prepare = sdio_prepare,
	.suspend = sdio_suspend,
	.resume = sdio_resume,
	.complete = sdio_complete,
};

static struct sdio_driver sdio_driver = {
	.name     = "xr829",
	.id_table = xradio_sdio_ids,
	.probe    = sdio_probe,
	.remove   = sdio_remove,
	.drv = {
			.owner = THIS_MODULE,
			.pm = &sdio_pm_ops,
	}
};


int xradio_sdio_register(){
	return sdio_register_driver(&sdio_driver);
}

void xradio_sdio_unregister(){
	sdio_unregister_driver(&sdio_driver);
}

MODULE_DEVICE_TABLE(sdio, xradio_sdio_ids);
