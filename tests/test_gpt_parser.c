/*
 * tests/test_gpt_parser.c
 *
 * Host unit test for zero-dependency UEFI 2.8 GPT partition parser,
 * IEEE 802.3 CRC32 verification, and userdata write boundary enforcement.
 *
 * Requirements: STOR-03
 * Threat Register: T-04-05 (Tampering)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#define SDHCI_BLOCK 512

/*
 * UEFI 2.8 Specification - Section 5.3.1: GPT Header
 */
struct gpt_header {
	char signature[8];                    /* "EFI PART" */
	uint32_t revision;                   /* 0x00010000 */
	uint32_t header_size;                /* 92 bytes */
	uint32_t header_crc32;               /* CRC32 with this field zeroed */
	uint32_t reserved;                   /* Must be 0 */
	uint64_t my_lba;                     /* LBA 1 */
	uint64_t alternate_lba;              /* Backup GPT LBA */
	uint64_t first_usable_lba;           /* Primary usable LBA */
	uint64_t last_usable_lba;            /* Last usable LBA */
	uint8_t disk_guid[16];               /* Unique disk GUID */
	uint64_t partition_entry_lba;        /* LBA 2 */
	uint32_t num_partition_entries;      /* Number of partition entries */
	uint32_t sizeof_partition_entry;     /* 128 bytes */
	uint32_t partition_entry_array_crc32;/* CRC32 of partition array */
} __attribute__((packed));

_Static_assert(sizeof(struct gpt_header) == 92, "GPT header size must be exactly 92 bytes");

/*
 * UEFI 2.8 Specification - Section 5.3.3: GPT Partition Entry
 */
struct gpt_entry {
	uint8_t type_guid[16];               /* Partition type GUID */
	uint8_t unique_guid[16];             /* Unique partition GUID */
	uint64_t starting_lba;               /* Starting LBA */
	uint64_t ending_lba;                 /* Ending LBA (inclusive) */
	uint64_t attributes;                 /* Attribute flags */
	uint16_t name[36];                   /* Partition name in UTF-16LE */
} __attribute__((packed));

_Static_assert(sizeof(struct gpt_entry) == 128, "GPT entry size must be exactly 128 bytes");

/*
 * Partition tracking state matching bsd/dev/santoni_sdhci.c
 */
struct partition_info {
	uint64_t first_block;
	uint64_t nblocks;
	bool writable;
};

static struct partition_info su;   /* userdata partition */
static struct partition_info sys;  /* system partition */

/*
 * Standard IEEE 802.3 CRC32 calculation (poly 0xEDB88320)
 */
static uint32_t
crc32_calculate(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int j = 0; j < 8; j++) {
			crc = (crc >> 1) ^ (0xEDB88320u & (-(crc & 1u)));
		}
	}
	return ~crc;
}

/*
 * Synthetic disk sector buffer for host testing (64 blocks * 512 bytes = 32 KiB)
 */
#define FAKE_DISK_BLOCKS 64
static uint8_t fake_disk[FAKE_DISK_BLOCKS * SDHCI_BLOCK];

static int
sdhci_read_blocks_mock(uint64_t lba, uint32_t count, void *buf)
{
	if (lba + count > FAKE_DISK_BLOCKS) {
		return EIO;
	}
	memcpy(buf, fake_disk + (lba * SDHCI_BLOCK), (size_t)count * SDHCI_BLOCK);
	return 0;
}

/*
 * Hardened GPT Parser with CRC32 Verification
 *
 * Verifies:
 *  1. EFI PART signature at LBA 1
 *  2. Header size validity and Header CRC32
 *  3. Partition entry dimensions and Entry Array CRC32
 *  4. Matches UTF-16LE partition names
 *  5. Validates non-inverted LBA bounds (last >= first)
 */
