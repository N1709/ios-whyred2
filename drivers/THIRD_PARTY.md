# Whyred driver provenance

The `Whyred*/` directories hold driver source from the **UniXNU-whyred** device
repository (`https://github.com/N1709/UniXNU-whyred`, `drivers/`). These are
whyred-specific drivers for the same SDM660/SDM636 family, staged there before
promotion into UniXNU components.

| | |
| --- | --- |
| Source | `UniXNU-whyred/drivers` |
| Upstream of the `native/` sources | `qine233/UniXNU-clover`, commit `4c5652e`, BSD-2-Clause-Patent |
| The `Whyred*.c` sources | the earlier whyred XNU port, kept as the record of what already ran |
| Licence text | one `LICENSE` per `native/` directory |

## These files are not build inputs here

Nothing under `Whyred*/` is compiled in this repository. The kernel build only
sees the patched XNU tree, so a driver that actually runs must arrive as a
numbered patch in `xnu/patches/` and be mirrored by `tools/extract_drivers.py`.

If a file here is adapted into the boot kernel, the adapted code goes into a
patch; the driver source stays as the board's record.

## What the drivers cover

| Driver | Role | whyred match |
| --- | --- | --- |
| `WhyredStorage`, `WhyredBlock` | eMMC host and `IOBlockStorageDevice` | same SDCC v5 controller this port already drives (`santoni_sdhci.c`) |
| `WhyredDisplay` | GOP handoff, MDP/DSI panel | same panel path; compare property sets before changing `AppleCLCD` |
| `WhyredGpu` | Adreno 509 clock and chip ID | same a5xx GPU, no firmware/ringbuffer yet |
| `WhyredTouch`, `WhyredInput`, `WhyredHidInput` | Synaptics DSX transport, probe and HID bridge | same controller; the port's `santoni_touch.c` is ahead on the bus experiments |
| `WhyredAudio` | ADSP over GLINK/SMEM | same ADSP; no codec path yet |
| `WhyredPower`, `WhyredPMGR`, `WhyredCpu` | PM660/PM660L rails, PMGR clock/reset, CPU/GIC/timer | same PMIC and clock tree |
| `WhyredKeyStore` | key-store event bridge | board-independent glue |

There is no Wi-Fi driver in the UniXNU tree yet, so `drivers/wifi/` still
refers to the topaz modem path in this repository.
