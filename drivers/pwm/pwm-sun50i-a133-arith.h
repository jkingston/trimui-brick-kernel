/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BRICK_PWM_ARITH_H
#define BRICK_PWM_ARITH_H
/* Pure bounded arithmetic shared with host tests; nanoseconds, 24 MHz. */
static inline int brick_pwm_ticks(unsigned long long period,
		unsigned long long duty, unsigned int *ticks)
{
	if (period != 20000 || duty > period)
		return -1;
	*ticks = duty * 480 / 20000; /* round down: never exceed requested duty */
	return 0;
}
static inline int brick_pwm_decode(unsigned int pair, unsigned int control,
	unsigned int period_reg, unsigned long long *period,
	unsigned long long *duty)
{
	unsigned int div = pair & 15, pre = (control & 255) + 1;
	unsigned int total = (period_reg >> 16) + 1, active = period_reg & 65535;
	unsigned long long scale;
	if ((pair & (3U << 7)) || div > 8 || (control & (1U << 9)) || active > total)
		return -1;
	scale = (1ULL << div) * pre * 1000000000ULL;
	*period = (total * scale + 23999999) / 24000000;
	*duty = (active * scale + 23999999) / 24000000;
	return 0;
}
#endif