static int
sdhci_find_partition(const char *want, struct partition_info *out)
{
	uint8_t hdr_buf[SDHCI_BLOCK];
	int error = sdhci_read_blocks_mock(1, 1, hdr_buf);
	if (error) {
		return error;
	}

	struct gpt_header hdr;
	memcpy(&hdr, hdr_buf, sizeof(hdr));

	/* 1. Signature check */
	if (memcmp(hdr.signature, "EFI PART", 8) != 0) {
		return ENXIO;
	}

	/* 2. Header bounds & CRC32 check */
	if (hdr.header_size < sizeof(struct gpt_header) || hdr.header_size > SDHCI_BLOCK) {
		return EINVAL;
	}

	uint32_t stored_crc = hdr.header_crc32;
	hdr.header_crc32 = 0;
	uint32_t computed_crc = crc32_calculate((const uint8_t *)&hdr, hdr.header_size);
	if (computed_crc != stored_crc) {
		return EBADMSG;
	}

	/* 3. Partition entry array validation */
	if (hdr.sizeof_partition_entry != sizeof(struct gpt_entry) ||
	    hdr.num_partition_entries == 0 ||
	    hdr.num_partition_entries > 128) {
		return EINVAL;
	}

	uint32_t total_entry_bytes = hdr.num_partition_entries * hdr.sizeof_partition_entry;
	uint32_t entry_blocks = (total_entry_bytes + SDHCI_BLOCK - 1) / SDHCI_BLOCK;
	if (hdr.partition_entry_lba + entry_blocks > FAKE_DISK_BLOCKS) {
		return EIO;
	}

	uint8_t entries_buf[SDHCI_BLOCK * 8];
	if (entry_blocks * SDHCI_BLOCK > sizeof(entries_buf)) {
		return EINVAL;
	}

	error = sdhci_read_blocks_mock(hdr.partition_entry_lba, entry_blocks, entries_buf);
	if (error) {
		return error;
	}

	uint32_t computed_array_crc = crc32_calculate(entries_buf, total_entry_bytes);
	if (computed_array_crc != hdr.partition_entry_array_crc32) {
		return EBADMSG;
	}

	/* 4. Enumerate partition entries */
	for (uint32_t i = 0; i < hdr.num_partition_entries; i++) {
		const struct gpt_entry *e = (const struct gpt_entry *)(entries_buf + (i * hdr.sizeof_partition_entry));
		char name[37];
		unsigned c;
		for (c = 0; c < 36; c++) {
			uint16_t ch = e->name[c];
			if (ch == 0 || ch > 0x7f) {
				break;
			}
			name[c] = (char)ch;
		}
		name[c] = '\0';

		if (strcmp(name, want) == 0) {
			if (e->ending_lba < e->starting_lba) {
				return EINVAL;
			}
			out->first_block = e->starting_lba;
			out->nblocks = e->ending_lba - e->starting_lba + 1;
			return 0;
		}
	}

	return ENOENT;
}

/*
 * Write Boundary Assertion (STRIDE T-04-05 Mitigation / ASVS Level 1)
 *
 * Rejects with EPERM any write:
 *  - When partition write permission (!su.writable) is inactive
 *  - Starting before partition start (lba < su.first_block)
 *  - Extending beyond partition end (lba + count > su.first_block + su.nblocks)
 *  - With integer overflow (lba + count < lba)
 * Rejects with EINVAL if count == 0.
 */
static int
sdhci_assert_write_bounds(uint64_t lba, uint32_t count)
{
	if (!su.writable) {
		return EPERM;
	}
	if (count == 0) {
		return EINVAL;
	}
	if (lba + count < lba) {
		return EPERM;
	}
	if (lba < su.first_block || (lba + count) > (su.first_block + su.nblocks)) {
		return EPERM;
	}
	return 0;
}

/*
 * Helper to build valid synthetic GPT table on fake_disk
 */
static void
build_synthetic_gpt(void)
{
	memset(fake_disk, 0, sizeof(fake_disk));

	/* Partition entries at LBA 2 (up to 4 entries = 512 bytes = 1 block) */
	struct gpt_entry *entries = (struct gpt_entry *)(fake_disk + (2 * SDHCI_BLOCK));
	memset(entries, 0, SDHCI_BLOCK);

	/* Entry 0: "system" partition (LBA 1000..5000) */
	entries[0].starting_lba = 1000;
	entries[0].ending_lba = 5000;
	const char *sys_name = "system";
	for (int i = 0; sys_name[i]; i++) {
		entries[0].name[i] = (uint16_t)sys_name[i];
	}

	/* Entry 1: "userdata" partition (LBA 6000..20000) */
	entries[1].starting_lba = 6000;
	entries[1].ending_lba = 20000;
	const char *ud_name = "userdata";
	for (int i = 0; ud_name[i]; i++) {
		entries[1].name[i] = (uint16_t)ud_name[i];
	}

	uint32_t array_crc = crc32_calculate((const uint8_t *)entries, 4 * sizeof(struct gpt_entry));

	/* Header at LBA 1 */
	struct gpt_header *hdr = (struct gpt_header *)(fake_disk + (1 * SDHCI_BLOCK));
	memcpy(hdr->signature, "EFI PART", 8);
	hdr->revision = 0x00010000;
	hdr->header_size = (uint32_t)sizeof(struct gpt_header);
	hdr->my_lba = 1;
	hdr->alternate_lba = 63;
	hdr->first_usable_lba = 34;
	hdr->last_usable_lba = 62;
	hdr->partition_entry_lba = 2;
	hdr->num_partition_entries = 4;
	hdr->sizeof_partition_entry = (uint32_t)sizeof(struct gpt_entry);
	hdr->partition_entry_array_crc32 = array_crc;
	hdr->header_crc32 = 0;
	hdr->header_crc32 = crc32_calculate((const uint8_t *)hdr, hdr->header_size);
}

