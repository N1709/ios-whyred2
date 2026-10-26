# Working on ios-sweet

- Read `NOTES.md` before starting; it contains the project goal, hardware facts,
  current blocker, local artifact locations and the next concrete action.
- The user switches agents frequently. Update the active handoff in `NOTES.md`
  after each milestone and before stopping. Record commits, CI run IDs, exact
  build/boot results and unfinished work. Distinguish observations from guesses.
- Commit completed changes. All new commits must include the trailer `Co-Authored-By: Gemini <noreply@google.com>`. Use `gh` for GitHub Actions kernel builds and logs.
  Local single-file cross-compiles are useful before spending a CI run.
- Leverage OpenCode for task delegation:
  - Use DeepSeek v4.1 Flash (`opencode run -m opencode/deepseek-v4.1-flash-free --auto "<prompt>"`) for reasoning, hardware register analysis, debugging, and authoring code modifications.
- Device bring-up uses temporary `fastboot boot` images. Preserve the Android
  installation and partition backups. The user approved (2026-10-15) using
  internal UFS for iOS and said Android may be deleted: the iOS root goes
  into the existing `userdata` partition (no GPT changes). Keep bootloaders,
  recovery, modem, persist and the backups intact, and confirm each
  destructive step with the user before running it.
- Keep device dumps, serial numbers and downloaded Apple artifacts out of git.

## Reference sources (user instruction, 2026-10-26)

When something on the whyred port cannot be worked out from XNU alone, check
the Android / Linux sources for this exact phone before guessing:

- `../refs/whyred-android/` (durable local copy, outside git):
  - `sdhci-msm.c`, `sdhci.c`, `sdhci.h` - Qualcomm's SDCC v5 driver from the
    MIUI/whyred kernel. Source of the confirmed vendor behaviour:
    `qcom,bus-width=<8>`, 25 MHz in `qcom,clk-rates`,
    `SDHCI_QUIRK2_USE_RESERVED_MAX_TIMEOUT` (TMO `0x0F` legal),
    `SDHCI_QUIRK2_RDWR_TX_ACTIVE_EOT`, and `CORE_PWRSAVE_DLL` (vendor spec 3
    bit 3) cleared outside HS400.
  - `dts_sdm660_whyred.dtsi`, `dts_sdm636_whyred.dtsi`,
    `dts_sdm660-mtp_whyred.dtsi` - the phone's SDCC, GCC, pinctrl and
    regulator nodes.
- `/home/n1709/linux/` - mainline Linux (7.2.10) with
  `drivers/mmc/host/sdhci-msm.c` and `drivers/mmc/host/sdhci.c`; useful for
  the standard SDHCI state machine and quirks.
- The MIUI files came from the Xiaomi whyred kernel source (`whyred-o-oss` /
  `whyred-q-oss` branches on Xiaomi_Kernel_OpenSource). If the local copy is
  lost, re-fetch those files rather than reconstructing register behaviour
  from memory.
