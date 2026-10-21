/*
 * Host unit test for the Qualcomm SDHCI MSM v4 (SDCC1) block driver.
 *
 * santoni's internal eMMC 5.1 hangs off SDCC1. The controller exposes the
 * standard SDHCI register file (HC) at 0x07824900 (0x500 bytes) and the
 * Qualcomm vendor core registers (CORE) at 0x07824000 (0x800 bytes); both fit
 * in one 0x1000 page window, HC at CORE + 0x900 (msm8937.dtsi sdhc_1).
 *
 * The routines below are the same command framing / PIO code that
 * bsd/dev/santoni_sdhci.c runs on hardware, exercised against a mock
 * controller that models the register file, the command handshake
 * (PRESENT_STATE inhibit -> COMMAND write -> INT_STATUS completion), the
 * buffer-data-ready handshake and the 512-byte PIO FIFO.
 *
 * Build and run on the host:
 *   clang -O2 -Wall -I. tests/test_sdhci_mock.c -o out/test_sdhci_mock
 *   ./out/test_sdhci_mock
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------- addresses */

/* Physical MMIO windows of SDCC1 (resources/linux-msm89x7-7.1.3
 * arch/arm64/boot/dts/qcom/msm8937.dtsi, sdhc_1). */
#define SDHCI_CORE_PHYS  0x07824000u
#define SDHCI_HC_PHYS    0x07824900u
#define SDHCI_CORE_SIZE  0x1000u
#define SDHCI_HC_OFFSET  (SDHCI_HC_PHYS - SDHCI_CORE_PHYS)  /* 0x900 */
#define SDHCI_HC_SIZE    0x500u

/* ------------------------------------------------- SDHCI standard registers */

#define SDHCI_BLOCK_SIZE       0x04    /* 16-bit */
#define SDHCI_BLOCK_COUNT      0x06    /* 16-bit */
#define SDHCI_ARGUMENT         0x08
#define SDHCI_TRANSFER_MODE    0x0C    /* 16-bit */
#define SDHCI_COMMAND          0x0E    /* 16-bit */
#define SDHCI_RESPONSE         0x10    /* 0x10..0x1C */
#define SDHCI_BUFFER           0x20
#define SDHCI_PRESENT_STATE    0x24
#define SDHCI_HOST_CONTROL     0x28    /* 8-bit */
#define SDHCI_POWER_CONTROL    0x29    /* 8-bit, never written here */
#define SDHCI_CLOCK_CONTROL    0x2C    /* 16-bit, never written here */
#define SDHCI_SOFTWARE_RESET   0x2F    /* 8-bit */
#define SDHCI_INT_STATUS       0x30
#define SDHCI_INT_ENABLE       0x34
#define SDHCI_SIGNAL_ENABLE    0x38

#define SDHCI_MAKE_BLKSZ(dma, blksz) (((dma) << 12) | ((blksz) & 0xfff))

/* TRANSFER_MODE bits */
#define SDHCI_TRNS_DMA         0x0001
#define SDHCI_TRNS_BLK_CNT_EN  0x0002
#define SDHCI_TRNS_AUTO_CMD12  0x0004
#define SDHCI_TRNS_READ        0x0010
#define SDHCI_TRNS_MULTI       0x0020

/* COMMAND bits */
#define SDHCI_CMD_RESP_NONE        0x00
#define SDHCI_CMD_RESP_LONG        0x01
#define SDHCI_CMD_RESP_SHORT       0x02
#define SDHCI_CMD_RESP_SHORT_BUSY  0x03
#define SDHCI_CMD_CRC              0x08
#define SDHCI_CMD_INDEX            0x10
#define SDHCI_CMD_DATA             0x20
#define SDHCI_MAKE_CMD(c, f)       ((((c) & 0xff) << 8) | ((f) & 0xff))

/* PRESENT_STATE bits */
#define SDHCI_CMD_INHIBIT      0x00000001u
#define SDHCI_DATA_INHIBIT     0x00000002u
#define SDHCI_DATA_AVAILABLE   0x00000800u
#define SDHCI_SPACE_AVAILABLE  0x00000400u

/* HOST_CONTROL: PIO needs the DMA select field cleared, bus width preserved */
#define SDHCI_CTRL_DMA_MASK    0x18u
#define SDHCI_CTRL_8BITBUS     0x20u

/* SOFTWARE_RESET */
#define SDHCI_RESET_ALL        0x01
#define SDHCI_RESET_CMD        0x02
#define SDHCI_RESET_DATA       0x04

/* INT_STATUS bits */
#define SDHCI_INT_CC           0x00000001u  /* command complete */
#define SDHCI_INT_TC           0x00000002u  /* transfer complete */
#define SDHCI_INT_DMA_END      0x00000008u
#define SDHCI_INT_BWR          0x00000010u  /* buffer write ready */
#define SDHCI_INT_BRR          0x00000020u  /* buffer read ready */
#define SDHCI_INT_ERR          0x00008000u

