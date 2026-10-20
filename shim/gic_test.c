/*
 * Host unit test for GICv2 register abstraction and lifecycle.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "gic.h"

static uint8_t mock_gicd[0x1000];
static uint8_t mock_gicc[0x1000];

#undef GICD_BASE
#undef GICC_BASE
#define GICD_BASE ((uintptr_t)mock_gicd)
#define GICC_BASE ((uintptr_t)mock_gicc)

static inline void gic_write32(uintptr_t addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
}

static inline uint32_t gic_read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

void gic_dist_init(void)
{
	uint32_t typer, it_lines, i;

	/* Disable distributor forwarding */
	gic_write32(GICD_BASE + GICD_CTLR, 0);

	/* Number of interrupt lines supported */
	typer = gic_read32(GICD_BASE + GICD_TYPER);
	it_lines = (typer & 0x1f) + 1;

	/* All interrupts to Group 1 (Non-Secure) */
	for (i = 0; i < it_lines; i++) {
		gic_write32(GICD_BASE + GICD_IGROUPR(i), 0xffffffffU);
	}

	/* Disable SPIs and clear pending */
	for (i = 1; i < it_lines; i++) {
		gic_write32(GICD_BASE + GICD_ICENABLER(i), 0xffffffffU);
		gic_write32(GICD_BASE + GICD_ICPENDR(i), 0xffffffffU);
	}

	/* Default priority for all lines (0xA0) */
	for (i = 0; i < it_lines * 8; i++) {
		gic_write32(GICD_BASE + GICD_IPRIORITYR(i), 0xa0a0a0a0U);
	}

	/* Route SPIs to CPU0 (bit 0) */
	for (i = 8; i < it_lines * 8; i++) {
		gic_write32(GICD_BASE + GICD_ITARGETSR(i), 0x01010101U);
	}

	/* Enable Group 1 forwarding */
	gic_write32(GICD_BASE + GICD_CTLR, 0x01);
}

void gic_cpu_init(void)
{
	/* Unmask all priority levels */
	gic_write32(GICC_BASE + GICC_PMR, 0xff);

	/* No preemption sub-groups */
	gic_write32(GICC_BASE + GICC_BPR, 0x00);

	/* Enable Group 1 signaling to CPU */
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
	if (intid < 1020) {
		gic_write32(GICC_BASE + GICC_EOIR, iar);
	}
}

int main(void)
{
	memset(mock_gicd, 0, sizeof(mock_gicd));
	memset(mock_gicc, 0, sizeof(mock_gicc));

	/* GICD_TYPER = 0 => it_lines = 1 (32 lines total) */
	*(uint32_t *)(mock_gicd + GICD_TYPER) = 0;

	/* Test distributor init */
	gic_dist_init();
	assert((*(uint32_t *)(mock_gicd + GICD_CTLR) & 0x01) == 0x01);
	assert(*(uint32_t *)(mock_gicd + GICD_IGROUPR(0)) == 0xffffffffU);
	assert(*(uint32_t *)(mock_gicd + GICD_IPRIORITYR(0)) == 0xa0a0a0a0U);

	/* Test CPU interface init */
	gic_cpu_init();
	assert(*(uint32_t *)(mock_gicc + GICC_PMR) == 0xff);
	assert(*(uint32_t *)(mock_gicc + GICC_BPR) == 0x00);
	assert(*(uint32_t *)(mock_gicc + GICC_CTLR) == 0x01);

	/* Test interrupt enable / disable for INTID 20 (PPI 4) */
	gic_enable_int(20);
	assert(*(uint32_t *)(mock_gicd + GICD_ISENABLER(0)) == (1U << 20));

	gic_disable_int(20);
	assert(*(uint32_t *)(mock_gicd + GICD_ICENABLER(0)) == (1U << 20));

	/* Test IAR parsing */
	*(uint32_t *)(mock_gicc + GICC_IAR) = 20;
	uint32_t iar = gic_ack();
	assert((iar & 0x3ff) == 20);

	/* Test EOI writing */
	*(uint32_t *)(mock_gicc + GICC_EOIR) = 0;
	gic_eoi(iar);
	assert(*(uint32_t *)(mock_gicc + GICC_EOIR) == 20);

	/* Test spurious interrupt (1023) filtering */
	*(uint32_t *)(mock_gicc + GICC_IAR) = 1023;
	iar = gic_ack();
	assert((iar & 0x3ff) == 1023);

	*(uint32_t *)(mock_gicc + GICC_EOIR) = 0;
	gic_eoi(iar);
	/* Spurious interrupts should NOT write to EOIR */
	assert(*(uint32_t *)(mock_gicc + GICC_EOIR) == 0);

	printf("gic_test: all assertions passed\n");
	return 0;
}
