/*
 * Freestanding ARM GICv2 driver for Snapdragon 435 / MSM8937 (santoni).
 *
 * Implements memory-mapped distributor and CPU interface initialization,
 * interrupt unmasking, and IAR/EOIR lifecycle handling.
 */
#include <stdint.h>
#include "gic.h"

/* Volatile MMIO write helper with data synchronization barrier */
static inline void gic_write32(uintptr_t addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Volatile MMIO read helper */
static inline uint32_t gic_read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

void gic_dist_init(void)
{
	uint32_t typer, it_lines, i;

	/* Step 1: Disable distributor forwarding during configuration */
	gic_write32(GICD_BASE + GICD_CTLR, 0);

	/* Step 2: Extract number of implemented interrupt lines */
	typer = gic_read32(GICD_BASE + GICD_TYPER);
	it_lines = (typer & 0x1f) + 1;

	/* Step 3: Configure all interrupts as Group 1 (Non-Secure) */
	for (i = 0; i < it_lines; i++) {
		gic_write32(GICD_BASE + GICD_IGROUPR(i), 0xffffffffU);
	}

	/* Step 4: Disable all SPIs and clear pending interrupts */
	for (i = 1; i < it_lines; i++) {
		gic_write32(GICD_BASE + GICD_ICENABLER(i), 0xffffffffU);
		gic_write32(GICD_BASE + GICD_ICPENDR(i), 0xffffffffU);
	}

	/* Step 5: Initialize all interrupt priorities to default (0xA0) */
	for (i = 0; i < it_lines * 8; i++) {
		gic_write32(GICD_BASE + GICD_IPRIORITYR(i), 0xa0a0a0a0U);
	}

	/* Step 6: Route all SPIs (INTID >= 32) to primary CPU 0 */
	for (i = 8; i < it_lines * 8; i++) {
		gic_write32(GICD_BASE + GICD_ITARGETSR(i), 0x01010101U);
	}

	/* Step 7: Enable distributor forwarding for Group 1 */
	gic_write32(GICD_BASE + GICD_CTLR, 0x01);
}

void gic_cpu_init(void)
{
	/* Unmask all priority levels */
	gic_write32(GICC_BASE + GICC_PMR, 0xff);

	/* Preemption group 0 (no sub-priority) */
	gic_write32(GICC_BASE + GICC_BPR, 0x00);

	/* Enable Group 1 interrupt signaling to the CPU */
	gic_write32(GICC_BASE + GICC_CTLR, 0x01);
}

void gic_enable_int(uint32_t intid)
{
	gic_write32(GICD_BASE + GICD_ISENABLER(intid / 32), 1U << (intid % 32));
}

void gic_disable_int(uint32_t intid)
{
	gic_write32(GICD_BASE + GICD_ICENABLER(intid / 32), 1U << (intid % 32));
}

uint32_t gic_ack(void)
{
	return gic_read32(GICC_BASE + GICC_IAR);
}

void gic_eoi(uint32_t iar)
{
	uint32_t intid = iar & 0x3ff;
	/* Ignore spurious interrupts (1020-1023) per GICv2 architecture specification */
	if (intid < 1020) {
		gic_write32(GICC_BASE + GICC_EOIR, iar);
	}
}
