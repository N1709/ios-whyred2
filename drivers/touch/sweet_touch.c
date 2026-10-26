/*
 * SWEET SE7 FIFO I2C / GT9896 Yellowstone / Focaltech 54:52 bring-up.
 * topaz (SM6225): FocalTech FT5452 at 0x38 on QUP0 SE2, IRQ GPIO80, reset
 * GPIO86, AVDD enable GPIO36 (no regulator), values from topaz-woa
 * TopazTouch. Call once after the scheduler is running; all work runs on one
 * thread. Requires ABL-loaded GENI I2C firmware.
 * No firmware/config upload or direct PMIC writes. Only IC data write:
 * zero to GT9896's event status after reading a report. See touch2/report.md.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
/* Mach MIG declaration is not included by BSD's exported thread.h. */
extern kern_return_t thread_terminate(thread_t);
extern int sweet_rpmh_vreg_enable(const char *res, unsigned mv);
extern void sweet_hid_touch(int finger, int down, int x, int y);
void sweet_touch_init(void);

#define TOUCH_SE_PA      0x04a88000u /* QUP0 SE2 */
#define TOUCH_GCC_PA     0x01400000u
#define TOUCH_WEST_PA    0x00500000u /* TLMM 0x400000 + WEST tile: GPIO 6/7/80/86 */
#define TOUCH_EAST_PA    0x00d00000u /* TLMM 0x400000 + EAST tile: GPIO 36 */
#define TOUCH_GCC_VOTE   0x7900cu    /* QUP0 wrapper + SE2 vote, bits 0x13c0 */
#define TOUCH_RCG       0x1f3a8u    /* QUP0 S2 CMD; CFG +4, CBCR -4 (gcc-khaje.c) */
#define TOUCH_PIN_RESET  86u
#define TOUCH_PIN_IRQ    80u
#define TOUCH_PIN_AVDD   36u
#define TOUCH_GPIO_CFG(n) ((uint32_t)(n) * 0x1000u)
#define TOUCH_GPIO_IO(n)  (TOUCH_GPIO_CFG(n) + 4u)
#define TOUCH_MAX        10u
#define TOUCH_GOODIX     1
#define TOUCH_FTS        2
/* GT9896 is Yellowstone in Xiaomi sweet-r-oss, not the Berlin protocol. */
#define GOODIX_PID_REG   0x4022u
#define GOODIX_EVENT_REG 0x4180u
#define GOODIX_HEAD_LEN  8u
#define GOODIX_POINT_LEN 8u

#define SE_STATUS       0x040u
#define SE_MCLK         0x048u
#define SE_FW           0x068u
#define SE_CLK_SEL      0x07cu
#define SE_DMA          0x258u
#define SE_TX_LEN       0x26cu
#define SE_RX_LEN       0x270u
#define SE_SCL          0x278u
#define SE_CMD          0x600u
#define SE_CTRL         0x604u
#define SE_IRQ          0x610u
#define SE_IRQ_EN       0x614u
#define SE_IRQ_CLR      0x618u
#define SE_TX           0x700u
#define SE_RX           0x780u
#define SE_TX_COUNT     0x800u
#define SE_RX_COUNT     0x804u
#define SE_HW_TX        0xe24u
#define SE_HW_RX        0xe28u
#define SE_TX_WM        0x80cu
#define SE_RX_WM_IRQ    (1u << 26)
#define SE_RX_LAST      (1u << 27)
#define SE_TX_WM_IRQ    (1u << 30)
#define SE_DONE         (1u << 0)
#define SE_NACK         (1u << 10)
#define SE_ERRORS       (0x3eu | (1u << 9) | (1u << 10) | (1u << 12) | \
                         (1u << 13) | (3u << 24) | (3u << 28))
#define SE_EVENTS       (SE_DONE | SE_ERRORS | SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ)
/* geni_se_config_packing(8, 4, true, true, true): start bits 7/15/23/31,
 * MSB-to-LSB, length 8, STOP only on vector 3. FIFO bytes remain LSB first. */
#define SE_PACK0        0x0007f8feu
#define SE_PACK1        0x000ffefeu

