# storage

- **Hardware:** Qualcomm SDCC v5 controller, eMMC 5.1, `sdhc_1` at
  `0x0c0c4000`; 8-bit bus, high-speed timing (the card comes out of ABL in
  HS400, so the driver lowers `HS_TIMING` before the first data command).
- **Code:** `bsd/dev/santoni_sdhci.c`, added by
  `xnu/patches/0142-santoni-sdhci-emmc-driver.patch`; the SDHCI fixes are the
  later `0149`-`0166` patches, the board wiring is `0148`.
- **Proven on the phone:** PIO and SDMA reads, 1/8-bit, CMD6, GPT parse,
  `userdata` exposed as `emmcroot0`, HFSX mounted read-write, journal replay.
- **Not proven:** sustained I/O, repeated boots under load, HS200/HS400.
- **Next:** leave alone unless a boot log shows a data-path error.

The host-side mock lives in `tests/test_sdhci_mock.c` and `tests/test_gpt_parser.c`.

Mirror: `santoni_sdhci.c` in this folder (regenerated from the patch series by
`tools/extract_drivers.py`; do not edit it here).
