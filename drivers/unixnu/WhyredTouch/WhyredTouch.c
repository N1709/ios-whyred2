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
 * pair lives in NORTH (TLMM + 0x900000), both relative to the vendor
 * pinctrl@03000000 window. The internal pull-up is used
 * because whyred has no external pull resistors on this bus.
 *
 * Boot-args:
 *   sweetouchi2c=1    enable the probe
 *   sweetouchreset=1  drive reset (GPIO 66) low for 10 ms and high for
 *                     60 ms before the scan; use it only in the boot that
 *                     asks whether the bootloader hand-off left the
 *                     controller held in reset
 *   sweetouchwait=N   hold the panel with the result for N seconds after
 *                     the scan (default 15, 0 continues at once, max 60)
 * The 0184 ring showed exactly one acknowledging address, 0x20. That is a
 * Synaptics part, not the Novatek 0x62/0x01 pair, so 0186 adds a real RMI4
 * read (two-byte register address, no page prefix), dumps the version
 * register, the function descriptor table and the F01/F12 query blocks.
 *
 *   sweetouchtouch=N  poll F12 touch data for N seconds: the top band of
 *                     the panel is blue while waiting and turns green on the
 *                     first change, and every change is written to the ring
 *   sweetouchhalt=1   diagnostic: paint the answer on the panel and stop
 *                     the boot there, repeating the result line forever.
 *                     The console stops reaching the panel somewhere around
 *                     the framebuffer hand-off, so the answer is written
 *                     straight into the scanout buffer instead of relying
 *                     on printf being visible.
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

#define WHYRED_TLMM_BASE	0x03000000ULL	/* vendor DTS: pinctrl@03000000 */
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

/*
 * Open-drain bit-bang. Only ever drive a line low; release it (input with
 * the internal pull-up) for a high. A push-pull high would fight a slave
 * that holds SCL low to stretch the clock - and a Synaptics controller does
 * stretch while it fetches a register - so every high is a release followed
 * by a wait for the line to actually rise. A timeout keeps a stuck bus from
 * hanging the boot.
 */
#define TOUCH_LINE_WAIT_US	2000

static void
bb_delay(void)
{
	delay(TOUCH_I2C_DELAY_US);
}

static void
sda_low(void)
{
	pin_out(&touch_sda, 0);
}

static void
scl_low(void)
{
	pin_out(&touch_scl, 0);
}

static int
sda_release(void)
{
	int i;

	pin_in(&touch_sda);
	for (i = 0; i < TOUCH_LINE_WAIT_US; i++) {
		if (pin_read(&touch_sda)) {
			return 1;
		}
		delay(1);
	}
	return 0;
}

static int
scl_release(void)
{
	int i;

	pin_in(&touch_scl);
	for (i = 0; i < TOUCH_LINE_WAIT_US; i++) {
		if (pin_read(&touch_scl)) {
			return 1;
		}
		delay(1);
	}
	return 0;
}

static void
i2c_start(void)
{
	sda_release();
	bb_delay();
	scl_release();
	bb_delay();
	sda_low();
	bb_delay();
	scl_low();
	bb_delay();
}

static void
i2c_stop(void)
{
	sda_low();
	bb_delay();
	scl_release();
	bb_delay();
	sda_release();
	bb_delay();
}

/* Returns 1 when the slave acknowledged the byte. */
static int
i2c_write(uint8_t byte)
{
	int bit, ack;

	for (bit = 7; bit >= 0; bit--) {
		if ((byte >> bit) & 1) {
			sda_release();
		} else {
			sda_low();
		}
		bb_delay();
		scl_release();
		bb_delay();
		scl_low();
		bb_delay();
	}
	sda_release();
	bb_delay();
	scl_release();
	bb_delay();
	ack = !pin_read(&touch_sda);
	scl_low();
	bb_delay();
	return ack;
}

