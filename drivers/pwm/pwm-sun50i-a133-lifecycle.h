/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BRICK_PWM_LIFECYCLE_H
#define BRICK_PWM_LIFECYCLE_H
/* Shared startup sequence, exercised with mocked clock/reset/MMIO callbacks.
 * No reset assertion callback exists: failure never pulses a shared controller.
 */
struct brick_pwm_start_ops {
	int (*reset_status)(void *ctx);
	int (*bus_is_on)(void *ctx);
	int (*bus_enable)(void *ctx);
	void (*bus_disable)(void *ctx);
	int (*reset_deassert)(void *ctx);
	int (*check_idle)(void *ctx);
};
static inline int brick_pwm_start(void *ctx, const struct brick_pwm_start_ops *ops)
{
	int reset, was_on, owned = 0, ret;

	reset = ops->reset_status(ctx);
	if (reset < 0) return reset;
	was_on = ops->bus_is_on(ctx);
	if (was_on < 0) return was_on;
	if (!was_on) {
		ret = ops->bus_enable(ctx);
		if (ret) return ret;
		owned = 1;
	}
	/* The bus is physically on here, including firmware-established state. */
	if (reset) {
		ret = ops->reset_deassert(ctx);
		if (ret) goto fail;
	}
	ret = ops->check_idle(ctx);
	if (ret) goto fail;
	/* On a running controller, inspect inherited outputs before acquiring a
	 * clock reference: error unwind must not gate firmware-owned outputs.
	 */
	if (!owned) {
		ret = ops->bus_enable(ctx);
		if (ret) return ret;
	}
	return 0; /* exactly one clock reference transfers to caller */
fail:
	if (owned) ops->bus_disable(ctx);
	return ret;
}
#endif
