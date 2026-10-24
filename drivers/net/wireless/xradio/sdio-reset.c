// SPDX-License-Identifier: GPL-2.0-only
/* Used before initial firmware loading and after transport quiescence.
 * No callback, work item, BH thread or SDIO IRQ may access the device.
 */
#include <linux/errno.h>
#include <linux/mmc/card.h>
#include <linux/mmc/core.h>
#include <linux/mmc/sdio_func.h>

#include "sdio-reset.h"

/* Reset transport through MMC's board power sequence; no GPIO/regulator bypass.
 * On success function1 is enabled with512-byte blocks, but no IRQ is installed.
 * On ANY error the caller must stay stopped; no counter/epoch reuse is allowed.
 * -EINPROGRESS means MMC scheduled removal; caller must not reinitialize func.
 * This function cannot prove caller quiescence or board reset wiring by itself.
 */
int xrs_sdio_power_reset(struct sdio_func *func, bool *enabled)
{
	int ret;

	if (!func || !enabled || func->num != 1)
		return -EINVAL;

	sdio_claim_host(func);
	if (func->irq_handler ||
	    atomic_read(&func->card->sdio_funcs_probed) != 1) {
		ret = -EBUSY;
		goto out;
	}

	ret = sdio_disable_func(func);
	if (ret)
		goto out;

	*enabled = false;
	ret = mmc_hw_reset(func->card);
	if (ret > 0) {
		ret = -EINPROGRESS;
		goto out;
	}
	if (ret)
		goto out;

	ret = sdio_enable_func(func);
	/* IOEx may already be set even when the ready wait failed. */
	*enabled = true;
	if (ret)
		goto disable;
	ret = sdio_set_block_size(func, 512);
	if (!ret)
		goto out;
disable:
	/* Enable may set IOEx before its ready wait fails. Clear it on either
	 * setup failure, preserving the original error if cleanup also fails.
	 */
	if (!sdio_disable_func(func))
		*enabled = false;
out:
	sdio_release_host(func);
	return ret;
}
