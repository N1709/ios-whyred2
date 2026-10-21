---
phase: 04-ram-disk-root-sdhci-emmc-storage-bring-up
plan: 03
subsystem: storage
tags: [gpt, emmc, crc32, logging, console-ring, dyld-shared-cache, stor-03, stor-04, stor-05]
status: complete
dependency_graph:
  requires: [04-02]
  provides: [STOR-03, STOR-04, STOR-05, Hardened-GPT, Persistent-Log, 4K-Dyld-Cache]
  affects: [filesystem, kernel-console, userland-runtime]
tech_stack:
  added: []
  patterns: [Hardened UEFI 2.8 GPT CRC32 verification, Lock-free circular console ring buffer, SYSTEM partition log isolation, 16 KiB CodeDirectory compatibility on 4 KiB translation granule]
key_files:
  created:
    - tests/test_gpt_parser.c
    - tests/test_log_ring.c
    - xnu/patches/0143-santoni-persistent-console-log.patch
    - xnu/patches/0144-santoni-4k-unsigned-dyld-cache.patch
  modified:
    - tools/sweet-log-pull.sh
    - tools/sweet-log.py
    - NOTES.md
decisions:
  - "Validate UEFI 2.8 GPT header CRC32 and partition entry array CRC32 using zero-dependency IEEE 802.3 CRC-32 before mounting userdata root"
  - "Enforce strict write boundary clamping with EPERM rejection for any write outside userdata partition bounds or when su.writable is false"
  - "Flush 4 MiB circular console log ring to the idle SYSTEM partition (sectors 0..8192) to prevent raw block write collisions with active HFS+ root on userdata"
  - "Allow 16 KiB CodeDirectory pageSize (14) in cs_validate_page() and disable cs enforcement on shared region submap to run stock iOS 16 dyld shared cache on 4 KiB Cortex-A53"
metrics:
  duration: 10m
  completed: "2026-10-21"
actuals:
  tokens: 7828
  tasks: 3
  commits: 4
  plan_head_before: 3a6a9cbf2f4a6f47e169319d760dd678b204e8d1
  plan_head_after: 392c44d36199a6755ab35d094ce6d93d85378c01
---

# Phase 04 Plan 03: Hardened GPT Parser, Persistent Console Log, and 4 KiB Dyld Cache Summary

Hardened UEFI 2.8 GPT partition parser with IEEE 802.3 CRC32 verification and strict userdata write boundary clamping (STOR-03), persistent 4 MiB circular console log flusher targeting dedicated sectors on the idle SYSTEM partition (STOR-04), 4 KiB unsigned dyld shared cache compatibility patch (STOR-05), and recovery extraction tools.

## Objectives & Deliverables

1. **Hardened GPT Partition Parser & Write Boundary Clamping (`tests/test_gpt_parser.c`) [STOR-03]**:
   - Implemented UEFI 2.8 GPT header (92 bytes) and partition entry (128 bytes) structures conforming to spec.
   - Built zero-dependency IEEE 802.3 CRC32 verification routine (`poly 0xEDB88320`).
   - Validated "EFI PART" signature, header CRC32, and partition entry array CRC32.
   - Decoded UTF-16LE partition names to locate `userdata` (`su.first_block`, `su.nblocks`) and `system` (`sys.first_block`, `sys.nblocks`).
   - Implemented write boundary checking (`sdhci_assert_write_bounds`): enforces `su.writable` (rejecting with `EPERM`), blocks any write with `lba < su.first_block` or `lba + count > su.first_block + su.nblocks` with `EPERM`, and guards against integer overflow.
   - Verified edge cases: corrupted signature, corrupted header CRC32, corrupted array CRC32, inverted LBA bounds, and non-existent partitions.

2. **Persistent 4 MiB Console Log Ring Buffer on SYSTEM Partition (`tests/test_log_ring.c`, `xnu/patches/0143-santoni-persistent-console-log.patch`, `tools/sweet-log-pull.sh`, `tools/sweet-log.py`) [STOR-04]**:
   - `tests/test_log_ring.c`: Verified 4 MiB circular buffer wrap-around mechanics, atomic byte counter (`santoni_log_total`), packed binary header `struct santoni_log_header` (36 bytes, magic "SANTONILOG1", version 1, 4 MiB size, total bytes, flush seq), and linear text reconstruction. Tested unwrapped stream, wrap-around boundary, and 10 MiB stress test.
   - `xnu/patches/0143-santoni-persistent-console-log.patch`:
     - In `osfmk/console/serial_console.c`: hooked `_cnputs` under `#if defined(ARM64_BOARD_CONFIG_SANTONI)` to call `santoni_log_append(c, size)` without colliding with `sweet_ufs.c`'s `sweet_log_append`.
     - In `bsd/dev/santoni_sdhci.c`: allocated 4 MiB ring buffer, spawned `santoni_log_thread`, and flushed dirty blocks to the idle SYSTEM partition starting at `sys.first_block` (sectors 0..8192), strictly isolating log writes from the active HFS+ filesystem on userdata. Controlled via boot-arg `sweetlog`.
   - `tools/sweet-log-pull.sh` & `tools/sweet-log.py`:
     - Query `/dev/block/bootdevice/by-name/system` or `/dev/block/by-name/system` on Santoni to pull 8193 blocks (4 MiB + 512B header).
     - Decodes `SANTONILOG1` binary header and circular stream into plain text.

