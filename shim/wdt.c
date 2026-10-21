/*
 * MSM8937 APPS (APCS) watchdog: read, pet and disable.
 *
 * Runs with the MMU off (loader.c) or with the identity map (shim.c); both are
 * fine for Device memory accesses. Aligned 32-bit accesses only: with the MMU
 * off every byte is Device-nGnRnE, so unaligned access would fault.
 */
#include "wdt.h"

static inline uint32_t rd32(uintptr_t a)
{
	return *(volatile uint32_t *)a;
}

static inline void wr32(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
}

static inline void barrier(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

void wdt_read(struct wdt_state *s)
{
	s->en = rd32(WDT_BASE + WDT_EN);
	s->sts = rd32(WDT_BASE + WDT_STS);
	s->bark = rd32(WDT_BASE + WDT_BARK_TIME);
	s->bite = rd32(WDT_BASE + WDT_BITE_TIME);
}

void wdt_pet(void)
{
	wr32(WDT_BASE + WDT_RST, 1);
	barrier();
}

void wdt_disable(void)
{
	wr32(WDT_BASE + WDT_EN, 0);
	barrier();
}

uint32_t wdt_quiesce(struct wdt_state *before, struct wdt_state *after)
{
	wdt_read(before);
	if (before->en & WDT_EN_ENABLE) {
		wdt_pet();
		wdt_disable();
	}
	wdt_read(after);
	return before->en;
}