static struct {
	vm_offset_t se, gcc, west, east;
	bool started, dead, probing;
	int kind, irq_hint;
	uint32_t irq_status, irq_last, active;
	unsigned sent, got;
	int x[TOUCH_MAX], y[TOUCH_MAX];
	/* sweettouchdiag: last step reached and the values seen on the way */
	int diag;
	const char *stage;
	uint32_t d_vote, d_cfg, d_cbcr, d_fw, d_status, d_hwtx, d_hwrx;
	int d_fe1, d_fe2, d_ge;
	uint8_t d_fid1, d_fid2;
	/* sweettouchdiag=2: polling statistics */
	unsigned s_frames, s_errs, s_down, s_up, s_maxn;
	int s_lasterr, s_lastx, s_lasty;
	uint8_t s_raw[12];
} st;
static int touch_sleep_chan;

static uint32_t
touch_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
touch_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
touch_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&touch_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static bool
touch_wait_clear(vm_offset_t base, uint32_t off, uint32_t mask)
{
	for (unsigned i = 0; i < 5000; ++i) {
		if (!(touch_rd(base, off) & mask)) {
			return true;
		}
		delay(1);
	}
	return false;
}

/* topaz: GPIO 36 is on the EAST tile, the touch pins otherwise on WEST. */
static vm_offset_t
touch_tile(unsigned pin)
{
	return pin == TOUCH_PIN_AVDD ? st.east : st.west;
}

static void
touch_gpio(unsigned pin, unsigned mux, unsigned pull, unsigned drive, bool out, bool high)
{
	vm_offset_t tile = touch_tile(pin);
	uint32_t off = TOUCH_GPIO_CFG(pin);
	uint32_t cfg = touch_rd(tile, off);
	if (out) {
		/* Set the desired latch before enabling output (reset is active low). */
		touch_wr(tile, TOUCH_GPIO_IO(pin),
		    (touch_rd(tile, TOUCH_GPIO_IO(pin)) & ~2u) | (high ? 2u : 0u));
	}
	cfg &= ~0x3ffu;
	cfg |= (mux << 2) | pull | (drive << 6) | (out ? 0x200u : 0u);
	touch_wr(tile, off, cfg);
}

static bool
touch_clocks(void)
{
	/*
	 * topaz (TopazTouch hwio.c): OR the QUP0 wrapper/SE2 votes into the
	 * shared vote register (other bits belong to other clients), XO parent
	 * with divider 1 = 19.2 MHz, no PLL or voltage change.
	 */
	touch_wr(st.gcc, TOUCH_GCC_VOTE, touch_rd(st.gcc, TOUCH_GCC_VOTE) | 0x13c0u);
	printf("SWEET TOUCH: gcc vote=%#x rcg=%#x cfg=%#x cbcr=%#x\n",
	    touch_rd(st.gcc, TOUCH_GCC_VOTE), touch_rd(st.gcc, TOUCH_RCG),
	    touch_rd(st.gcc, TOUCH_RCG + 4u), touch_rd(st.gcc, TOUCH_RCG - 4u));
	st.d_vote = touch_rd(st.gcc, TOUCH_GCC_VOTE);
	touch_wr(st.gcc, TOUCH_RCG + 4u, 1u);
	touch_wr(st.gcc, TOUCH_RCG, touch_rd(st.gcc, TOUCH_RCG) | 1u);
	if (!touch_wait_clear(st.gcc, TOUCH_RCG, 1u)) {
		printf("SWEET TOUCH: RCG update timeout\n");
		return false;
	}
	st.d_cfg = touch_rd(st.gcc, TOUCH_RCG + 4u);
	st.d_cbcr = touch_rd(st.gcc, TOUCH_RCG - 4u);
	if (!touch_wait_clear(st.gcc, TOUCH_RCG - 4u, 1u << 31)) {
		printf("SWEET TOUCH: SE2 branch halted (%#x)\n", touch_rd(st.gcc, TOUCH_RCG - 4u));
		return false;
	}
	return true;
}

