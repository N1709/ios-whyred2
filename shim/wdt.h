/*
 * MSM8937 APPS (APCS) watchdog.
 *
 * The block lives at 0x0b017000 in the APCS region; the layout below is the
 * "qcom,kpss-wdt" one used by both the downstream msm-3.18 watchdog_v2.c
 * (drivers/soc/qcom/watchdog_v2.c) and mainline qcom-wdt.c for msm8937.
 *
 * The kernel arms it with an 11 s bark and a 10 s pet interval (msm8937.dtsi
 * qcom,wdt@b017000), and the bite is taken by the secure world, which resets
 * the SoC. Nothing in LK touches these registers, so whatever the boot chain
 * left behind is still programmed when the shim starts.
 */
#ifndef SHIM_WDT_H
#define SHIM_WDT_H

#include <stdint.h>

#define WDT_BASE		0x0b017000UL
#define WDT_RST			0x04	/* write 1: pet */
#define WDT_EN			0x08	/* bit 0: enable, bit 1: unmasked bark */
#define WDT_STS			0x0c	/* [20:1]: current count */
#define WDT_BARK_TIME		0x10
#define WDT_BITE_TIME		0x14

#define WDT_EN_ENABLE		(1U << 0)
#define WDT_EN_UNMASKED_INT	(1U << 1)

struct wdt_state {
	uint32_t en, sts, bark, bite;
};

void wdt_read(struct wdt_state *s);
void wdt_pet(void);
void wdt_disable(void);

/*
 * Snapshot before, then pet + clear EN if the watchdog is enabled, then
 * snapshot after. Returns the pre-clear EN value; the caller can compare
 * before/after to see whether the disable actually stuck.
 */
uint32_t wdt_quiesce(struct wdt_state *before, struct wdt_state *after);

#endif
