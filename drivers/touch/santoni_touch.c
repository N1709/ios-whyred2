/*
 * WHYRED touch bring-up, step 1: does the panel's touch controller answer on
 * the BLSP1 QUP1 bus at all?
 *
 * whyred has no I2C controller driver in this kernel, so this file does not
 * touch the QUP engine. It re-muxes the two bus lines as plain TLMM GPIOs
 * (SDA GPIO 2, SCL GPIO 3, both blsp_i2c1 in the whyred DTS) and bit-bangs
 * I2C at roughly 100 kHz. That is enough to answer one question per boot: is
 * the controller powered, out of reset and acknowledging its address? The
 * Novatek chip ID is read in the same run. A QUP driver and the HID report
 * path come only after an address answers.
 *
 * Register layout follows the vendor pinctrl-sdm660 tables: one 0x1000
 * window per pin, CTRL at +0x0 (mux bits 2..5, pull bits 0..1, drive bits
 * 6..8, output enable bit 9) and IO at +0x4 (bit 0 pad level, bit 1 driven
 * value). GPIO 2/3 live in the SOUTH tile (TLMM + 0x100000); the reset/IRQ
 * pair lives in NORTH (TLMM + 0x900000). The internal pull-up is used
 * because whyred has no external pull resistors on this bus.
 *
 * Boot-args:
 *   sweetouchi2c=1    enable the probe
 *   sweetouchreset=1  drive reset (GPIO 66) low for 10 ms and high for
 *                     60 ms before the scan; use it only in the boot that
 *                     asks whether the bootloader hand-off left the
 *                     controller held in reset
 *
 * The probe is read-mostly on purpose: with no reset pulse it does not touch
 * GPIO 66/67 at all, so the log still describes the state ABL handed over.
 */
#if defined(ARM64_BOARD_CONFIG_WHYRED)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/memory_types.h>
#include <vm/pmap.h>

/* osfmk/arm/machine_routines.h, not exported to the BSD layer */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size,
    uint32_t flags);

#define WHYRED_TLMM_BASE	0x03400000ULL
#define WHYRED_TLMM_SOUTH	0x00100000u	/* GPIO 0..15 */
#define WHYRED_TLMM_NORTH	0x00900000u	/* GPIO 66..67 */

#define TLMM_REG_SIZE		0x1000u
#define TLMM_CTRL		0x000u
#define TLMM_IO			0x004u
#define TLMM_OE			(1u << 9)

#define WHYRED_SDA		2
#define WHYRED_SCL		3
#define WHYRED_RST		66
#define WHYRED_IRQ		67

#define TOUCH_I2C_DELAY_US	10	/* half a bit, ~50 kHz: the internal
					 * pull-up makes the edges slow */

