# WhyredSdhci

State: **adapted source**. Two implementations are staged here:

- `WhyredSdhci.c` — the polled SDHCI eMMC driver from the earlier whyred XNU
  port, with the `WHYRED` guard.
- `native/WhyredSdhci.cpp` — the native UniXNU driver ported from
  `qine233/UniXNU-clover` (commit `4c5652e`, BSD-2-Clause-Patent,
  `native/LICENSE`), with the class, symbols and boot arguments renamed to
  whyred and the `userdata` window set to `0x2b8c00..0x1d7d6ff`.

Neither is a build input yet (see `../README.md`).

## Hardware contract

- Controller: `sdhc_1` at `0xc0c4000`, `hc_mem` and `cmdq_mem` each `0x1000`.
- Interrupts: 110 and 112.
- Bus: 8-bit eMMC 5.1. ABL hands the card over powered, clocked and in transfer
  state, so the driver takes over that state instead of rebuilding the GCC
  clock and SPMI regulator trees.
- Standard SDHCI registers sit at `+0x900`; the Qualcomm legacy MCI block is at
  `+0x000`.
- Card-side geometry used by the port: `userdata` starts at 4 KiB LBA
  `0x2b8c00` and ends at `0x1d7d6ff` (109643 MiB); the backup GPT follows the
  entry.
- Vendor SDHCI sources are pinned in `refs/whyred-android/sdhci.h`, `sdhci.c`
  and `sdhci-msm.c`.

## Source

`WhyredSdhci.c` came from the previous whyred XNU port
(`ios-whyred2/drivers/storage/`). Its outer guard now selects on
`ARM64_BOARD_CONFIG_WHYRED` (the `LAVENDER` branches stay because the SDM660
host block is shared). The file keeps the `santoni_` identifiers the earlier
port used; they are internal names, not hardware claims.

Verified behaviour on the phone, from that port:

- polled PIO and SDMA reads at 1-bit and 8-bit;
- `CMD6`, GPT parse, `userdata` exposed as `emmcroot0`;
- HFSX mounted read-write with journal replay.

Host model verification:

- `native/tests/whyred_sdhci_host.cpp` passes all host assertions: SDCC1 controller
  handshake, inhibit/timeout states, bootloader handover invariants, UEFI 2.8 GPT
  parsing with IEEE 802.3 CRC32, strict userdata write boundary enforcement, and
  `kTeeProbeClasses` probe budgeting.

Not verified: sustained I/O, repeated boots under load, HS200/HS400.

## Safety

No write to the card while proving the host. Writes stay inside the `userdata`
GPT entry, and the backup GPT is never touched. The only non-`userdata` write
path is the persistent console log at the start of `system`, and it refuses
anything outside its 8193-sector area.

## Promotion

Move the file into the XNU source tree through `package/` (or a component
repository), add the build rule, and keep this README as the board contract.
The read-only smoke test stays the first boot milestone.