static uint8_t
i2c_read(int ack)
{
	uint8_t value = 0;
	int bit;

	sda_release();
	bb_delay();
	for (bit = 0; bit < 8; bit++) {
		scl_release();
		bb_delay();
		value = (uint8_t)((value << 1) | (pin_read(&touch_sda) ? 1 : 0));
		scl_low();
		bb_delay();
	}
	if (ack) {
		sda_low();
	}
	bb_delay();
	scl_release();
	bb_delay();
	scl_low();
	bb_delay();
	sda_release();
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

	sda_release();
	for (i = 0; i < 9 && !pin_read(&touch_sda); i++) {
		scl_release();
		bb_delay();
		scl_low();
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

/*
 * Synaptics DSX over I2C. The vendor driver
 * (drivers/input/touchscreen/synaptics_dsx/synaptics_dsx_i2c.c) selects a
 * page with a two-byte write 0xFF <page>, then writes the low address byte
 * and reads the data with a repeated start. The earlier RMI4 guess (two
 * address bytes, no 0xFF) was wrong: it wrote the second address byte into
 * register 0x00 of the chip.
 *
 * Register map used here, from synaptics_dsx_core.h: the page descriptor
 * table starts at 0x00E9 and steps down by 6 bytes per entry; each entry is
 * query base (2 bytes, big endian), command base, control base, data base and
 * a function number in the low six bits of the last byte.
 */
#define DSX_ADDR		0x20
#define DSX_PDT_START		0x00e9
#define DSX_PDT_END		0x00d0
#define DSX_PDT_ENTRY_SIZE	0x0006
#define DSX_F01_QUERY_LEN	21
#define DSX_PRODUCT_ID_OFFSET	11
#define DSX_PRODUCT_ID_SIZE	10

static int
dsx_read_ex(uint8_t addr, uint16_t reg, uint8_t *buf, int len,
    int use_page, int addr_bytes)
{
	int i, ok = 1;

	if (use_page) {
		/* page select: 0xFF <page> */
		i2c_start();
		ok &= i2c_write((uint8_t)(addr << 1));
		ok &= i2c_write(0xff);
		ok &= i2c_write((uint8_t)((reg >> 8) & 0xff));
		i2c_stop();
		if (!ok) {
			return EIO;
		}
	}

	i2c_start();
	ok &= i2c_write((uint8_t)(addr << 1));
	if (addr_bytes == 2) {
		ok &= i2c_write((uint8_t)((reg >> 8) & 0xff));
	}
	ok &= i2c_write((uint8_t)(reg & 0xff));
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

/* The vendor driver's read: page select, then one address byte. */
static int
dsx_read(uint8_t addr, uint16_t reg, uint8_t *buf, int len)
{
	return dsx_read_ex(addr, reg, buf, len, 1, 1);
}

/* Hex plus printable text: the product and build strings are ASCII. */
static void
dsx_dump(const char *what, uint16_t reg, const uint8_t *b, int len)
{
	char text[32];
	int i;

	printf("WHYRED TOUCH: dsx %s [0x%04x]", what, reg);
	for (i = 0; i < len; i++) {
		printf(" %02x", b[i]);
	}
	printf(" |");
	for (i = 0; i < len && i < (int)sizeof(text) - 1; i++) {
		text[i] = (b[i] >= 0x20 && b[i] < 0x7f) ? (char)b[i] : '.';
	}
	text[i] = 0;
	printf("%s|\n", text);
}

struct dsx_funcs {
	uint16_t f01_query;
	uint16_t f12_query;
	uint16_t f12_ctrl;
	uint16_t f12_data;
	int found_f01;
	int found_f12;
	int entries;
};

/*
 * Read-only identification. No control register is written, so a wrong
 * address cannot wedge the controller: the page table, the F01 query block
 * (manufacturer, product string, build id) and the F12 query block are all
 * plain reads.
 */
static void
dsx_identify(struct dsx_funcs *f)
{
	uint8_t b[DSX_F01_QUERY_LEN];
	uint16_t addr;

	f->f01_query = 0;
	f->f12_query = 0;
	f->f12_ctrl = 0;
	f->f12_data = 0;
	f->found_f01 = 0;
	f->found_f12 = 0;
	f->entries = 0;

	for (addr = DSX_PDT_START; addr > DSX_PDT_END;
	    addr = (uint16_t)(addr - DSX_PDT_ENTRY_SIZE)) {
		unsigned fn;

		if (dsx_read(DSX_ADDR, addr, b, DSX_PDT_ENTRY_SIZE) != 0) {
			printf("WHYRED TOUCH: dsx pdt read failed at 0x%04x\n",
			    addr);
			return;
		}
		if (!b[0] && !b[1] && !b[2] && !b[3] && !b[4] && !b[5]) {
			continue;	/* unused slot */
		}
		fn = b[5] & 0x3f;
		f->entries++;
		printf("WHYRED TOUCH: dsx fn %02x query %02x%02x cmd %02x "
		    "ctrl %02x data %02x\n", fn, b[0], b[1], b[2], b[3], b[4]);
		if (fn == 0x01 && !f->found_f01) {
			f->f01_query = (uint16_t)((b[0] << 8) | b[1]);
			f->found_f01 = 1;
		} else if (fn == 0x12 && !f->found_f12) {
			f->f12_query = (uint16_t)((b[0] << 8) | b[1]);
			f->f12_ctrl = b[3];
			f->f12_data = b[4];
			f->found_f12 = 1;
		}
	}

	if (!f->found_f01) {
		printf("WHYRED TOUCH: dsx no F01 in the page table\n");
		return;
	}
	if (dsx_read(DSX_ADDR, f->f01_query, b, DSX_F01_QUERY_LEN) != 0) {
		printf("WHYRED TOUCH: dsx F01 query read failed\n");
		return;
	}
	dsx_dump("f01", f->f01_query, b, DSX_F01_QUERY_LEN);
	printf("WHYRED TOUCH: dsx manufacturer %u product \"%.*s\" build "
	    "%02x%02x%02x\n", b[0], DSX_PRODUCT_ID_SIZE,
	    &b[DSX_PRODUCT_ID_OFFSET], b[18], b[19], b[20]);

	if (f->found_f12) {
		if (dsx_read(DSX_ADDR, f->f12_query, b, 12) == 0) {
			dsx_dump("f12q", f->f12_query, b, 12);
		}
		printf("WHYRED TOUCH: dsx f12 ctrl 0x%04x data 0x%04x\n",
		    f->f12_ctrl, f->f12_data);
	}
}

/* Painted band across the top of the panel; defined with panel_report below. */
static void panel_band(uint32_t colour);

/*
 * 0187 could not read the page descriptor table: reading 0x00e9 and then
 * 0x00e3 returned one continuous byte stream (0x00e9 -> 27 00 09 00 21 34,
 * 0x00e3 -> 2f 26 0d 04 01 01), and the "first" PDT entry decoded as
 * function 0x34, which cannot be the entry at 0x00e9 - that slot always holds
 * F01. Either the address phase is not taking effect or the chip is in a
 * state where it only streams. This diagnostic answers both questions in one
 * boot, read-only:
 *
 *   same address twice      does the second read repeat or advance?
 *   0x00e9, one byte        the vendor driver's protocol
 *   0x00e9, two bytes       the RMI4 protocol 0186 used
 *   0x00e8 / 0x00ea         off-by-one check on the same protocol
 *   0x00e9, no page select  page select itself may be the problem
 *   page 1, 0x0000          does 0xFF <page> reach a different page?
 */
static void
dsx_diag(void)
{
	uint8_t a[8], b[8];
	int i, same = 1;

	if (dsx_read_ex(DSX_ADDR, 0x0000, a, sizeof(a), 1, 1) == 0) {
		dsx_dump("diag 0000 a1 #1", 0x0000, a, sizeof(a));
	} else {
		printf("WHYRED TOUCH: diag 0000 a1 #1 read failed\n");
	}
	delay(2000);
	if (dsx_read_ex(DSX_ADDR, 0x0000, b, sizeof(b), 1, 1) == 0) {
		dsx_dump("diag 0000 a1 #2", 0x0000, b, sizeof(b));
	} else {
		printf("WHYRED TOUCH: diag 0000 a1 #2 read failed\n");
	}
	for (i = 0; i < (int)sizeof(a); i++) {
		if (a[i] != b[i]) {
			same = 0;
		}
	}
	printf("WHYRED TOUCH: diag same address twice -> %s\n",
	    same ? "repeated" : "advanced");

	if (dsx_read_ex(DSX_ADDR, 0x00e9, a, 6, 1, 1) == 0) {
		dsx_dump("diag 00e9 a1", 0x00e9, a, 6);
	}
	if (dsx_read_ex(DSX_ADDR, 0x00e9, a, 6, 1, 2) == 0) {
		dsx_dump("diag 00e9 a2", 0x00e9, a, 6);
	}
	if (dsx_read_ex(DSX_ADDR, 0x00e8, a, 6, 1, 1) == 0) {
		dsx_dump("diag 00e8 a1", 0x00e8, a, 6);
	}
	if (dsx_read_ex(DSX_ADDR, 0x00ea, a, 6, 1, 1) == 0) {
		dsx_dump("diag 00ea a1", 0x00ea, a, 6);
	}
	if (dsx_read_ex(DSX_ADDR, 0x00e9, a, 6, 0, 2) == 0) {
		dsx_dump("diag 00e9 a2 nopage", 0x00e9, a, 6);
	}
	if (dsx_read_ex(DSX_ADDR, 0x0100, a, 6, 1, 1) == 0) {
		dsx_dump("diag page1 0000 a1", 0x0100, a, 6);
	}
}

/*
 * Read F12's 2D data block and report every change. The layout is not
 * assumed: the first sample is the idle snapshot, and a difference in any of
 * the eleven bytes means the controller is reporting something. The top band
 * is blue while nothing is seen and turns green on the first change, so the
 * panel itself answers "does the digitizer see my finger?" without a console.
 */
static struct touch_pin touch_irq;
static int touch_irq_ready;

static void
touch_irq_setup(vm_offset_t north)
{
	if (!touch_irq_ready) {
		pin_setup(&touch_irq, north, WHYRED_IRQ);
		touch_irq_ready = 1;
	}
}

static void
dsx_touch_test(const struct dsx_funcs *f, uint32_t seconds)
{
	uint8_t idle[11], b[11];
	int i, touched = 0, irq_level, irq_edges = 0;

	if (!f->found_f12 || !f->f12_data) {
		printf("WHYRED TOUCH: no F12 data base, cannot poll touch\n");
		return;
	}
	if (seconds > 60) {
		seconds = 60;	/* keep the boot bounded */
	}
	if (dsx_read(DSX_ADDR, f->f12_data, idle, sizeof(idle)) != 0) {
		printf("WHYRED TOUCH: F12 data read failed\n");
		return;
	}
	printf("WHYRED TOUCH: polling F12 0x%04x for %u s; idle %02x %02x %02x "
	    "%02x %02x\n", f->f12_data, seconds, idle[0], idle[1], idle[2],
	    idle[3], idle[4]);
	irq_level = touch_irq_ready ? pin_read(&touch_irq) : 1;
	printf("WHYRED TOUCH: irq %d at start\n", irq_level);
	panel_band(0xff2040e0);		/* blue: waiting for a finger */

	for (i = 0; i < (int)(seconds * 50); i++) {
		int changed = 0;

		if (touch_irq_ready) {
			int level = pin_read(&touch_irq);

			if (level != irq_level) {
				irq_edges++;
				printf("WHYRED TOUCH: irq %d -> %d (edge %d)\n",
				    irq_level, level, irq_edges);
				irq_level = level;
				if (!touched) {
					touched = 1;
					panel_band(0xff20e020);
				}
			}
		}
		if (dsx_read(DSX_ADDR, f->f12_data, b, sizeof(b)) != 0) {
			printf("WHYRED TOUCH: F12 data read failed\n");
			break;
		}
		for (int k = 0; k < (int)sizeof(b); k++) {
			if (b[k] != idle[k]) {
				changed = 1;
			}
		}
		if (changed) {
			printf("WHYRED TOUCH: f12 %02x %02x %02x %02x %02x %02x "
			    "%02x %02x %02x %02x %02x\n", b[0], b[1], b[2], b[3],
			    b[4], b[5], b[6], b[7], b[8], b[9], b[10]);
			for (int k = 0; k < (int)sizeof(b); k++) {
				idle[k] = b[k];
			}
			if (!touched) {
				touched = 1;
				panel_band(0xff20e020);	/* green: seen */
			}
		} else if (i % 50 == 0) {
			/* Once a second: prove the poll is alive and show
			 * what the chip reports while nobody touches it. */
			printf("WHYRED TOUCH: poll %u s data %02x %02x %02x %02x "
			    "%02x irq %d\n", i / 50, b[0], b[1], b[2], b[3],
			    b[4], irq_level);
		}
		delay(20000);
	}
	printf("WHYRED TOUCH: touch test done (%s)\n",
	    touched ? "data changed" : "nothing seen");
}

/*
 * whyred's panel scanout buffer, same constants santoni_fb_test() uses
 * (bsd/dev/santoni_sdhci.c). Painting here is independent of the console:
 * the four-bar test proved writes to this address reach the panel.
 *
 * Colour code, top quarter of the screen:
 *   white    first ack 0x01  Novatek running firmware
 *   yellow   first ack 0x62  Novatek still in bootloader
 *   cyan     first ack 0x20  Synaptics panel
 *   magenta  some other address answered
 *   red      nothing acknowledged
 * Bottom three quarters: green if at least one address answered, else red.
 */
#define WHYRED_FB_PHYS		0x9d400000ULL
#define WHYRED_FB_WIDTH		1080u
#define WHYRED_FB_HEIGHT	2160u
#define WHYRED_FB_STRIDE	4320u	/* bytes, 32 bpp */

static uint32_t
panel_code(int acks, uint8_t first)
{
	if (acks <= 0) {
		return 0xffe02020;	/* red */
	}
	switch (first) {
	case 0x01:
		return 0xffffffff;	/* white */
	case 0x62:
		return 0xffffff20;	/* yellow */
	case 0x20:
		return 0xff20e0e0;	/* cyan */
	default:
		return 0xffff20ff;	/* magenta */
	}
}

static volatile uint32_t *panel_fb;

static volatile uint32_t *
panel_map(void)
{
	if (panel_fb == NULL) {
		panel_fb = (volatile uint32_t *)ml_io_map_unmappable(
		    WHYRED_FB_PHYS,
		    (vm_size_t)WHYRED_FB_STRIDE * WHYRED_FB_HEIGHT,
		    VM_WIMG_IO);
	}
	return panel_fb;
}

static int
panel_fill_rows(uint32_t top, uint32_t bottom)
{
	volatile uint32_t *fb = panel_map();

	if (fb == NULL) {
		return ENOMEM;
	}
	for (uint32_t y = 0; y < WHYRED_FB_HEIGHT; y++) {
		volatile uint32_t *line = fb + (uint64_t)y *
		    (WHYRED_FB_STRIDE / 4);
		uint32_t px = (y < WHYRED_FB_HEIGHT / 4) ? top : bottom;

		for (uint32_t x = 0; x < WHYRED_FB_WIDTH; x++) {
			line[x] = px;
		}
	}
	return 0;
}

/* Cheap visible state for the touch poll: a band across the top. */
static void
panel_band(uint32_t colour)
{
	volatile uint32_t *fb = panel_map();

	if (fb == NULL) {
		return;
	}
	for (uint32_t y = 0; y < 120; y++) {
		volatile uint32_t *line = fb + (uint64_t)y *
		    (WHYRED_FB_STRIDE / 4);

		for (uint32_t x = 0; x < WHYRED_FB_WIDTH; x++) {
			line[x] = colour;
		}
	}
}

static int
panel_report(int acks, uint8_t first)
{
	return panel_fill_rows(panel_code(acks, first),
	    acks > 0 ? 0xff20e020 : 0xffe02020);
}

int
santoni_touch_probe(void)
{
	vm_offset_t south, north;
	uint32_t on = 0, pulse = 0, wait, halt;
	uint8_t id[3] = { 0, 0, 0 };
	uint8_t first_ack = 0;
	struct dsx_funcs dsx;
	int addr, acks = 0, pulsed, saw_rmi = 0;
	uint32_t touch_s;
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

		/* whyred's DT: synaptics,reset-active-ms = 20,
		 * synaptics,reset-delay-ms = 200; the controller boots its
		 * firmware during that delay, so wait before any read. */
		pin_setup(&rst, north, WHYRED_RST);
		pin_out(&rst, 0);
		delay(20000);
		pin_out(&rst, 1);
		delay(250000);
		printf("WHYRED TOUCH: reset pulse low 20 ms, high 200 ms + "
		    "50 ms settle\n");
	}

	for (addr = 0x08; addr <= 0x77; addr++) {
		if (!i2c_ack((uint8_t)addr)) {
			continue;
		}
		acks++;
		if (acks == 1) {
			first_ack = (uint8_t)addr;
		}
		printf("WHYRED TOUCH: ack 0x%02x\n", addr);
		if (addr == DSX_ADDR) {
			/* Synaptics DSX: the Novatek page protocol would only
			 * return noise, the DSX page-table dump below is the
			 * real identification. */
			saw_rmi = 1;
			continue;
		}
		if (nvt_read((uint8_t)addr, 0x00, 0x00, id, sizeof(id)) == 0) {
			printf("WHYRED TOUCH: id 0x%02x %02x %02x %02x\n", addr,
			    id[0], id[1], id[2]);
		} else {
			printf("WHYRED TOUCH: id 0x%02x read failed\n", addr);
		}
	}

	printf("WHYRED TOUCH: scan done, %d address(es) acknowledged; sda %d "
	    "scl %d\n", acks, pin_read(&touch_sda), pin_read(&touch_scl));

	if (saw_rmi) {
		touch_irq_setup(north);
		dsx_diag();
		dsx_identify(&dsx);
		touch_s = 0;
		if (PE_parse_boot_argn("sweetouchtouch", &touch_s,
		    sizeof(touch_s)) && touch_s) {
			dsx_touch_test(&dsx, touch_s);
		}
	}

	/*
	 * Hold the panel so the answer is readable without a serial cable:
	 * the first iOS frame clears the console, and these lines print
	 * minutes before it. sweetouchwait=SECONDS overrides the 15 s hold,
	 * 0 continues immediately.
	 */
	/*
	 * sweetouchhalt=1: repeat the result forever instead of booting on.
	 * The panel is the only output on this phone, so a boot that keeps
	 * scrolling is easy to miss; a halted boot can be read at leisure and
	 * then powered off. Diagnostic images only.
	 */
	halt = 0;
	if (PE_parse_boot_argn("sweetouchhalt", &halt, sizeof(halt)) && halt) {
		if (panel_report(acks, first_ack) != 0) {
			printf("WHYRED TOUCH: panel paint failed\n");
		}
		for (;;) {
			printf("WHYRED TOUCH: RESULT acks=%d first=0x%02x sda=%d "
			    "scl=%d id=%02x%02x%02x\n", acks, first_ack,
			    pin_read(&touch_sda), pin_read(&touch_scl), id[0],
			    id[1], id[2]);
			delay(2000000);
		}
	}

	wait = 15;
	(void)PE_parse_boot_argn("sweetouchwait", &wait, sizeof(wait));
	if (wait) {
		if (wait > 60) {
			wait = 60;
		}
		printf("WHYRED TOUCH: holding the panel %u s\n", wait);
		delay((int)(wait * 1000000u));
	}
	return acks ? 0 : ENXIO;
}

#else /* !ARM64_BOARD_CONFIG_WHYRED */

int
santoni_touch_probe(void)
{
	return ENXIO;
}

#endif /* ARM64_BOARD_CONFIG_WHYRED */