static inline uint32_t
tlmm_rd(vm_offset_t addr)
{
	uint32_t v = *(volatile uint32_t *)addr;

	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline void
tlmm_wr(vm_offset_t addr, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)addr = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

struct touch_pin {
	vm_offset_t cfg;
	vm_offset_t io;
	uint32_t idle;		/* CTRL with mux=gpio, pull-up, OE=0 */
};

static struct touch_pin touch_sda, touch_scl;
static int touch_probe_done;

/*
 * Re-mux one pad as a plain GPIO with the internal pull-up on. Returns the
 * CTRL value so output/input transitions do not have to re-read it.
 */
static uint32_t
pin_gpio(vm_offset_t ctl)
{
	uint32_t v = tlmm_rd(ctl);

	v &= ~(0xfu << 2);	/* mux 0: plain GPIO, not blsp_i2c1 */
	v &= ~(3u << 0);
	v |= (3u << 0);		/* pull-up */
	v &= ~(7u << 6);
	v |= (3u << 6);		/* drive strength 3 */
	v &= ~TLMM_OE;		/* start as input */
	tlmm_wr(ctl, v);
	return v;
}

static void
pin_setup(struct touch_pin *p, vm_offset_t tile, unsigned pin)
{
	p->cfg = tile + TLMM_REG_SIZE * pin;
	p->io = p->cfg + (TLMM_IO - TLMM_CTRL);
	p->idle = pin_gpio(p->cfg);
}

static void
pin_out(struct touch_pin *p, int value)
{
	uint32_t io = tlmm_rd(p->io);

	/* Value first, then output enable: no glitch on the bus. */
	io = (io & ~(1u << 1)) | (value ? (1u << 1) : 0u);
	tlmm_wr(p->io, io);
	tlmm_wr(p->cfg, p->idle | TLMM_OE);
}

static void
pin_in(struct touch_pin *p)
{
	tlmm_wr(p->cfg, p->idle);
}

static int
pin_read(struct touch_pin *p)
{
	return (tlmm_rd(p->io) & 1u) != 0;
}

static void
bb_delay(void)
{
	delay(TOUCH_I2C_DELAY_US);
}

static void
i2c_start(void)
{
	pin_out(&touch_sda, 1);
	bb_delay();
	pin_out(&touch_scl, 1);
	bb_delay();
	pin_out(&touch_sda, 0);
	bb_delay();
	pin_out(&touch_scl, 0);
	bb_delay();
}

static void
i2c_stop(void)
{
	pin_out(&touch_sda, 0);
	bb_delay();
	pin_out(&touch_scl, 1);
	bb_delay();
	pin_out(&touch_sda, 1);
	bb_delay();
}

/* Returns 1 when the slave acknowledged the byte. */
static int
i2c_write(uint8_t byte)
{
	int bit, ack;

	for (bit = 7; bit >= 0; bit--) {
		pin_out(&touch_sda, (byte >> bit) & 1);
		bb_delay();
		pin_out(&touch_scl, 1);
		bb_delay();
		pin_out(&touch_scl, 0);
		bb_delay();
	}
	pin_in(&touch_sda);
	bb_delay();
	pin_out(&touch_scl, 1);
	bb_delay();
	ack = !pin_read(&touch_sda);
	pin_out(&touch_scl, 0);
	bb_delay();
	return ack;
}

static uint8_t
i2c_read(int ack)
{
	uint8_t value = 0;
	int bit;

	pin_in(&touch_sda);
	bb_delay();
	for (bit = 0; bit < 8; bit++) {
		pin_out(&touch_scl, 1);
		bb_delay();
		value = (uint8_t)((value << 1) | (pin_read(&touch_sda) ? 1 : 0));
		pin_out(&touch_scl, 0);
		bb_delay();
	}
	if (ack) {
		pin_out(&touch_sda, 0);
	} else {
		pin_in(&touch_sda);
	}
	bb_delay();
	pin_out(&touch_scl, 1);
	bb_delay();
	pin_out(&touch_scl, 0);
	bb_delay();
	if (ack) {
		pin_in(&touch_sda);
	}
	return value;
}

/*
 * Clock a slave out of a half-finished transfer: nine pulses with SDA
 * released. Only used when the idle bus is stuck low, which is the one case
 * where a scan would otherwise report nothing at all.
 */
static int
i2c_clear_bus(void)
{
	int i, pulsed = 0;

	pin_in(&touch_sda);
	for (i = 0; i < 9 && !pin_read(&touch_sda); i++) {
		pin_out(&touch_scl, 1);
		bb_delay();
		pin_out(&touch_scl, 0);
		bb_delay();
		pulsed++;
	}
	if (pulsed) {
		i2c_stop();
	}
	return pulsed;
}

static int
i2c_ack(uint8_t addr)
{
	int ack;

	i2c_start();
	ack = i2c_write((uint8_t)(addr << 1));
	i2c_stop();
	return ack;
}

/*
 * Novatek register read: page select through 0xFF, then the byte offset
 * inside that page, then a repeated start and the data. The controller
 * answers at 0x62 in bootloader mode and at 0x01 once its firmware runs.
 */
static int
nvt_read(uint8_t addr, uint8_t page, uint8_t reg, uint8_t *buf, int len)
{
	int i, ok = 1;

	i2c_start();
	ok &= i2c_write((uint8_t)(addr << 1));
	ok &= i2c_write(0xff);
	ok &= i2c_write(0x00);
	ok &= i2c_write(page);
	i2c_stop();
	if (!ok) {
		return EIO;
	}
	i2c_start();
	ok &= i2c_write((uint8_t)(addr << 1));
	ok &= i2c_write(reg);
	if (!ok) {
		i2c_stop();
		return EIO;
	}
	i2c_start();
	ok &= i2c_write((uint8_t)((addr << 1) | 1));
	if (!ok) {
		i2c_stop();
		return EIO;
	}
	for (i = 0; i < len; i++) {
		buf[i] = i2c_read(i + 1 < len);
	}
	i2c_stop();
	return 0;
}

int
santoni_touch_probe(void)
{
	vm_offset_t south, north;
	uint32_t on = 0, pulse = 0;
	uint8_t id[3];
	int addr, acks = 0, pulsed;
	uint32_t rst_cfg, rst_io, irq_cfg, irq_io;

	if (touch_probe_done) {
		return 0;
	}
	if (!PE_parse_boot_argn("sweetouchi2c", &on, sizeof(on)) || !on) {
		return 0;
	}
	touch_probe_done = 1;

	south = ml_io_map_unmappable(WHYRED_TLMM_BASE + WHYRED_TLMM_SOUTH,
	    0x10000, VM_WIMG_IO);
	north = ml_io_map_unmappable(WHYRED_TLMM_BASE + WHYRED_TLMM_NORTH,
	    0x50000, VM_WIMG_IO);
	if (south == 0 || north == 0) {
		printf("WHYRED TOUCH: TLMM map failed (south %p north %p)\n",
		    (void *)south, (void *)north);
		return ENOMEM;
	}

	/* Raw reset/IRQ state before anything is touched. */
	rst_cfg = tlmm_rd(north + TLMM_REG_SIZE * WHYRED_RST + TLMM_CTRL);
	rst_io = tlmm_rd(north + TLMM_REG_SIZE * WHYRED_RST + TLMM_IO);
	irq_cfg = tlmm_rd(north + TLMM_REG_SIZE * WHYRED_IRQ + TLMM_CTRL);
	irq_io = tlmm_rd(north + TLMM_REG_SIZE * WHYRED_IRQ + TLMM_IO);
	printf("WHYRED TOUCH: tlmm south %p north %p; rst cfg %08x io %08x; "
	    "irq cfg %08x io %08x\n", (void *)south, (void *)north, rst_cfg,
	    rst_io, irq_cfg, irq_io);

	pin_setup(&touch_sda, south, WHYRED_SDA);
	pin_setup(&touch_scl, south, WHYRED_SCL);
	printf("WHYRED TOUCH: sda%d ctl %08x scl%d ctl %08x; idle sda %d scl "
	    "%d\n", WHYRED_SDA, touch_sda.idle, WHYRED_SCL, touch_scl.idle,
	    pin_read(&touch_sda), pin_read(&touch_scl));

	pulsed = i2c_clear_bus();
	if (pulsed) {
		printf("WHYRED TOUCH: bus was stuck low, %d clocks\n", pulsed);
	}

	if (PE_parse_boot_argn("sweetouchreset", &pulse, sizeof(pulse)) && pulse) {
		struct touch_pin rst;

		pin_setup(&rst, north, WHYRED_RST);
		pin_out(&rst, 0);
		delay(10000);
		pin_out(&rst, 1);
		delay(60000);
		printf("WHYRED TOUCH: reset pulse low 10 ms, high 60 ms\n");
	}

	for (addr = 0x08; addr <= 0x77; addr++) {
		if (!i2c_ack((uint8_t)addr)) {
			continue;
		}
		acks++;
		printf("WHYRED TOUCH: ack 0x%02x\n", addr);
		if (nvt_read((uint8_t)addr, 0x00, 0x00, id, sizeof(id)) == 0) {
			printf("WHYRED TOUCH: id 0x%02x %02x %02x %02x\n", addr,
			    id[0], id[1], id[2]);
		} else {
			printf("WHYRED TOUCH: id 0x%02x read failed\n", addr);
		}
	}

	printf("WHYRED TOUCH: scan done, %d address(es) acknowledged; sda %d "
	    "scl %d\n", acks, pin_read(&touch_sda), pin_read(&touch_scl));
	return acks ? 0 : ENXIO;
}

#else /* !ARM64_BOARD_CONFIG_WHYRED */

int
santoni_touch_probe(void)
{
	return ENXIO;
}

#endif /* ARM64_BOARD_CONFIG_WHYRED */
