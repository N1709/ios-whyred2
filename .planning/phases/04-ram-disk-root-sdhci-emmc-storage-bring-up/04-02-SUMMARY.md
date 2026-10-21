---
phase: 04-ram-disk-root-sdhci-emmc-storage-bring-up
plan: 02
subsystem: storage
tags: [sdhci, msm8937, emmc, stor-02, polled-pio, adt, bdevsw]
status: complete
dependency_graph:
  requires: [04-01]
  provides: [STOR-02, /dev/emmcroot0, /santoni-sdhci]
  affects: [bsd-dev, iokit-bsdinit, adt-synthesis, root-mount]
tech_stack:
  added:
    - tests/test_sdhci_mock.c
    - xnu/patches/0142-santoni-sdhci-emmc-driver.patch
  modified:
    - tools/mkadt.py
    - NOTES.md
patterns:
  - Polled SDHCI MSM v4 command dispatch (PRESENT_STATE inhibit -> COMMAND -> INT_STATUS)
  - PIO FIFO block transfers (CMD18/CMD25 + CMD12), no DMA/ADMA
  - LK bootloader state retention (no RESET_ALL, no CLOCK_CONTROL write)
  - BSD bdevsw/cdevsw + devfs registration, GPT partition slicing
decisions:
  - "Test drives the same framing/PIO routines the driver uses against a mock controller that models the inhibit handshake, FIFO, BRR/BWR and CC/TC/ERR bits, so register-level regressions fail on the host"
  - "CMD12 is framed R1b (0x1b = RESP_SHORT_BUSY|CRC|INDEX); the research sketch's 0x1a omitted the busy response type"
  - "ADT node is at the root (not under /arm-io) so reg stays absolute and is not offset by the /arm-io ranges property"
  - "Driver maps one 0x1000 window at 0x07824000; HC is base+0x900. ADT also carries hc-reg for documentation"
  - "SDHCI make blocking-clock check is read-only (0x2C); a disabled card clock fails init instead of writing GCC-derived state"
metrics:
  duration: ~35m
  completed: "2026-10-21"
actuals:
  tokens: 50000
  tasks: 2
  commits: 2
  plan_head_before: 3a6a9cb
  plan_head_after: fbc7f7d
---

# Phase 04 Plan 02: Qualcomm SDHCI MSM v4 eMMC Driver Summary

Qualcomm SDHCI MSM v4 (SDCC1) host-side verification, an XNU block driver
patch for the internal eMMC 5.1, and the matching ADT nub are authored.
Nothing has run on silicon yet: the driver is only validated by the host mock
test, the ADT dump, and patch-application checks.

## Tasks Completed

| Task | Name | Commit | Key Files |
|------|------|--------|-----------|
| 04-02-01 | Author Qualcomm SDHCI MSM v4 Register Definitions, Command Framing, and Host Unit Test (STOR-02) | `ed76a6d` | `tests/test_sdhci_mock.c` |
| 04-02-02 | Author XNU SDHCI Driver Patch and ADT Device Tree Nub for Santoni eMMC 5.1 (STOR-02) | `fbc7f7d` | `xnu/patches/0142-santoni-sdhci-emmc-driver.patch`, `tools/mkadt.py` |

## Deliverables

### 1. `tests/test_sdhci_mock.c` (903 lines build, ~700 source lines)

Self-contained host test of the register file and transfer logic, mirroring
what `bsd/dev/santoni_sdhci.c` does:

- **Register map**: standard SDHCI offsets 0x04/0x06/0x08/0x0C/0x0E/0x10/
  0x20/0x24/0x28/0x2C/0x2F/0x30, Qualcomm core offsets 0x78 (HC_MODE,
  HC_MODE_EN bit 0, FF_CLK_SW_RST_DIS bit 13), 0xdc, 0xe0; asserts the
  0x900 HC/Core delta and that HC+0x500 fits in the 0x1000 window.
- **Mock controller**: PRESENT_STATE CMD inhibit (programmable poll count),
  COMMAND write -> trace + response, INT_STATUS CC/TC/BWR/BRR/ERR with
  write-1-to-clear semantics, 512-byte FIFO model, fake 16-block card,
  writable CMD register, self-clearing SOFTWARE_RESET, CLOCK_CONTROL write
  counter, and stall/fail injection.
- **Take-over test**: asserts exactly one `RESET_CMD|RESET_DATA` write, no
  `RESET_ALL`, zero writes to 0x2C, 8-bit bus preserved, DMA select cleared,
  interrupts masked; ENXIO when HC_MODE_EN is clear.
- **Command tests**: CMD13 (R1 0x900, state 4 = TRAN), CMD10/CMD9 128-bit
  responses read from 0x10..0x1C, inhibit retry path.
- **PIO tests**: 3-block CMD18 read from LBA 2 compared byte-for-byte with
  the card image, CMD12 termination (`0x0c1b`), 2-block CMD25 write at LBA 7
  read back, `EINVAL` for 0 / >2048 blocks / LBA > UINT32_MAX with no
  controller traffic, error-bit -> EIO with status cleared, data error ->
  EIO with CMD12 still sent, stalled command -> ETIMEDOUT after the deadline.