/* ------------------------------------------ Qualcomm MSM v4 core registers */

#define CORE_HC_MODE           0x78
#define CORE_HC_MODE_EN        0x00000001u
#define CORE_FF_CLK_SW_RST_DIS 0x00002000u
#define CORE_PWRCTL_STATUS     0xdc
#define CORE_PWRCTL_MASK       0xe0

/* --------------------------------------------------------- card / protocol */

#define SDHCI_BLOCK      512u
#define SDHCI_BOUNCE     (1u << 20)
#define SDHCI_MAX_BLOCKS (SDHCI_BOUNCE / SDHCI_BLOCK)

#define MOCK_CARD_BLOCKS 16u
#define CMD_TIMEOUT_US   50000u
#define CMD_INHIBIT_US   50000u

/* -------------------------------------------------------------- host timing */

static uint64_t
host_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The kernel uses delay(us); a short sleep keeps this test from spinning. */
static void
host_delay_us(unsigned us)
{
	struct timespec ts = { 0, (long)us * 1000 };

	nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------ mock hardware */

static uint8_t mock_hc[SDHCI_CORE_SIZE];
static uint8_t mock_core[SDHCI_CORE_SIZE];

static struct mock {
	uint32_t int_status;
	uint32_t present_state;
	int inhibit_polls;
	int stall_cmd;
	int fail_cmd;
	int fail_data;
	unsigned state_reads;
	unsigned clock_writes;
	unsigned reset_writes;
	uint16_t reset_value;
	uint16_t blk_size;
	uint16_t blk_count;
	uint16_t tr_mode;
	uint32_t argument;
	unsigned cmd_count;
	struct {
		uint16_t cmd;
		uint16_t word;
		uint32_t arg;
		uint16_t tr_mode;
	} trace[32];
	/* data phase */
	int data_active;
	int data_read;          /* 1 = card -> host */
	uint32_t cur_lba;
	uint32_t blocks_left;
	uint32_t word;
	uint8_t block[SDHCI_BLOCK];
} m;

static uint8_t
mock_card[MOCK_CARD_BLOCKS * SDHCI_BLOCK];

static uint16_t
mock_rd16(uint32_t off)
{
	return (uint16_t)(mock_hc[off] | (mock_hc[off + 1] << 8));
}

static void
mock_wr16(uint32_t off, uint16_t val)
{
	mock_hc[off] = (uint8_t)val;
	mock_hc[off + 1] = (uint8_t)(val >> 8);
}

static uint32_t
mock_rd32(uint32_t off)
{
	return (uint32_t)mock_hc[off] | ((uint32_t)mock_hc[off + 1] << 8) |
	    ((uint32_t)mock_hc[off + 2] << 16) | ((uint32_t)mock_hc[off + 3] << 24);
}

static void
mock_wr32(uint32_t off, uint32_t val)
{
	mock_hc[off] = (uint8_t)val;
	mock_hc[off + 1] = (uint8_t)(val >> 8);
	mock_hc[off + 2] = (uint8_t)(val >> 16);
	mock_hc[off + 3] = (uint8_t)(val >> 24);
}

/* Load the current block of the fake card into the FIFO model. */
static void
mock_load_block(void)
{
	memcpy(m.block, mock_card + (uint64_t)m.cur_lba * SDHCI_BLOCK, SDHCI_BLOCK);
	m.word = 0;
	m.int_status |= m.data_read ? SDHCI_INT_BRR : SDHCI_INT_BWR;
}

static void
mock_start_data(uint32_t lba, uint16_t count, int read)
{
	m.cur_lba = lba;
	m.blocks_left = count;
	m.data_read = read;
	m.data_active = 1;
	if (read) {
		mock_load_block();
	} else {
		memset(m.block, 0, sizeof(m.block));
		m.word = 0;
		m.int_status |= SDHCI_INT_BWR;
	}
}

static uint32_t
mock_buf_read(void)
{
	uint32_t w;

	if (!m.data_active) {
		return 0xffffffffu;
	}
	if (m.fail_data) {
		m.int_status |= SDHCI_INT_ERR;
		m.data_active = 0;
		return 0xdeadbeefu;
	}
	w = (uint32_t)m.block[m.word * 4] | ((uint32_t)m.block[m.word * 4 + 1] << 8) |
	    ((uint32_t)m.block[m.word * 4 + 2] << 16) |
	    ((uint32_t)m.block[m.word * 4 + 3] << 24);
	m.word++;
	if (m.word == SDHCI_BLOCK / 4) {
		if (!m.data_read) {
			/* A write block is only committed once fully pushed. */
			memcpy(mock_card + (uint64_t)m.cur_lba * SDHCI_BLOCK, m.block,
			    SDHCI_BLOCK);
		}
		m.blocks_left--;
		m.cur_lba++;
		if (m.blocks_left) {
			if (m.data_read) {
				mock_load_block();
			} else {
				memset(m.block, 0, sizeof(m.block));
				m.word = 0;
				m.int_status |= SDHCI_INT_BWR;
			}
		} else {
			m.data_active = 0;
			m.int_status |= SDHCI_INT_TC;
		}
	}
	return w;
}

static void
mock_buf_write(uint32_t val)
{
	if (!m.data_active || m.data_read) {
		return;
	}
	m.block[m.word * 4] = (uint8_t)val;
	m.block[m.word * 4 + 1] = (uint8_t)(val >> 8);
	m.block[m.word * 4 + 2] = (uint8_t)(val >> 16);
	m.block[m.word * 4 + 3] = (uint8_t)(val >> 24);
	m.word++;
	if (m.word == SDHCI_BLOCK / 4) {
		memcpy(mock_card + (uint64_t)m.cur_lba * SDHCI_BLOCK, m.block,
		    SDHCI_BLOCK);
		m.blocks_left--;
		m.cur_lba++;
		if (m.blocks_left) {
			memset(m.block, 0, sizeof(m.block));
			m.word = 0;
			m.int_status |= SDHCI_INT_BWR;
		} else {
			m.data_active = 0;
			m.int_status |= SDHCI_INT_TC;
		}
	}
}

static void
mock_command(uint16_t val)
{
	uint8_t index = (uint8_t)(val >> 8);
	uint8_t flags = (uint8_t)val;

	if (m.cmd_count < sizeof(m.trace) / sizeof(m.trace[0])) {
		m.trace[m.cmd_count].cmd = index;
		m.trace[m.cmd_count].word = val;
		m.trace[m.cmd_count].arg = m.argument;
		m.trace[m.cmd_count].tr_mode = m.tr_mode;
	}
	m.cmd_count++;

	if (m.stall_cmd) {
		return;                 /* no completion: driver must time out */
	}
	if (m.fail_cmd) {
		m.int_status |= SDHCI_INT_ERR;
		return;
	}

	switch (index) {
	case 13:                        /* SEND_STATUS: R1, card in TRAN */
		mock_wr32(SDHCI_RESPONSE, 0x00000900u);   /* ready for data, state 4 */
		break;
	case 10:                        /* SEND_CID: 128-bit response */
		mock_wr32(SDHCI_RESPONSE + 0, 0x15010044u);
		mock_wr32(SDHCI_RESPONSE + 4, 0x33413831u);
		mock_wr32(SDHCI_RESPONSE + 8, 0x00000000u);
		mock_wr32(SDHCI_RESPONSE + 12, 0x00000000u);
		break;
	case 9:                         /* SEND_CSD: 128-bit response */
		mock_wr32(SDHCI_RESPONSE + 0, 0xd00f590au);
		mock_wr32(SDHCI_RESPONSE + 4, 0x0f5a83b9u);
		mock_wr32(SDHCI_RESPONSE + 8, 0x3b9aca00u);
		mock_wr32(SDHCI_RESPONSE + 12, 0x00000000u);
		break;
	case 17:                        /* READ_SINGLE_BLOCK */
		mock_start_data(m.argument, 1, 1);
		break;
	case 18:                        /* READ_MULTIPLE_BLOCK */
		mock_start_data(m.argument, m.blk_count, 1);
		break;
	case 24:                        /* WRITE_BLOCK */
		mock_start_data(m.argument, 1, 0);
		break;
	case 25:                        /* WRITE_MULTIPLE_BLOCK */
		mock_start_data(m.argument, m.blk_count, 0);
		break;
	case 12:                        /* STOP_TRANSMISSION */
		m.data_active = 0;
		break;
	default:
		break;
	}
	m.int_status |= SDHCI_INT_CC;
	(void)flags;
}

static uint32_t
mock_hc_read(uint32_t off)
{
	switch (off) {
	case SDHCI_PRESENT_STATE:
		m.state_reads++;
		if (m.inhibit_polls > 0) {
			m.inhibit_polls--;
			return SDHCI_CMD_INHIBIT;
		}
		return m.present_state;
	case SDHCI_INT_STATUS:
		return m.int_status;
	case SDHCI_BUFFER:
		return mock_buf_read();
	default:
		return mock_rd32(off);
	}
}

static void
mock_hc_write(uint32_t off, uint32_t val)
{
	switch (off) {
	case SDHCI_ARGUMENT:
		m.argument = val;
		mock_wr32(off, val);
		break;
	case SDHCI_TRANSFER_MODE:
		m.tr_mode = (uint16_t)val;
		mock_wr16(off, (uint16_t)val);
		break;
	case SDHCI_BLOCK_SIZE:
		m.blk_size = (uint16_t)val;
		mock_wr16(off, (uint16_t)val);
		break;
	case SDHCI_BLOCK_COUNT:
		m.blk_count = (uint16_t)val;
		mock_wr16(off, (uint16_t)val);
		break;
	case SDHCI_COMMAND:
		mock_wr16(off, (uint16_t)val);
		mock_command((uint16_t)val);
		break;
	case SDHCI_BUFFER:
		mock_buf_write(val);
		break;
	case SDHCI_INT_STATUS:
		m.int_status &= ~val;
		break;
	case SDHCI_SOFTWARE_RESET:
		m.reset_writes++;
		m.reset_value = (uint16_t)val;
		mock_hc[off] = 0;       /* reset self-clears when complete */
		break;
	case SDHCI_CLOCK_CONTROL:
		m.clock_writes++;       /* the driver must never write this */
		mock_wr16(off, (uint16_t)val);
		break;
	default:
		mock_wr32(off, val);
		break;
	}
}

/* ------------------------------------------- driver-side accessors (tested) */

static inline uint32_t
hc_rd(uint32_t off)
{
	return mock_hc_read(off);
}

static inline void
hc_wr(uint32_t off, uint32_t val)
{
	mock_hc_write(off, val);
}

static inline uint16_t
hc_rd16(uint32_t off)
{
	return mock_rd16(off);
}

static inline void
hc_wr16(uint32_t off, uint16_t val)
{
	mock_hc_write(off, val);
}

static inline uint8_t
hc_rd8(uint32_t off)
{
	return mock_hc[off];
}

static inline void
hc_wr8(uint32_t off, uint8_t val)
{
	mock_hc_write(off, val);
}

static inline uint32_t
core_rd(uint32_t off)
{
	uint32_t v = (uint32_t)mock_core[off] | ((uint32_t)mock_core[off + 1] << 8) |
	    ((uint32_t)mock_core[off + 2] << 16) | ((uint32_t)mock_core[off + 3] << 24);
	return v;
}

static inline void
core_wr(uint32_t off, uint32_t val)
{
	mock_core[off] = (uint8_t)val;
	mock_core[off + 1] = (uint8_t)(val >> 8);
	mock_core[off + 2] = (uint8_t)(val >> 16);
	mock_core[off + 3] = (uint8_t)(val >> 24);
}

/* ------------------------------------------------------------ driver logic */

/*
 * Take the controller over from the bootloader: verify the vendor core is in
 * HC mode with the feedback-clock software reset disabled, then reset only the
 * command and data paths. SDHCI_RESET_ALL (0x01) would wipe the core clocks
 * that LK programmed through GCC, and CLOCK_CONTROL (0x2C) is never written.
 */
static int
sdhci_take_over(void)
{
	uint32_t hc_mode = core_rd(CORE_HC_MODE);

	if (!(hc_mode & CORE_HC_MODE_EN)) {
		return ENXIO;
	}
	if (!(hc_mode & CORE_FF_CLK_SW_RST_DIS)) {
		core_wr(CORE_HC_MODE, hc_mode | CORE_FF_CLK_SW_RST_DIS);
	}

	hc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
	uint64_t deadline = host_now_ns() + 100000000ull;
	while (hc_rd8(SDHCI_SOFTWARE_RESET) & (SDHCI_RESET_CMD | SDHCI_RESET_DATA)) {
		if (host_now_ns() > deadline) {
			return ETIMEDOUT;
		}
		host_delay_us(5);
	}

	/* PIO: select the (unused) SDMA field and keep bus width / speed. */
	hc_wr8(SDHCI_HOST_CONTROL, (uint8_t)(hc_rd8(SDHCI_HOST_CONTROL) &
	    ~SDHCI_CTRL_DMA_MASK));

	/* Polled operation: no interrupt sources. */
	hc_wr(SDHCI_INT_ENABLE, 0);
	hc_wr(SDHCI_SIGNAL_ENABLE, 0);
	hc_wr(SDHCI_INT_STATUS, hc_rd(SDHCI_INT_STATUS));
	return 0;
}

/* Wait until PRESENT_STATE no longer reports the given inhibit bits. */
static int
sdhci_poll_state(uint32_t mask, uint64_t timeout_us)
{
	uint64_t deadline = host_now_ns() + timeout_us * 1000ull;

	while (hc_rd(SDHCI_PRESENT_STATE) & mask) {
		if (host_now_ns() > deadline) {
			return ETIMEDOUT;
		}
		host_delay_us(10);
	}
	return 0;
}

static int
sdhci_cmd(uint8_t index, uint32_t arg, uint16_t tr_mode, uint16_t flags,
    uint32_t *resp)
{
	int error = sdhci_poll_state(SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT,
	    CMD_INHIBIT_US);
	if (error) {
		return EBUSY;
	}

	hc_wr(SDHCI_ARGUMENT, arg);
	hc_wr16(SDHCI_TRANSFER_MODE, tr_mode);
	hc_wr16(SDHCI_COMMAND, (uint16_t)SDHCI_MAKE_CMD(index, flags));

	uint64_t deadline = host_now_ns() + CMD_TIMEOUT_US * 1000ull;
	for (;;) {
		uint32_t st = hc_rd(SDHCI_INT_STATUS);

		if (st & SDHCI_INT_ERR) {
			hc_wr(SDHCI_INT_STATUS, st);
			return EIO;
		}
		if (st & SDHCI_INT_CC) {
			hc_wr(SDHCI_INT_STATUS, SDHCI_INT_CC);
			if (resp) {
				*resp = hc_rd(SDHCI_RESPONSE);
			}
			return 0;
		}
		if (host_now_ns() > deadline) {
			return ETIMEDOUT;
		}
		host_delay_us(20);
	}
}

static int
sdhci_read_response128(uint8_t index, uint32_t out[4])
{
	int error = sdhci_cmd(index, 0, 0,
	    SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC | SDHCI_CMD_INDEX, NULL);
	if (error) {
		return error;
	}
	/* RESPONSE[0] is bits 31:0; callers want out[0] = bits 127:96. */
	for (int i = 0; i < 4; i++) {
		out[i] = hc_rd(SDHCI_RESPONSE + 4 * (3 - i));
	}
	return 0;
}

static int
sdhci_read_cid_csd(uint32_t cid[4], uint32_t csd[4])
{
	int error = sdhci_read_response128(10, cid);
	if (error) {
		return error;
	}
	return sdhci_read_response128(9, csd);
}

/* Poll one data interrupt (BRR for reads, BWR for writes) and clear it. */
static int
sdhci_wait_data(uint32_t bit, int *saw_error)
{
	uint64_t deadline = host_now_ns() + CMD_TIMEOUT_US * 1000ull;

	for (;;) {
		uint32_t st = hc_rd(SDHCI_INT_STATUS);

		if (st & SDHCI_INT_ERR) {
			hc_wr(SDHCI_INT_STATUS, st);
			*saw_error = 1;
			return EIO;
		}
		if (st & bit) {
			hc_wr(SDHCI_INT_STATUS, bit);
			return 0;
		}
		if (host_now_ns() > deadline) {
			return ETIMEDOUT;
		}
		host_delay_us(20);
	}
}

static int
sdhci_wait_transfer_complete(void)
{
	uint64_t deadline = host_now_ns() + CMD_TIMEOUT_US * 1000ull;

	for (;;) {
		uint32_t st = hc_rd(SDHCI_INT_STATUS);

		if (st & SDHCI_INT_ERR) {
			hc_wr(SDHCI_INT_STATUS, st);
			return EIO;
		}
		if (st & SDHCI_INT_TC) {
			hc_wr(SDHCI_INT_STATUS, SDHCI_INT_TC);
			return 0;
		}
		if (host_now_ns() > deadline) {
			return ETIMEDOUT;
		}
		host_delay_us(20);
	}
}

static int
sdhci_stop_transmission(void)
{
	return sdhci_cmd(12, 0, 0,
	    SDHCI_CMD_RESP_SHORT_BUSY | SDHCI_CMD_CRC | SDHCI_CMD_INDEX, NULL);
}

/* Reset the command and data paths, clear status, then best-effort CMD12. */
static int
sdhci_recover(void)
{
	hc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
	uint64_t deadline = host_now_ns() + CMD_INHIBIT_US * 1000ull;
	while (hc_rd8(SDHCI_SOFTWARE_RESET) & (SDHCI_RESET_CMD | SDHCI_RESET_DATA)) {
		if (host_now_ns() > deadline) {
			break;
		}
		host_delay_us(5);
	}
	hc_wr(SDHCI_INT_STATUS, hc_rd(SDHCI_INT_STATUS));
	(void)sdhci_stop_transmission();
	hc_wr(SDHCI_INT_STATUS, hc_rd(SDHCI_INT_STATUS));
	return 0;
}

static int
sdhci_read_blocks_pio(uint64_t lba, uint32_t count, void *buf)
{
	if (count == 0 || count > SDHCI_MAX_BLOCKS || lba > 0xffffffffull) {
		return EINVAL;
	}
	hc_wr16(SDHCI_BLOCK_SIZE, SDHCI_MAKE_BLKSZ(0, SDHCI_BLOCK));
	hc_wr16(SDHCI_BLOCK_COUNT, (uint16_t)count);

	int error = sdhci_cmd(18, (uint32_t)lba,
	    SDHCI_TRNS_MULTI | SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN,
	    SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX |
	    SDHCI_CMD_DATA, NULL);
	if (error) {
		sdhci_recover();
		return error;
	}

	uint32_t *dst = (uint32_t *)buf;
	for (uint32_t b = 0; b < count; b++) {
		int saw_error = 0;

		error = sdhci_wait_data(SDHCI_INT_BRR, &saw_error);
		if (error) {
			sdhci_recover();
			return error;
		}
		for (uint32_t i = 0; i < SDHCI_BLOCK / 4; i++) {
			dst[b * (SDHCI_BLOCK / 4) + i] = hc_rd(SDHCI_BUFFER);
		}
	}
	error = sdhci_wait_transfer_complete();
	if (error) {
		sdhci_recover();
		return error;
	}
	/* BLK_CNT_EN stops the card at the block count: no CMD12 on success. */
	return 0;
}

static int
sdhci_write_blocks_pio(uint64_t lba, uint32_t count, const void *buf)
{
	if (count == 0 || count > SDHCI_MAX_BLOCKS || lba > 0xffffffffull) {
		return EINVAL;
	}
	hc_wr16(SDHCI_BLOCK_SIZE, SDHCI_MAKE_BLKSZ(0, SDHCI_BLOCK));
	hc_wr16(SDHCI_BLOCK_COUNT, (uint16_t)count);

	int error = sdhci_cmd(25, (uint32_t)lba,
	    SDHCI_TRNS_MULTI | SDHCI_TRNS_BLK_CNT_EN,
	    SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX |
	    SDHCI_CMD_DATA, NULL);
	if (error) {
		sdhci_recover();
		return error;
	}

	const uint32_t *src = (const uint32_t *)buf;
	for (uint32_t b = 0; b < count; b++) {
		int saw_error = 0;

		error = sdhci_wait_data(SDHCI_INT_BWR, &saw_error);
		if (error) {
			sdhci_recover();
			return error;
		}
		for (uint32_t i = 0; i < SDHCI_BLOCK / 4; i++) {
			hc_wr(SDHCI_BUFFER, src[b * (SDHCI_BLOCK / 4) + i]);
		}
	}
	error = sdhci_wait_transfer_complete();
	if (error) {
		sdhci_recover();
		return error;
	}
	/* BLK_CNT_EN stops the card at the block count: no CMD12 on success. */
	return 0;
}

/* -------------------------------------------------------------- test suite */

static void
reset_mock(void)
{
	memset(&m, 0, sizeof(m));
	memset(mock_hc, 0, sizeof(mock_hc));
	memset(mock_core, 0, sizeof(mock_core));

	/* LK hand-off: vendor core in HC mode, feedback clock reset disabled,
	 * 8-bit high-speed bus already selected. */
	core_wr(CORE_HC_MODE, CORE_HC_MODE_EN | CORE_FF_CLK_SW_RST_DIS);
	core_wr(CORE_PWRCTL_MASK, 0x00000000u);
	mock_hc[SDHCI_HOST_CONTROL] = SDHCI_CTRL_8BITBUS | SDHCI_CTRL_DMA_MASK;

	for (uint32_t b = 0; b < MOCK_CARD_BLOCKS; b++) {
		for (uint32_t i = 0; i < SDHCI_BLOCK; i++) {
			mock_card[b * SDHCI_BLOCK + i] =
			    (uint8_t)(0x5a ^ (b * 7u) ^ i);
		}
	}
}

static void
test_registers(void)
{
	/* Standard SDHCI register file */
	assert(SDHCI_BLOCK_SIZE == 0x04);
	assert(SDHCI_BLOCK_COUNT == 0x06);
	assert(SDHCI_ARGUMENT == 0x08);
	assert(SDHCI_TRANSFER_MODE == 0x0C);
	assert(SDHCI_COMMAND == 0x0E);
	assert(SDHCI_RESPONSE == 0x10);
	assert(SDHCI_BUFFER == 0x20);
	assert(SDHCI_PRESENT_STATE == 0x24);
	assert(SDHCI_HOST_CONTROL == 0x28);
	assert(SDHCI_CLOCK_CONTROL == 0x2C);
	assert(SDHCI_SOFTWARE_RESET == 0x2F);
	assert(SDHCI_INT_STATUS == 0x30);

	/* Qualcomm MSM v4 vendor core registers (sdhci_msm_mci_offset) */
	assert(CORE_HC_MODE == 0x78);
	assert(CORE_PWRCTL_STATUS == 0xdc);
	assert(CORE_PWRCTL_MASK == 0xe0);
	assert(CORE_HC_MODE_EN == (1u << 0));
	assert(CORE_FF_CLK_SW_RST_DIS == (1u << 13));

	/* One 0x1000 MMIO window maps core + HC (0x900) including its 0x500 */
	assert(SDHCI_HC_OFFSET == 0x900);
	assert(SDHCI_HC_OFFSET + SDHCI_HC_SIZE <= SDHCI_CORE_SIZE);

	/* Command framing constants used by the driver */
	assert(SDHCI_MAKE_CMD(18, 0x3a) == 0x123a);
	assert(SDHCI_MAKE_CMD(12, 0x1b) == 0x0c1b);
	assert(SDHCI_MAKE_CMD(13, 0x1a) == 0x0d1a);
	assert(SDHCI_TRNS_READ | SDHCI_TRNS_MULTI | SDHCI_TRNS_BLK_CNT_EN == 0x32);
	assert(SDHCI_CMD_DATA | SDHCI_CMD_CRC | SDHCI_CMD_INDEX |
	    SDHCI_CMD_RESP_SHORT == 0x3a);
}

static void
test_take_over(void)
{
	reset_mock();
	assert(sdhci_take_over() == 0);
	/* Only CMD|DATA reset, and only once. */
	assert(m.reset_value == (SDHCI_RESET_CMD | SDHCI_RESET_DATA));
	assert(m.reset_writes == 1);
	/* CLOCK_CONTROL (0x2C) must never be written. */
	assert(m.clock_writes == 0);
	/* Command/data reset must not be RESET_ALL. */
	assert((m.reset_value & SDHCI_RESET_ALL) == 0);
	/* 8-bit bus preserved, DMA select cleared for PIO. */
	assert((hc_rd8(SDHCI_HOST_CONTROL) & SDHCI_CTRL_8BITBUS) != 0);
	assert((hc_rd8(SDHCI_HOST_CONTROL) & SDHCI_CTRL_DMA_MASK) == 0);
	/* Polled: interrupts masked and pending status cleared. */
	assert(hc_rd(SDHCI_INT_ENABLE) == 0);
	assert(hc_rd(SDHCI_SIGNAL_ENABLE) == 0);
	assert(hc_rd(SDHCI_INT_STATUS) == 0);
	assert(hc_rd16(SDHCI_BLOCK_SIZE) == 0);

	/* Without HC_MODE_EN the core is not ours. */
	reset_mock();
	core_wr(CORE_HC_MODE, 0);
	assert(sdhci_take_over() == ENXIO);
}

static void
test_cmd13_status(void)
{
	uint32_t resp = 0;
	int error;

	reset_mock();
	assert(sdhci_take_over() == 0);

	m.inhibit_polls = 3;    /* controller busy for the first three polls */
	error = sdhci_cmd(13, 0, 0,
	    SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX, &resp);
	assert(error == 0);
	assert(m.state_reads >= 4);
	assert(resp == 0x00000900u);
	assert(((resp >> 9) & 0xf) == 4);       /* TRAN */

	/* ARGUMENT / TRANSFER_MODE / COMMAND bytes actually hit the FIFO. */
	assert(m.cmd_count == 1);
	assert(m.trace[0].cmd == 13);
	assert(m.trace[0].arg == 0);
	assert(m.trace[0].tr_mode == 0);
	assert(mock_rd32(SDHCI_ARGUMENT) == 0);
	assert(m.trace[0].word == 0x0d1a);
	assert(hc_rd(SDHCI_INT_STATUS) == 0);   /* CC acknowledged */
}

static void
test_cid_csd(void)
{
	uint32_t cid[4] = { 0 }, csd[4] = { 0 };

	reset_mock();
	assert(sdhci_take_over() == 0);
	assert(sdhci_read_cid_csd(cid, csd) == 0);

	assert(m.cmd_count == 2);
	assert(m.trace[0].cmd == 10);
	assert(m.trace[1].cmd == 9);
	/* R2 is stored least-significant-word first in RESPONSE[0]: the driver
	 * reverses it so out[0] is bits 127:96 (RESPONSE+12). */
	assert(cid[3] == 0x15010044u);
	assert(cid[2] == 0x33413831u);
	assert(cid[0] == 0x00000000u);
	assert(csd[3] == 0xd00f590au);
	assert(csd[1] == 0x3b9aca00u);
}

static void
test_read_pio(void)
{
	static uint8_t buf[3 * SDHCI_BLOCK];
	int error;

	reset_mock();
	assert(sdhci_take_over() == 0);
	memset(buf, 0, sizeof(buf));
	error = sdhci_read_blocks_pio(2, 3, buf);
	assert(error == 0);

	/* Block geometry and multi-block read framing. */
	assert(m.blk_size == SDHCI_BLOCK);
	assert(m.blk_count == 3);
	assert(m.trace[0].cmd == 18);
	assert(m.trace[0].arg == 2);           /* sector, not byte, addressing */
	assert(m.trace[0].tr_mode == 0x32);
	/* CMD18: R1 + CRC + index + data (0x3a) */
	assert(m.trace[0].word == 0x123a);

	/* Data really came from the addressed card sectors. */
	assert(memcmp(buf, mock_card + 2 * SDHCI_BLOCK, sizeof(buf)) == 0);

	/* BLK_CNT_EN stops the card itself: no CMD12 on the success path. */
	assert(m.cmd_count == 1);
	assert(hc_rd(SDHCI_INT_STATUS) == 0);
}

static void
test_write_pio(void)
{
	static uint8_t buf[2 * SDHCI_BLOCK];
	int error;

	reset_mock();
	assert(sdhci_take_over() == 0);
	for (uint32_t i = 0; i < sizeof(buf); i++) {
		buf[i] = (uint8_t)(0xa5 ^ (i * 3u));
	}
	error = sdhci_write_blocks_pio(7, 2, buf);
	assert(error == 0);

	assert(m.trace[0].cmd == 25);
	assert(m.trace[0].arg == 7);
	assert(m.trace[0].tr_mode == 0x22);     /* MULTI | BLK_CNT_EN, no READ */
	assert(m.trace[0].word == 0x193a);
	assert(memcmp(mock_card + 7 * SDHCI_BLOCK, buf, sizeof(buf)) == 0);
	/* No CMD12 after a successful BLK_CNT_EN transfer. */
	assert(m.cmd_count == 1);
	assert(hc_rd(SDHCI_INT_STATUS) == 0);
}

static void
test_bounds(void)
{
	static uint8_t buf[SDHCI_BLOCK];

	reset_mock();
	assert(sdhci_take_over() == 0);

	assert(sdhci_read_blocks_pio(0, 0, buf) == EINVAL);
	assert(sdhci_read_blocks_pio(0, SDHCI_MAX_BLOCKS + 1, buf) == EINVAL);
	assert(sdhci_read_blocks_pio(0x100000000ull, 1, buf) == EINVAL);
	assert(sdhci_write_blocks_pio(0, 0, buf) == EINVAL);
	assert(sdhci_write_blocks_pio(0, SDHCI_MAX_BLOCKS + 1, buf) == EINVAL);
	/* Rejected requests never touch the controller. */
	assert(m.cmd_count == 0);
	assert(mock_rd16(SDHCI_COMMAND) == 0);
}

static void
test_errors(void)
{
	uint32_t resp = 0;

	/* Command error: INT_STATUS error bit, driver clears it and reports EIO */
	reset_mock();
	assert(sdhci_take_over() == 0);
	m.fail_cmd = 1;
	assert(sdhci_cmd(13, 0, 0,
	    SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX, &resp) == EIO);
	assert(hc_rd(SDHCI_INT_STATUS) == 0);

	/* Data error during a read: recover resets CMD|DATA, then a best-effort
	 * CMD12 leaves the card out of its data state. */
	reset_mock();
	assert(sdhci_take_over() == 0);
	{
		static uint8_t buf[2 * SDHCI_BLOCK];

		m.fail_data = 1;
		assert(sdhci_read_blocks_pio(0, 2, buf) == EIO);
		assert(m.reset_writes >= 1);
		assert(m.trace[m.cmd_count - 1].cmd == 12);
	}

	/* Timeout: the command never completes, but we must not hang */
	reset_mock();
	assert(sdhci_take_over() == 0);
	m.stall_cmd = 1;
	{
		uint64_t start = host_now_ns();
		int error = sdhci_cmd(13, 0, 0,
		    SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX, &resp);

		assert(error == ETIMEDOUT);
		assert(host_now_ns() - start >= (uint64_t)CMD_TIMEOUT_US * 1000ull);
	}

	/* Inhibit that clears: the driver retries rather than failing */
	reset_mock();
	assert(sdhci_take_over() == 0);
	m.inhibit_polls = 5;
	assert(sdhci_cmd(13, 0, 0,
	    SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX, &resp) == 0);

	/* DATA_INHIBIT (PRESENT_STATE bit 1) is waited on as well: a stuck data
	 * path must fail closed with EBUSY, not issue the command. */
	reset_mock();
	assert(sdhci_take_over() == 0);
	m.present_state = SDHCI_DATA_INHIBIT;
	assert(sdhci_cmd(13, 0, 0,
	    SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC | SDHCI_CMD_INDEX, &resp) == EBUSY);
	assert(m.cmd_count == 0);
}

int
main(void)
{
	test_registers();
	test_take_over();
	test_cmd13_status();
	test_cid_csd();
	test_read_pio();
	test_write_pio();
	test_bounds();
	test_errors();

	printf("test_sdhci_mock: all assertions passed\n");
	return 0;
}