int
main(void)
{
	printf("=== Running tests/test_gpt_parser ===\n");

	/* Test 1: Valid GPT parsing */
	build_synthetic_gpt();

	int err = sdhci_find_partition("userdata", &su);
	assert(err == 0);
	assert(su.first_block == 6000);
	assert(su.nblocks == (20000 - 6000 + 1));
	printf("  [PASS] Parsed userdata partition: first_block=%llu, nblocks=%llu\n",
	       (unsigned long long)su.first_block, (unsigned long long)su.nblocks);

	err = sdhci_find_partition("system", &sys);
	assert(err == 0);
	assert(sys.first_block == 1000);
	assert(sys.nblocks == (5000 - 1000 + 1));
	printf("  [PASS] Parsed system partition: first_block=%llu, nblocks=%llu\n",
	       (unsigned long long)sys.first_block, (unsigned long long)sys.nblocks);

	/* Test 2: Non-existent partition */
	struct partition_info dummy;
	err = sdhci_find_partition("vendor", &dummy);
	assert(err == ENOENT);
	printf("  [PASS] Non-existent partition returns ENOENT\n");

	/* Test 3: Write Boundary Enforcement (su.writable = false) */
	su.writable = false;
	assert(sdhci_assert_write_bounds(6000, 10) == EPERM);
	printf("  [PASS] Write rejected with EPERM when su.writable is false\n");

	/* Test 4: Write Boundary Enforcement (su.writable = true) */
	su.writable = true;

	/* Valid writes within bounds */
	assert(sdhci_assert_write_bounds(6000, 1) == 0);
	assert(sdhci_assert_write_bounds(6000, (uint32_t)su.nblocks) == 0);
	assert(sdhci_assert_write_bounds(20000, 1) == 0);
	assert(sdhci_assert_write_bounds(10000, 500) == 0);
	printf("  [PASS] Valid writes within userdata boundaries succeed\n");

	/* Invalid writes rejected with EPERM */
	assert(sdhci_assert_write_bounds(5999, 1) == EPERM);       /* Starts before userdata */
	assert(sdhci_assert_write_bounds(5999, 10) == EPERM);      /* Spans across start */
	assert(sdhci_assert_write_bounds(20001, 1) == EPERM);      /* Starts after userdata */
	assert(sdhci_assert_write_bounds(20000, 2) == EPERM);      /* Spans across end */
	assert(sdhci_assert_write_bounds(0, 10) == EPERM);          /* Bootloader/GPT area */
	assert(sdhci_assert_write_bounds(1000, 10) == EPERM);       /* SYSTEM partition area */
	assert(sdhci_assert_write_bounds(UINT64_MAX - 5, 10) == EPERM); /* Integer overflow */
	assert(sdhci_assert_write_bounds(6000, 0) == EINVAL);      /* Zero count */
	printf("  [PASS] Out-of-bounds writes strictly fenced with EPERM\n");

	/* Test 5: Corrupt signature */
	build_synthetic_gpt();
	fake_disk[1 * SDHCI_BLOCK] = 'X'; /* Invalidate "EFI PART" */
	assert(sdhci_find_partition("userdata", &dummy) == ENXIO);
	printf("  [PASS] Corrupted signature returns ENXIO\n");

	/* Test 6: Corrupt header CRC32 */
	build_synthetic_gpt();
	struct gpt_header *hdr = (struct gpt_header *)(fake_disk + (1 * SDHCI_BLOCK));
	hdr->header_crc32 ^= 0x12345678;
	assert(sdhci_find_partition("userdata", &dummy) == EBADMSG);
	printf("  [PASS] Corrupted header CRC32 returns EBADMSG\n");

	/* Test 7: Corrupt partition entries CRC32 */
	build_synthetic_gpt();
	hdr = (struct gpt_header *)(fake_disk + (1 * SDHCI_BLOCK));
	hdr->partition_entry_array_crc32 ^= 0x87654321;
	assert(sdhci_find_partition("userdata", &dummy) == EBADMSG);
	printf("  [PASS] Corrupted partition array CRC32 returns EBADMSG\n");

	/* Test 8: Inverted LBA bounds (ending < starting) */
	build_synthetic_gpt();
	struct gpt_entry *entries = (struct gpt_entry *)(fake_disk + (2 * SDHCI_BLOCK));
	entries[1].starting_lba = 20000;
	entries[1].ending_lba = 6000; /* Inverted */
	/* Recompute array CRC and header CRC so parser reaches entry validation */
	hdr = (struct gpt_header *)(fake_disk + (1 * SDHCI_BLOCK));
	hdr->partition_entry_array_crc32 = crc32_calculate((const uint8_t *)entries, 4 * sizeof(struct gpt_entry));
	hdr->header_crc32 = 0;
	hdr->header_crc32 = crc32_calculate((const uint8_t *)hdr, hdr->header_size);
	assert(sdhci_find_partition("userdata", &dummy) == EINVAL);
	printf("  [PASS] Inverted LBA partition bounds return EINVAL\n");

	printf("test_gpt_parser: all assertions passed\n");
	return 0;
}