/* Called only for a transfer started by this driver, including a stretched TX. */
static void
touch_abort(void)
{
	/* i2c-qcom-geni.c: M_GENI_CMD_CANCEL, then ABORT if cancel does not finish */
	bool done = false;
	touch_wr(st.se, SE_CTRL, 4);
	for (unsigned i = 0; i < 1000; ++i) {
		if (touch_rd(st.se, SE_IRQ) & (1u << 4)) {
			done = true;
			break;
		}
		delay(1);
	}
	if (!done) {
		touch_wr(st.se, SE_CTRL, 2); /* abort M command, release held bus */
		for (unsigned i = 0; i < 1000; ++i) {
			if (touch_rd(st.se, SE_IRQ) & (1u << 5)) {
				done = true;
				break;
			}
			delay(1);
		}
	}
	if (!done || !touch_wait_clear(st.se, SE_STATUS, 1u)) {
		st.dead = true;
		printf("SWEET TOUCH: abort failed status=%#x irq=%#x; stopping\n",
		    touch_rd(st.se, SE_STATUS), touch_rd(st.se, SE_IRQ));
	}
	/* Bound drain even if the hardware reports an impossible FIFO count. */
	for (unsigned i = 0; i < 64 && (touch_rd(st.se, SE_RX_COUNT) & 0x1ffffffu); ++i) {
		(void)touch_rd(st.se, SE_RX);
	}
	touch_wr(st.se, SE_IRQ_CLR, 0xffffffffu);
	if (touch_rd(st.se, SE_TX_COUNT) & 0xfffffffu) {
		st.dead = true;
		printf("SWEET TOUCH: TX FIFO not empty after abort; stopping\n");
	}
}

/* Keep all observed bits for diagnostics, even if a watermark was cleared. */
static uint32_t
touch_irq(void)
{
	st.irq_last = touch_rd(st.se, SE_IRQ);
	st.irq_status |= st.irq_last;
	return st.irq_last;
}

/* One Linux-style FIFO message. TX is at most 3 bytes; RX at most 90. */
static int
touch_msg_run(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx,
    unsigned nr, bool stretch)
{
	if (st.dead || nt > 3 || nr > 90 || (nt && nr) || (!nt && !nr)) {
		return EIO;
	}
	if (!touch_wait_clear(st.se, SE_STATUS, 1u)) {
		st.dead = true; /* no other SE7 owner is supported */
		return EBUSY;
	}
	touch_wr(st.se, SE_IRQ_CLR, 0xffffffffu);
	touch_wr(st.se, SE_TX_WM, nt ? 1u : 0u);
	touch_wr(st.se, nr ? SE_RX_LEN : SE_TX_LEN, nr ? nr : nt);
	touch_wr(st.se, SE_CMD, ((nr ? 2u : 1u) << 27) |
	    ((uint32_t)addr << 9) | (stretch ? 4u : 0u));
	if (nr > 8 && !st.probing) {
		touch_wait(2); /* sleep, do not spin, through most of the transfer */
	}
	int err = ETIMEDOUT;
	for (unsigned i = 0; i < 5000; ++i) {
		uint32_t irq = touch_irq();
		if (irq & SE_ERRORS) {
			err = (irq & SE_NACK) ? ENXIO : EIO;
			/* A completed NACK is normal during address probing. Aborting an
			 * idle sequencer may never produce ABORT_DONE. Wait for completion
			 * and leave a clean idle engine available for the other IC. */
			for (unsigned j = 0; j < 5000 && !(irq & SE_DONE); ++j) {
				delay(1);
				irq = touch_irq();
			}
			if ((irq & SE_DONE) && !(touch_rd(st.se, SE_STATUS) & 1u) &&
			    !(touch_rd(st.se, SE_TX_COUNT) & 0xfffffffu) &&
			    !(touch_rd(st.se, SE_RX_COUNT) & 0x1ffffffu)) {
				touch_wr(st.se, SE_IRQ_CLR, irq);
				return err;
			}
			break;
		}
		if (nt && !st.sent && (irq & SE_TX_WM_IRQ)) {
			uint32_t word = 0;
			for (unsigned j = 0; j < nt; ++j) {
				word |= (uint32_t)tx[j] << (8u * j);
			}
			touch_wr(st.se, SE_TX, word);
			st.sent = nt;
			touch_wr(st.se, SE_TX_WM, 0);
		}
		uint32_t count = touch_rd(st.se, SE_RX_COUNT) & 0x1ffffffu;
		if (count > 64 || count > (nr - st.got + 3u) / 4u) {
			err = EIO;
			break;
		}
		while (count--) {
			uint32_t word = touch_rd(st.se, SE_RX);
			for (unsigned j = 0; j < 4 && st.got < nr; ++j) {
				rx[st.got++] = (uint8_t)(word >> (8u * j));
			}
		}
		/* DONE is checked after draining RX, so final short words aren't lost. */
		if (irq & SE_DONE) {
			/* Re-sample after the final drain; a late error must beat DONE. */
			irq = touch_irq();
			if (irq & SE_ERRORS) {
				continue;
			}
			touch_wr(st.se, SE_IRQ_CLR, irq);
			if (st.got == nr && st.sent == nt) {
				return 0;
			}
			err = EIO;
			break;
		}
		/* Preserve DONE/errors if they arrive during this iteration. */
		touch_wr(st.se, SE_IRQ_CLR, irq & (SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ));
		delay(1);
	}
	touch_abort();
	return err;
}