3. **4 KiB Unsigned Dyld Shared Cache Patch (`xnu/patches/0144-santoni-4k-unsigned-dyld-cache.patch`) [STOR-05]**:
   - In `bsd/kern/ubc_subr.c`:
     - `cs_validate_page()`: permitted `cd->pageSize == 14` (16 KiB hash unit) under `ARM64_BOARD_CONFIG_SANTONI` on 4 KiB VM page systems.
     - `ubc_cs_blob_add()`: bailed early when `cd->pageSize > PAGE_SHIFT` to prevent out-of-bounds page hashing during unsigned boot.
   - In `osfmk/vm/vm_shared_region.c`:
     - `vm_shared_region_enter()`: called `vm_map_cs_enforcement_set(sub_map, false)` under `ARM64_BOARD_CONFIG_SANTONI` so userland processes can execute code from the 16 KiB-signed iOS shared cache without code-signing termination.

## Tasks Completed

| Task | Name | Commit | Key Files |
|------|------|--------|-----------|
| 04-03-01 | Author Hardened GPT Partition Parser, CRC32 Verification, and Boundary Clamping (STOR-03) | `08e4806` | `tests/test_gpt_parser.c` |
| 04-03-02 | Author Persistent 4 MiB Console Log Ring Buffer on SYSTEM Partition and Recovery Tools (STOR-04) | `09c163c` | `tests/test_log_ring.c`, `xnu/patches/0143-santoni-persistent-console-log.patch`, `tools/sweet-log-pull.sh`, `tools/sweet-log.py` |
| 04-03-03 | Author 4 KiB Unsigned Shared Cache Patch for Cortex-A53 iOS 16 Userland (STOR-05) | `dadde27` | `xnu/patches/0144-santoni-4k-unsigned-dyld-cache.patch` |

## Verification Details

- **Task 04-03-01 Automated Verification**:
  ```bash
  mkdir -p out tests && clang -O2 -Wall -I. tests/test_gpt_parser.c -o out/test_gpt_parser && ./out/test_gpt_parser
  ```
  Result: Passed (`test_gpt_parser: all assertions passed`).

- **Task 04-03-02 Automated Verification**:
  ```bash
  mkdir -p out tests && clang -O2 -Wall -I. tests/test_log_ring.c -o out/test_log_ring && ./out/test_log_ring && test -s xnu/patches/0143-santoni-persistent-console-log.patch && grep -q "santoni_log" xnu/patches/0143-santoni-persistent-console-log.patch && bash -n tools/sweet-log-pull.sh && python3 -m py_compile tools/sweet-log.py
  ```
  Result: Passed (`test_log_ring: all assertions passed`, valid patch syntax, shell & python compilation passed).

- **Task 04-03-03 Automated Verification**:
  ```bash
  test -s xnu/patches/0144-santoni-4k-unsigned-dyld-cache.patch && grep -q "ARM64_BOARD_CONFIG_SANTONI" xnu/patches/0144-santoni-4k-unsigned-dyld-cache.patch && grep -q "vm_map_cs_enforcement_set" xnu/patches/0144-santoni-4k-unsigned-dyld-cache.patch
  ```
  Result: Passed (valid patch with required guards and submap CS enforcement call).

## Deviations from Plan

None - plan executed exactly as written.

## Self-Check: PASSED

- [x] `tests/test_gpt_parser.c` exists and passes all assertions.
- [x] `tests/test_log_ring.c` exists and passes all assertions.
- [x] `xnu/patches/0143-santoni-persistent-console-log.patch` exists and passes syntax validation.
- [x] `xnu/patches/0144-santoni-4k-unsigned-dyld-cache.patch` exists and passes syntax validation.
- [x] `tools/sweet-log-pull.sh` and `tools/sweet-log.py` compile and execute cleanly.
- [x] Commits `08e4806`, `09c163c`, and `dadde27` exist in git history.
