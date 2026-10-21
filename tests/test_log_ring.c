/*
 * tests/test_log_ring.c
 *
 * Host unit test for persistent 4 MiB console log circular ring buffer,
 * atomic append simulation, santoni_log_header binary packing, and
 * stream decoder reconstruction.
 *
 * Requirements: STOR-04
 * Threat Register: T-04-06, T-04-07
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#define SANTONI_LOG_SIZE          (4u << 20) /* 4 MiB = 4,194,304 bytes */
#define SDHCI_BLOCK               512
#define SANTONI_LOG_BLOCKS        (1 + SANTONI_LOG_SIZE / SDHCI_BLOCK) /* 8,193 blocks */

/*
 * Binary log header written to Sector 0 of SYSTEM partition
 */
struct santoni_log_header {
	char magic[12];                  /* "SANTONILOG1\0" */
	uint32_t version;               /* 1 */
	uint32_t size;                  /* Ring buffer size (4 MiB) */
	uint64_t total;                 /* Total cumulative bytes written */
	uint64_t seq;                   /* Flush sequence counter */
} __attribute__((packed));

_Static_assert(sizeof(struct santoni_log_header) == 36, "santoni_log_header packed size must be 36 bytes");

/*
 * Static 4 MiB circular ring buffer and total byte counter
 */
static char santoni_log_ring[SANTONI_LOG_SIZE];
static uint64_t santoni_log_total = 0;

/*
 * Simulated santoni_log_append matching XNU serial_console.c hook
 */
static void
santoni_log_append(const char *c, int size)
{
	if (size <= 0) {
		return;
	}
	uint64_t at = __atomic_fetch_add(&santoni_log_total, (uint64_t)size, __ATOMIC_RELAXED);
	for (int i = 0; i < size; i++) {
		santoni_log_ring[(at + (uint64_t)i) % SANTONI_LOG_SIZE] = c[i];
	}
}

/*
 * Simulated decoder reconstructing linear stream matching tools/sweet-log.py
 */
static void
decode_log_stream(const char *ring, uint64_t total, uint32_t size, char *out_buf, size_t *out_len)
{
	if (total <= size) {
		memcpy(out_buf, ring, (size_t)total);
		*out_len = (size_t)total;
	} else {
		uint32_t start = (uint32_t)(total % size);
		uint32_t first_part = size - start;
		memcpy(out_buf, ring + start, first_part);
		memcpy(out_buf + first_part, ring, start);
		*out_len = size;
	}
}