static int
touch_msg(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx,
    unsigned nr, bool stretch)
{
	st.irq_status = st.irq_last = 0;
	st.sent = st.got = 0;
	int err = touch_msg_run(addr, tx, nt, rx, nr, stretch);
	if (st.probing) {
		printf("SWEET TOUCH: probe xfer addr=%#x %s len=%u stretch=%u err=%d irq=%#x seen=%#x tx=%u rx=%u status=%#x\n",
		    addr, nr ? "read" : "write", nr ? nr : nt, stretch ? 1u : 0u,
		    err, st.irq_last, st.irq_status, st.sent, st.got,
		    touch_rd(st.se, SE_STATUS));
	}
	return err;
}

static int
touch_read(uint8_t addr, uint16_t reg, bool wide, uint8_t *buf, unsigned len)
{
	uint8_t ptr[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
	int err = touch_msg(addr, wide ? ptr : ptr + 1, wide ? 2u : 1u,
	    NULL, 0, true);
	if (err) {
		return err;
	}
	/* STOP_STRETCH on the pointer write retains the bus for a repeated START. */
	return touch_msg(addr, NULL, 0, buf, len, false);
}

static int
touch_goodix_ack(void)
{
	const uint8_t ack[3] = {GOODIX_EVENT_REG >> 8, GOODIX_EVENT_REG & 0xffu, 0};
	return touch_msg(0x5d, ack, sizeof(ack), NULL, 0, false);
}

/* Raw read only: 0x8240 is the vendor's generic/older VERSION candidate,
 * not a second accepted GT9896 identity. Never try a four-byte Berlin pointer:
 * on this 16-bit IC the extra bytes would be register data writes. */
static int
touch_probe_info(uint16_t reg, uint8_t *buf)
{
	memset(buf, 0xa5, 32);
	int err = touch_read(0x5d, reg, true, buf, 32);
	printf("SWEET TOUCH: probe info reg=%#x len=32 err=%d irq=%#x seen=%#x\n",
	    reg, err, st.irq_last, st.irq_status);
	if (!err) {
		for (unsigned off = 0; off < 32; off += 16) {
			char line[49];
			for (unsigned j = 0; j < 16; ++j) {
				(void)snprintf(line + j * 3u, 4, "%02x ", buf[off + j]);
			}
			printf("SWEET TOUCH: probe raw %#x: %s\n", reg + off, line);
		}
	}
	return err;
}

static unsigned
touch_be16(const uint8_t *p)
{
	return ((unsigned)p[0] << 8) | p[1];
}

static bool
touch_checksum(const uint8_t *buf, unsigned len)
{
	if (len < 2) {
		return false;
	}
	unsigned sum = 0;
	for (unsigned i = 0; i + 2 < len; ++i) {
		sum += buf[i];
	}
	return (sum & 0xffffu) == touch_be16(buf + len - 2);
}

static void
touch_report(uint32_t mask, const int *x, const int *y)
{
	for (unsigned id = 0; id < TOUCH_MAX; ++id) {
		uint32_t bit = 1u << id;
		if (mask & bit) {
			if (!(st.active & bit) || x[id] != st.x[id] || y[id] != st.y[id]) {
				sweet_hid_touch((int)id, 1, x[id], y[id]);
				st.s_down++;
				st.s_lastx = x[id];
				st.s_lasty = y[id];
			}
			st.x[id] = x[id];
			st.y[id] = y[id];
		} else if (st.active & bit) {
			sweet_hid_touch((int)id, 0, st.x[id], st.y[id]);
			st.s_up++;
		}
	}
	st.active = mask;
}

static int
touch_goodix(void)
{
	uint8_t buf[90];
	int x[TOUCH_MAX] = {0}, y[TOUCH_MAX] = {0};
	uint32_t mask = 0;
	int err = touch_read(0x5d, GOODIX_EVENT_REG, true, buf, GOODIX_HEAD_LEN);
	if (err) {
		return err;
	}
	if (!(buf[0] & 0x80u)) {
		/* Do not acknowledge firmware/config requests that we cannot service. */
		if (buf[0] & 0x40u) {
			return ENOTSUP;
		}
		return 0; /* no new touch frame: retain current contacts */
	}
	if (!touch_checksum(buf, GOODIX_HEAD_LEN)) {
		return EIO; /* header untrusted: no write to the IC */
	}
	unsigned n = buf[2] & 15u;
	if (n > TOUCH_MAX) {
		(void)touch_goodix_ack();
		return EIO;
	}
	if (n) {
		err = touch_read(0x5d, GOODIX_EVENT_REG + GOODIX_HEAD_LEN, true,
		    buf + GOODIX_HEAD_LEN, n * GOODIX_POINT_LEN + 2u);
		if (err) {
			return err;
		}
		if (!touch_checksum(buf + GOODIX_HEAD_LEN, n * GOODIX_POINT_LEN + 2u)) {
			(void)touch_goodix_ack();
			return EIO;
		}
	}
	for (unsigned i = 0; i < n; ++i) {
		const uint8_t *p = buf + GOODIX_HEAD_LEN + i * GOODIX_POINT_LEN;
		unsigned id = p[0] >> 4;
		unsigned px = touch_be16(p + 2), py = touch_be16(p + 4);
		/* Low nibble 1/3 denotes pen data in the vendor parser. */
		if ((p[0] & 15u) == 1u || (p[0] & 15u) == 3u || id >= TOUCH_MAX ||
		    (mask & (1u << id)) || px > 1079 || py > 2399) {
			(void)touch_goodix_ack();
			return EIO;
		}
		mask |= 1u << id;
		x[id] = (int)px;
		y[id] = (int)py;
	}
	err = touch_goodix_ack();
	if (!err) {
		touch_report(mask, x, y);
	}
	return err;
}

static int
touch_focaltech(void)
{
	uint8_t buf[63];
	int x[TOUCH_MAX] = {0}, y[TOUCH_MAX] = {0};
	uint32_t mask = 0, seen = 0;
	int err = touch_read(0x38, 0, false, buf, sizeof(buf));
	if (err) {
		return err;
	}
	unsigned n = buf[2] & 15u, active = 0;
	if (n || !st.s_frames) {
		memcpy(st.s_raw, buf, sizeof(st.s_raw)); /* keep the last frame with contacts */
	}
	st.s_frames++;
	if (n > st.s_maxn) {
		st.s_maxn = n;
	}
	if (n > TOUCH_MAX && buf[3] == 0xffu) {
		/*
		 * topaz: with no contact the FT5452 (firmware as updated by HyperOS
		 * 2.0.208) reports TD_STATUS 0xff and 0xff records. TopazTouch: a count
		 * of 0 or > 10 means "release all", not an error.
		 */
		touch_report(0, x, y);
		return 0;
	}
	if (n > TOUCH_MAX) {
		/* topaz: show what the IC actually sends (rate-limited) */
		static uint64_t last_dump;
		uint64_t now = mach_absolute_time(), gap;
		nanoseconds_to_absolutetime(NSEC_PER_SEC, &gap);
		if (now - last_dump > gap) {
			last_dump = now;
			printf("SWEET TOUCH: bad frame n=%u raw %02x %02x %02x | %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x\n",
			    n, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8],
			    buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);
		}
		return EIO;
	}
	for (unsigned i = 0; i < TOUCH_MAX; ++i) {
		const uint8_t *p = buf + 3u + 6u * i;
		unsigned id = p[2] >> 4, event = p[0] >> 6;
		if (id >= TOUCH_MAX) {
			break; /* vendor's unused-record sentinel */
		}
		if (seen & (1u << id)) {
			return EIO;
		}
		seen |= 1u << id;
		if (event == 1u) {
			continue; /* UP record */
		}
		if (event == 3u) {
			continue; /* topaz: "no event" slot (TopazTouch) */
		}
		unsigned px = ((unsigned)(p[0] & 15u) << 8) | p[1];
		unsigned py = ((unsigned)(p[2] & 15u) << 8) | p[3];
		/* topaz: raw 0..1349 x 0..2999 (1.25x the panel) + 2-bit fractions */
		if (px > 1349 || py > 2999) {
			return EIO;
		}
		px = (px * 4u + (p[4] >> 6)) / 5u;
		py = (py * 4u + ((p[4] >> 4) & 3u)) / 5u;
		px = px > 1079 ? 1079 : px;
		py = py > 2399 ? 2399 : py;
		mask |= 1u << id;
		x[id] = (int)px;
		y[id] = (int)py;
		++active;
	}
	(void)active;   /* topaz: byte 2 does not always match the records */
	touch_report(mask, x, y);
	return 0;
}

