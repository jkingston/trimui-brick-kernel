/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef A133_CE_RECOVERY_H
#define A133_CE_RECOVERY_H
/*
 * Caller holds submission/retirement mutex.
 * IRQ must not acquire that mutex. All submitted DMA remains owned until take.
 * Linux integration supplies errno, bool and the reset/IRQ operations below.
 */
#define CE_RECOVERY_FLOWS 4
struct ce_recovery {
	unsigned int active, retired, quarantine;
	int result[CE_RECOVERY_FLOWS];
	int reset_error;
	bool dead;
};
struct ce_recovery_ops {
	void (*irq_drain)(void *ctx); /* disable_irq, no spinlock held */
	void (*mask)(void *ctx); /* ICR=0, readback before reset */
	int (*assert_reset)(void *ctx);
	int (*reset_status)(void *ctx);
	void (*hold_reset)(void *ctx); /* CCU reset pulse: udelay(10) */
	int (*deassert_reset)(void *ctx);
	void (*clear_pending)(void *ctx); /* A133 ISR=0xff, readback */
	void (*irq_resume)(void *ctx);
};
static int ce_recovery_submit(struct ce_recovery *s, unsigned int flow)
{
	unsigned int bit;

	if (flow >= CE_RECOVERY_FLOWS)
		return -EINVAL;
	if (s->dead)
		return -EIO;
	bit = 1U << flow;
	if ((s->active | s->retired | s->quarantine) & bit)
		return -EBUSY;
	s->active |= bit;
	return 0;
}
/* Call only after DMA-terminal success evidence for this exact flow. */
static int ce_recovery_complete(struct ce_recovery *s, unsigned int flow)
{
	unsigned int bit;

	if (flow >= CE_RECOVERY_FLOWS)
		return -EINVAL;
	bit = 1U << flow;
	if (!(s->active & bit))
		return -ENOENT;
	s->active &= ~bit;
	s->retired |= bit;
	s->result[flow] = 0;
	return 0;
}
static int ce_recovery_fault(struct ce_recovery *s, unsigned int flow, int cause,
			     const struct ce_recovery_ops *ops, void *ctx)
{
	unsigned int affected, i;
	int ret;

	if (flow >= CE_RECOVERY_FLOWS || cause >= 0)
		return -EINVAL;
	/* A second failing waiter consumes its recorded result, not another reset. */
	if (!(s->active & (1U << flow)))
		return -ENOENT;
	affected = s->active;
	ops->irq_drain(ctx);
	ops->mask(ctx);
	ret = ops->assert_reset(ctx);
	if (ret)
		goto uncontained;
	ret = ops->reset_status(ctx);
	if (ret != 1) {
		ret = ret < 0 ? ret : -EIO;
		goto uncontained;
	}
	/*
	 * Asserted CE reset terminates CE DMA according to vendor recovery.
	 * This matches vendor recovery; device sentinel qualification is pending.
	 */
	ops->hold_reset(ctx);
	s->active = 0;
	s->retired |= affected;
	for (i = 0; i < CE_RECOVERY_FLOWS; i++)
		if (affected & (1U << i))
			s->result[i] = i == flow ? cause : -EIO;
	ret = ops->deassert_reset(ctx);
	if (ret)
		goto stopped;
	ret = ops->reset_status(ctx);
	if (ret) {
		ret = ret < 0 ? ret : -EIO;
		goto stopped;
	}
	ops->clear_pending(ctx);
	ops->irq_resume(ctx);
	return 0;
uncontained:
	/* No release permit: caller-owned mappings cannot be finalized either. */
	s->active = 0;
	s->quarantine |= affected;
	for (i = 0; i < CE_RECOVERY_FLOWS; i++)
		if (affected & (1U << i))
			s->result[i] = i == flow ? cause : -EIO;
stopped:
	s->dead = true;
	if (!s->reset_error)
		s->reset_error = ret;
	return ret;
}
/* This permit precedes DMA unmap/free; error never grants a release permit. */
static int ce_recovery_take(struct ce_recovery *s, unsigned int flow, int *result)
{
	unsigned int bit;

	if (flow >= CE_RECOVERY_FLOWS)
		return -EINVAL;
	bit = 1U << flow;
	if (!(s->retired & bit))
		return -EBUSY;
	*result = s->result[flow];
	s->retired &= ~bit;
	return 0;
}
#endif
