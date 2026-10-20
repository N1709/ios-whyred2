#pragma once
#include <stdint.h>

/* Physical base addresses for Qualcomm QGIC2 on MSM8937 */
#define GICD_BASE		0x0b000000UL	/* santoni QGIC2 distributor (4 KiB) */
#define GICC_BASE		0x0b002000UL	/* santoni QGIC2 CPU interface (4 KiB) */

/* Distributor Register Offsets */
#define GICD_CTLR		0x000		/* Distributor Control Register */
#define GICD_TYPER		0x004		/* Interrupt Controller Type Register */
#define GICD_IIDR		0x008		/* Distributor Implementer Identification Register */
#define GICD_IGROUPR(n)		(0x080 + ((n) * 4))	/* Interrupt Group Registers */
#define GICD_ISENABLER(n)	(0x100 + ((n) * 4))	/* Interrupt Set-Enable Registers */
#define GICD_ICENABLER(n)	(0x180 + ((n) * 4))	/* Interrupt Clear-Enable Registers */
#define GICD_ISPENDR(n)		(0x200 + ((n) * 4))	/* Interrupt Set-Pending Registers */
#define GICD_ICPENDR(n)		(0x280 + ((n) * 4))	/* Interrupt Clear-Pending Registers */
#define GICD_IPRIORITYR(n)	(0x400 + ((n) * 4))	/* Interrupt Priority Registers */
#define GICD_ITARGETSR(n)	(0x800 + ((n) * 4))	/* Interrupt Processor Targets Registers */
#define GICD_ICFGR(n)		(0xc00 + ((n) * 4))	/* Interrupt Configuration Registers */

/* CPU Interface Register Offsets */
#define GICC_CTLR		0x000		/* CPU Interface Control Register */
#define GICC_PMR		0x004		/* Interrupt Priority Mask Register */
#define GICC_BPR		0x008		/* Binary Point Register */
#define GICC_IAR		0x00c		/* Interrupt Acknowledge Register */
#define GICC_EOIR		0x010		/* End of Interrupt Register */
#define GICC_IIDR		0x0fc		/* CPU Interface Identification Register */

/* Driver function prototypes */
void gic_dist_init(void);
void gic_cpu_init(void);
void gic_enable_int(uint32_t intid);
void gic_disable_int(uint32_t intid);
uint32_t gic_ack(void);
void gic_eoi(uint32_t iar);