static bool
touch_se_init(void)
{
	uint32_t fw = touch_rd(st.se, SE_FW);
	uint32_t tx = touch_rd(st.se, SE_HW_TX), rx = touch_rd(st.se, SE_HW_RX);
	st.d_fw = fw;
	st.d_status = touch_rd(st.se, SE_STATUS);
	st.d_hwtx = tx;
	st.d_hwrx = rx;
	printf("SWEET TOUCH: se2 fw=%#x proto=%u status=%#x tx=%#x rx=%#x ios=%#x\n",
	    fw, (fw >> 8) & 0xffu, touch_rd(st.se, SE_STATUS), tx, rx,
	    touch_rd(st.se, 0x908));
	if (((fw >> 8) & 0xffu) != 3u || (touch_rd(st.se, 0x64) & 1u) ||
	    ((tx >> 24) & 0x3fu) != 32u || ((rx >> 24) & 0x3fu) != 32u ||
	    ((tx >> 16) & 0x3fu) < 2u || ((rx >> 16) & 0x3fu) < 2u) {
		printf("SWEET TOUCH: need ABL-loaded I2C firmware and 32-bit FIFOs; probes skipped\n");
		return false;
	}
	if (touch_rd(st.se, SE_STATUS) & 0x1001u) {
		printf("SWEET TOUCH: SE2 already busy; probes skipped\n");
		return false;
	}
	touch_wr(st.se, 0xe18, 0); /* no GSI events */
	touch_wr(st.se, 0xe1c, 0); /* mask external IRQ routing; poll local status */
	touch_wr(st.se, 0x644, 0);
	touch_wr(st.se, SE_IRQ_CLR, 0xffffffffu);
	touch_wr(st.se, 0x648, 0xffffffffu);
	touch_wr(st.se, SE_DMA, 0);
	touch_wr(st.se, 0x28, touch_rd(st.se, 0x28) | 0x7fu);
	touch_wr(st.se, 0xe30, touch_rd(st.se, 0xe30) | 0x0fu);
	touch_wr(st.se, 0x24, 0x7f);
	touch_wr(st.se, 0x20, 1);
	touch_wr(st.se, SE_CLK_SEL, 0);
	touch_wr(st.se, SE_MCLK, (2u << 4) | 1u);
	touch_wr(st.se, SE_SCL, (5u << 20) | (11u << 10) | 22u); /* 400 kHz */
	/* Linux 8-bit I2C packing: MSB on the wire, LSB byte first in FIFO. */
	touch_wr(st.se, 0x254, 0);
	touch_wr(st.se, 0x260, SE_PACK0);
	touch_wr(st.se, 0x264, SE_PACK1);
	touch_wr(st.se, 0x284, SE_PACK0);
	touch_wr(st.se, 0x288, SE_PACK1);
	touch_wr(st.se, SE_TX_WM, 0);
	touch_wr(st.se, 0x810, 0);
	touch_wr(st.se, 0x814, ((rx >> 16) & 0x3fu) - 1u);
	touch_wr(st.se, SE_IRQ_EN, SE_EVENTS);
	printf("SWEET TOUCH: packing tx=%#x/%#x rx=%#x/%#x gran=%#x\n",
	    touch_rd(st.se, 0x260), touch_rd(st.se, 0x264),
	    touch_rd(st.se, 0x284), touch_rd(st.se, 0x288), touch_rd(st.se, 0x254));
	if (touch_rd(st.se, 0x260) != SE_PACK0 || touch_rd(st.se, 0x264) != SE_PACK1 ||
	    touch_rd(st.se, 0x284) != SE_PACK0 || touch_rd(st.se, 0x288) != SE_PACK1 ||
	    touch_rd(st.se, 0x254) != 0) {
		printf("SWEET TOUCH: packing readback mismatch; probes skipped\n");
		return false;
	}
	if ((touch_rd(st.se, SE_TX_COUNT) & 0xfffffffu) ||
	    (touch_rd(st.se, SE_RX_COUNT) & 0x1ffffffu)) {
		printf("SWEET TOUCH: inherited FIFO data; probes skipped\n");
		return false;
	}
	return true;
}

