/*
 * Freestanding ARM Generic Architectural Timer driver for MSM8937 (Cortex-A53).
 *
 * Programs CNTV_CTL_EL0, CNTV_TVAL_EL0, and CNTV_CVAL_EL0 without GICv3 system registers.
 */
#include <stdint.h>
#include "timer.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

uint64_t timer_get_freq(void)
{
	return rsr("cntfrq_el0");
}

uint64_t timer_get_ticks(void)
{
	isb();
	return rsr("cntvct_el0");
}

void timer_arm_ticks(uint32_t ticks)
{
	wsr("cntv_tval_el0", ticks);
	wsr("cntv_ctl_el0", 1);		/* ENABLE=1, IMASK=0 */
	isb();
}

void timer_arm_deadline(uint64_t deadline)
{
	wsr("cntv_cval_el0", deadline);
	wsr("cntv_ctl_el0", 1);		/* ENABLE=1, IMASK=0 */
	isb();
}

void timer_disarm(void)
{
	wsr("cntv_ctl_el0", 2);		/* ENABLE=0, IMASK=1 */
	isb();
}
