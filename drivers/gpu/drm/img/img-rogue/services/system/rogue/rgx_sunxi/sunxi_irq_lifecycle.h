/* SPDX-License-Identifier: GPL-2.0-only */
/* Exclusive GPU hard-IRQ ownership across internal power transitions.
 * Caller holds platform lifecycle mutex; quiesce/resume also DDK power mutex.
 * No IRQ handler may acquire either mutex. Operations are synchronous.
 */
enum irq_phase { IRQ_NEW, IRQ_ACTIVE, IRQ_QUIESCED, IRQ_RETIRED };
struct irq_lifecycle {
	enum irq_phase phase;
	unsigned int irq;
	unsigned long long stop_sequence;
	void *handle;
};
struct irq_lifecycle_ops {
	void (*disable_sync)(unsigned int irq); /* disable_irq, not nosync */
	void (*enable)(unsigned int irq);
	int (*uninstall)(void *handle); /* OSUninstallSystemLISR -> free_irq */
};
/* Called only after successful exclusive OSInstallSystemLISR. Return false if
 * caller would overwrite an owned handler; registration must check first.
 */
static bool irq_can_install(const struct irq_lifecycle *s)
{
	return s->phase == IRQ_NEW || s->phase == IRQ_RETIRED;
}
static int irq_installed(struct irq_lifecycle *s, unsigned int irq, void *handle)
{
	if (!irq_can_install(s) || !handle)
		return -EINVAL;
	s->irq = irq; s->handle = handle;
	s->stop_sequence = 0;
	s->phase = IRQ_ACTIVE;
	return 0;
}
/* Return1 means synchronous hard-IRQ drain is held,0 means skipped.
 * No caller may infer cut eligibility from return0. Token is consumed upstream.
 */
static int irq_quiesce(struct irq_lifecycle *s, const struct irq_lifecycle_ops *ops,
	bool stop_eligible, bool forced, unsigned long long sequence)
{
	if (!stop_eligible || forced)
		return 0;

	if (s->phase == IRQ_NEW || s->phase == IRQ_RETIRED)
		return 0;
	if (s->phase == IRQ_QUIESCED)
		return sequence == s->stop_sequence ? 1 : -EINVAL;
	if (s->phase != IRQ_ACTIVE || !s->handle)
		return -EINVAL;
	ops->disable_sync(s->irq);
	/* Publish held state only after all running hard IRQ handlers completed. */
	s->stop_sequence = sequence;
	s->phase = IRQ_QUIESCED;
	return 1;
}
/* Restoration proof must come from the physical owner, NOT !sticky_fault.
 * On failed restore keep the IRQ disabled; no implicit recovery attempt.
 */
static int irq_resume(struct irq_lifecycle *s, const struct irq_lifecycle_ops *ops,
	bool accessible_confirmed, bool sticky_fault)
{
	if (!accessible_confirmed || sticky_fault)
		return -EIO;

	if (s->phase == IRQ_NEW || s->phase == IRQ_RETIRED)
		return 0;
	if (s->phase == IRQ_ACTIVE)
		return 0;
	if (s->phase != IRQ_QUIESCED || !s->handle)
		return -EINVAL;
	ops->enable(s->irq);
	s->phase = IRQ_ACTIVE;
	s->stop_sequence = 0;
	return 0;
}
/* Do NOT enable a quiesced IRQ just to balance depth before removal.
 * free_irq removes and synchronizes the exclusive action while shut down.
 * A later successful request_irq starts a fresh descriptor activation/depth.
 */
static int irq_uninstall(struct irq_lifecycle *s, const struct irq_lifecycle_ops *ops)
{
	int ret;

	if (s->phase == IRQ_NEW || s->phase == IRQ_RETIRED)
		return 0;
	if (!s->handle)
		return -EINVAL;
	ret = ops->uninstall(s->handle);
	if (ret)
		return ret; /* retain exact ownership on failure */
	s->handle = NULL;
	s->phase = IRQ_RETIRED;
	s->stop_sequence = 0;
	return 0;
}