static void
touch_thread(void *arg, wait_result_t result)
{
	unsigned delay_s = 45;
	(void)arg;
	(void)result;
	/*
	 * Start late (boot-arg sweettouchdelay, seconds): if the bus access
	 * below hangs the SoC, the persistent log and the GUI got there first.
	 */
	(void)PE_parse_boot_argn("sweettouchdelay", &delay_s, sizeof(delay_s));
	printf("SWEET TOUCH: starting in %u s\n", delay_s);
	touch_wait(delay_s * 1000u);
	st.stage = "map";
	printf("SWEET TOUCH: mapping GCC/TLMM/SE2\n");
	st.gcc = ml_io_map(TOUCH_GCC_PA, 0x80000);
	st.west = ml_io_map(TOUCH_WEST_PA, 0x58000);
	st.east = ml_io_map(TOUCH_EAST_PA, 0x25000);
	st.se = ml_io_map(TOUCH_SE_PA, 0x4000);
	if (!st.gcc || !st.west || !st.east || !st.se) {
		printf("SWEET TOUCH: MMIO mapping failed\n");
		goto out;
	}
	st.stage = "clocks";
	if (!touch_clocks()) {
		goto out;
	}
	st.stage = "se2 init";
	if (!touch_se_init()) {
		goto out;
	}
	st.stage = "power/reset";
	printf("SWEET TOUCH: TLMM reset=%#x irq=%#x avdd=%#x\n",
	    touch_rd(st.west, TOUCH_GPIO_IO(TOUCH_PIN_RESET)),
	    touch_rd(st.west, TOUCH_GPIO_IO(TOUCH_PIN_IRQ)),
	    touch_rd(st.east, TOUCH_GPIO_IO(TOUCH_PIN_AVDD)));
	/* TopazTouch touch.c: IRQ input, AVDD on, I2C pins, then the reset pulse. */
	touch_gpio(TOUCH_PIN_IRQ, 0, 3, 3, false, false);       /* CTL 0xc3 */
	touch_gpio(TOUCH_PIN_AVDD, 0, 3, 3, true, true);        /* CTL 0x2c3, high */
	touch_gpio(6, 1, 0, 0, false, false);                   /* qup2 SDA, CTL 0x4 */
	touch_gpio(7, 1, 0, 0, false, false);                   /* qup2 SCL */
	touch_gpio(TOUCH_PIN_RESET, 0, 3, 3, true, false);      /* CTL 0x2c3, low */
	touch_wait(20);
	touch_gpio(TOUCH_PIN_RESET, 0, 3, 3, true, true);
	touch_wait(250);
	st.stage = "probe";
	uint8_t gid[32], legacy[32], fid1 = 0, fid2 = 0;
	st.probing = true;
	int ge = touch_probe_info(GOODIX_PID_REG, gid);
	printf("SWEET TOUCH: probe 0x5d err=%d pid=%02x%02x%02x%02x fw=%02x.%02x.%02x.%02x irq=%#x\n",
	    ge, gid[0], gid[1], gid[2], gid[3], gid[8], gid[9], gid[10], gid[11], st.irq_status);
	(void)touch_probe_info(0x8240, legacy);
	int fe1 = touch_read(0x38, 0xa3, false, &fid1, 1);
	int fe2 = touch_read(0x38, 0x9f, false, &fid2, 1);
	printf("SWEET TOUCH: probe 0x38 err=%d/%d chip-id=%02x:%02x irq=%#x\n",
	    fe1, fe2, fid1, fid2, st.irq_status);
	{
		uint8_t fw = 0, vendor = 0, mode = 0, pmode = 0, lib[2] = {0, 0};
		int e1 = touch_read(0x38, 0xa6, false, &fw, 1);
		int e2 = touch_read(0x38, 0xa8, false, &vendor, 1);
		int e3 = touch_read(0x38, 0x00, false, &mode, 1);
		int e4 = touch_read(0x38, 0xa5, false, &pmode, 1);
		int e5 = touch_read(0x38, 0xa1, false, lib, 2);
		printf("SWEET TOUCH: ft fw=%02x/%d vendor=%02x/%d devmode=%02x/%d pmode=%02x/%d lib=%02x%02x/%d\n",
		    fw, e1, vendor, e2, mode, e3, pmode, e4, lib[0], lib[1], e5);
	}
	st.probing = false;
	st.d_ge = ge;
	st.d_fe1 = fe1;
	st.d_fe2 = fe2;
	st.d_fid1 = fid1;
	st.d_fid2 = fid2;
	bool goodix = !ge && gid[0] == '9' && gid[1] == '8' && gid[2] == '9' && gid[3] == '6';
	bool fts = !fe1 && !fe2 && fid1 == 0x54 && fid2 == 0x52;
	if (st.dead || goodix == fts) {
		printf("SWEET TOUCH: no unique supported IC (goodix=%d fts=%d dead=%d); stopping\n",
		    goodix, fts, st.dead);
		goto out;
	}
	st.kind = goodix ? TOUCH_GOODIX : TOUCH_FTS;
	st.stage = "polling";
	printf("SWEET TOUCH: selected %s, 1080x2400, poll=10ms irq-hint=%d\n",
	    goodix ? "GT9896 Yellowstone" : "Focaltech 54:52", st.irq_hint);
	unsigned errors = 0, ticks = 0, polls = 0;
	while (!st.dead) {
		if (st.diag == 2 && ++polls == 3000) {  /* ~30 s at 10 ms */
			panic("SWEET TOUCH stats: frames=%u errs=%u lasterr=%d maxn=%u hid down=%u up=%u last=%d,%d | "
			    "raw %02x %02x %02x | %02x %02x %02x %02x %02x %02x | %02x %02x %02x",
			    st.s_frames, st.s_errs, st.s_lasterr, st.s_maxn, st.s_down, st.s_up,
			    st.s_lastx, st.s_lasty, st.s_raw[0], st.s_raw[1], st.s_raw[2],
			    st.s_raw[3], st.s_raw[4], st.s_raw[5], st.s_raw[6], st.s_raw[7],
			    st.s_raw[8], st.s_raw[9], st.s_raw[10], st.s_raw[11]);
		}
		/* Optional IRQ hint still reads every 100ms, and always while down. */
		if (!st.irq_hint || st.active ||
		    !(touch_rd(st.west, TOUCH_GPIO_IO(TOUCH_PIN_IRQ)) & 1u) ||
		    (++ticks % 10u) == 0) {
			int err = st.kind == TOUCH_GOODIX ? touch_goodix() : touch_focaltech();
			if (err) {
				st.s_errs++;
				st.s_lasterr = err;
				if ((errors++ % 100u) == 0) {
					printf("SWEET TOUCH: report err=%d irq=%#x\n", err, st.irq_status);
				}
				if (errors == 10) {
					touch_report(0, st.x, st.y); /* release after persistent bad reads */
				}
			} else {
				errors = 0;
			}
		}
		touch_wait(10);
	}
	touch_report(0, st.x, st.y);
out:
	if (st.diag) {
		panic("SWEET TOUCH diag: stopped at '%s' dead=%d | gcc vote=%#x cfg=%#x cbcr=%#x | "
		    "se2 fw=%#x status=%#x hwtx=%#x hwrx=%#x | goodix err=%d | ft 0x38 err=%d/%d id=%02x:%02x | irq=%#x",
		    st.stage ? st.stage : "?", st.dead, st.d_vote, st.d_cfg, st.d_cbcr,
		    st.d_fw, st.d_status, st.d_hwtx, st.d_hwrx, st.d_ge,
		    st.d_fe1, st.d_fe2, st.d_fid1, st.d_fid2, st.irq_status);
	}
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
sweet_touch_init(void)
{
	int enabled = 1;
	thread_t thread;
	if (st.started) {
		return;
	}
	st.started = true;
	(void)PE_parse_boot_argn("sweettouch", &enabled, sizeof(enabled));
	st.irq_hint = 1; /* topaz: read on IRQ GPIO80 low, while touching, else every 100 ms */
	(void)PE_parse_boot_argn("sweettouchirq", &st.irq_hint, sizeof(st.irq_hint));
	(void)PE_parse_boot_argn("sweettouchdiag", &st.diag, sizeof(st.diag));
	if (!enabled) {
		printf("SWEET TOUCH: disabled (sweettouch=0)\n");
		return;
	}
	kern_return_t kr = kernel_thread_start(touch_thread, NULL, &thread);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("SWEET TOUCH: kernel_thread_start failed %d\n", kr);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