### 2. `xnu/patches/0142-santoni-sdhci-emmc-driver.patch`

Three-file git diff (verified with both `git apply --check` and
`patch -p1 --forward --dry-run` against a synthetic post-0094 tree):

- `bsd/conf/files`: `bsd/dev/santoni_sdhci.c standard` after
  `sweet_cpufreq.c`.
- `iokit/bsddev/IOKitBSDInit.cpp`: `rdBootVar == "emmcroot0"` block guarded by
  `ARM64_BOARD_CONFIG_SANTONI`, calls `santoni_sdhci_rootdev(root)`, panics with
  the errno if unavailable, otherwise sets rootName/oflags and jumps to
  `iofrootx`. Inserted after the existing sweet-UFS block, before the md0
  comment (which stays intact).
- `bsd/dev/santoni_sdhci.c` (903 lines): polled driver
  - maps `/santoni-sdhci` `reg` (0x07824000, 0x1000) with `ml_io_map()`;
  - all standard registers are accessed at base+0x900, core at base+0;
  - `sdhci_take_over()` validates HC_MODE_EN, sets FF_CLK_SW_RST_DIS if needed,
    issues only `RESET_CMD|RESET_DATA`, clears DMA select in HOST_CONTROL,
    masks/clars interrupts, and only *reads* CLOCK_CONTROL;
  - CMD13 TRAN check, CMD10 CID, CMD9 CSD, CMD8 ext-CSD (SEC_COUNT at 212),
    logged with MID/OID/PNM/revision/serial;
  - GPT parse of LBA 1 to find `emmcroot` (default `userdata`);
  - PIO CMD18 read / CMD25 write with BRR/BWR polling, TC wait, CMD12 stop,
    1 MiB contiguous bounce buffer and `flush_dcache64` around it;
  - bdevsw + cdevsw, `/dev/emmcroot0` and `/dev/remmcroot0`;
  - writes gated by `emmcrw=1` and clamped to the partition in both
    `santoni_sdhci_write_blocks()` and the strategy routine;
  - `santoni_sdhci_rootdev()` initializes lazily for `rd=emmcroot0`.

### 3. `tools/mkadt.py`

New root node `/santoni-sdhci`:

```
santoni-sdhci {
  compatible = 'qcom,sdhci-msm-v4|generic-sdhci'
  device_type = 'sdhci'
  reg = <0x7824000 0x1000>
  hc-reg = <0x7824900 0x500>
  bus-width = 0x8
  non-removable = <>
}
```

Placed at the root (like `/sweet-ufs`) so `reg` is absolute and not translated
by `/arm-io`'s `ranges`.

## Verification

Task 04-02-01:

```bash
clang -O2 -Wall -I. tests/test_sdhci_mock.c -o out/test_sdhci_mock && ./out/test_sdhci_mock
# test_sdhci_mock: all assertions passed
```

Also clean under `-Wextra`.

Task 04-02-02:

```bash
test -s xnu/patches/0142-santoni-sdhci-emmc-driver.patch \
  && grep -q "0x07824000" xnu/patches/0142-santoni-sdhci-emmc-driver.patch \
  && grep -q "emmcroot0" xnu/patches/0142-santoni-sdhci-emmc-driver.patch \
  && python3 -m py_compile tools/mkadt.py \
  && python3 tools/mkadt.py out/santoni_test.adt --dump | grep -q "santoni-sdhci"
# TASK2_VERIFY_OK
```

Additional local checks (not part of the plan command):

- `patch -p1 --forward --dry-run` exits 0 against a synthetic tree that already
  contains patch 0028's sweet-UFS block and 0094's `sweet_cpufreq.c` line, and
  also against a pristine tree with neither.
- `git apply --check -p1` exits 0.
- Applied driver file is byte-identical to the staged source.
- Only reads of `SDHCI_CLOCK_CONTROL`; `SDHCI_RESET_ALL` appears only in the
  define and comments.

## Observations vs. Open Items

- **Observation:** GNU patch 2.8 on Termux rejects short hunks that end one
  line before unrelated long context (it reports fuzz on identical tabbed
  lines). Hunks here carry 3+ trailing context lines and apply cleanly; the
  same diff is accepted by `git apply`.
- **Observation:** the test is not yet wired into `shim/build.sh` or CI, so a
  regression in the mock test will not fail the kernel build by itself.
- **Unfinished / next:** compile the kernel with 0142 in CI (no local XNU
  tree), confirm the ADT node is visible at boot, then `fastboot boot` and
  check the `SANTONI SDHCI:` CID/CSD/capacity lines and the
  `"userdata" first block ...` line. Do not pass `emmcrw=1` until the GPT
  bounds have been observed on the device. If `sdhci_take_over()` reports
  "card clock not enabled", LK did not leave SDCC1 clocked and the plan's
  state-retention assumption (A1) needs revisiting rather than a 0x2C write.
- Plan 04-03 (HFS+ mount, persistent log) is still pending.