int
main(void)
{
	printf("=== Running tests/test_log_ring ===\n");

	/* Test 1: Header packing and structure verification */
	struct santoni_log_header hdr;
	memset(&hdr, 0, sizeof(hdr));
	memcpy(hdr.magic, "SANTONILOG1", 12);
	hdr.version = 1;
	hdr.size = SANTONI_LOG_SIZE;
	hdr.total = 42;
	hdr.seq = 1;

	assert(sizeof(hdr) == 36);
	assert(strcmp(hdr.magic, "SANTONILOG1") == 0);
	assert(hdr.version == 1);
	assert(hdr.size == (4u << 20));
	assert(hdr.total == 42);
	assert(hdr.seq == 1);
	assert(SANTONI_LOG_BLOCKS == 8193);
	printf("  [PASS] struct santoni_log_header packing verified (36 bytes, magic SANTONILOG1)\n");

	/* Test 2: Unwrapped stream append and decode */
	memset(santoni_log_ring, 0, sizeof(santoni_log_ring));
	santoni_log_total = 0;

	const char *msg1 = "Darwin Kernel Version 22.6.0: Wed Oct 21 2026 santoni\n";
	const char *msg2 = "SANTONI SDHCI: eMMC 5.1 card found (rev 0x48)\n";
	santoni_log_append(msg1, (int)strlen(msg1));
	santoni_log_append(msg2, (int)strlen(msg2));

	assert(santoni_log_total == strlen(msg1) + strlen(msg2));

	char *decoded = malloc(SANTONI_LOG_SIZE + 1);
	assert(decoded != NULL);
	size_t decoded_len = 0;

	decode_log_stream(santoni_log_ring, santoni_log_total, SANTONI_LOG_SIZE, decoded, &decoded_len);
	decoded[decoded_len] = '\0';

	char expected[256];
	snprintf(expected, sizeof(expected), "%s%s", msg1, msg2);
	assert(decoded_len == strlen(expected));
	assert(strcmp(decoded, expected) == 0);
	printf("  [PASS] Unwrapped stream append and decode matched exactly (%zu bytes)\n", decoded_len);

	/* Test 3: Circular wrap-around mechanics */
	/* Advance buffer near wrap-around boundary: SANTONI_LOG_SIZE - 10 bytes */
	memset(santoni_log_ring, 0, sizeof(santoni_log_ring));
	santoni_log_total = 0;

	/* Append 4 MiB - 10 bytes of 'A' */
	size_t chunk = 64 * 1024;
	char *buf = malloc(chunk);
	assert(buf != NULL);
	memset(buf, 'A', chunk);

	size_t target = SANTONI_LOG_SIZE - 10;
	size_t written = 0;
	while (written < target) {
		size_t to_write = (target - written > chunk) ? chunk : (target - written);
		santoni_log_append(buf, (int)to_write);
		written += to_write;
	}
	assert(santoni_log_total == target);

	/* Append 20 bytes across the wrap boundary: 10 'B's before wrap, 10 'C's after wrap */
	char wrap_test[20];
	memset(wrap_test, 'B', 10);
	memset(wrap_test + 10, 'C', 10);
	santoni_log_append(wrap_test, 20);

	assert(santoni_log_total == SANTONI_LOG_SIZE + 10);

	/* Verify ring indices directly */
	assert(memcmp(santoni_log_ring + SANTONI_LOG_SIZE - 10, "BBBBBBBBBB", 10) == 0);
	assert(memcmp(santoni_log_ring, "CCCCCCCCCC", 10) == 0);

	/* Decode wrapped stream */
	decode_log_stream(santoni_log_ring, santoni_log_total, SANTONI_LOG_SIZE, decoded, &decoded_len);
	assert(decoded_len == SANTONI_LOG_SIZE);
	/* First bytes of decoded should be 'A's (oldest surviving) */
	assert(decoded[0] == 'A');
	/* Byte SANTONI_LOG_SIZE - 20 to -11 should be 'B' */
	assert(memcmp(decoded + SANTONI_LOG_SIZE - 20, "BBBBBBBBBB", 10) == 0);
	/* Last 10 bytes should be 'C' */
	assert(memcmp(decoded + SANTONI_LOG_SIZE - 10, "CCCCCCCCCC", 10) == 0);
	printf("  [PASS] Circular wrap-around boundary verified across modulo boundary\n");

	/* Test 4: Heavy wrap-around stress test (write 10 MiB of cyclical numbered stream) */
	memset(santoni_log_ring, 0, sizeof(santoni_log_ring));
	santoni_log_total = 0;

	for (int i = 0; i < (int)chunk; i++) {
		buf[i] = (char)('a' + (i % 26));
	}

	size_t total_stress = 10 * 1024 * 1024; /* 10 MiB */
	written = 0;
	while (written < total_stress) {
		size_t to_write = (total_stress - written > chunk) ? chunk : (total_stress - written);
		santoni_log_append(buf, (int)to_write);
		written += to_write;
	}
	assert(santoni_log_total == total_stress);

	decode_log_stream(santoni_log_ring, santoni_log_total, SANTONI_LOG_SIZE, decoded, &decoded_len);
	assert(decoded_len == SANTONI_LOG_SIZE);

	/* Verify that every decoded byte matches cyclical stream */
	uint64_t start_byte = santoni_log_total - SANTONI_LOG_SIZE;
	bool match = true;
	for (size_t i = 0; i < decoded_len; i++) {
		char expected_ch = (char)('a' + ((start_byte + i) % chunk % 26));
		if (decoded[i] != expected_ch) {
			match = false;
			break;
		}
	}
	assert(match == true);
	printf("  [PASS] 10 MiB wrap-around stress test passed; ring correctly holds newest 4 MiB\n");

	free(decoded);
	free(buf);
	printf("test_log_ring: all assertions passed\n");
	return 0;
}
