# Third-party driver references

The `unixnu/` tree holds driver source taken from the user's **UniXNU-whyred**
device repository (`https://github.com/N1709/UniXNU-whyred`, `drivers/`). These
are whyred-specific drivers for the same SDM660/SDM636 family, staged there
before promotion into UniXNU components. Keeping the copy here gives this port
a same-board reference instead of a driver for different silicon.

| | |
| --- | --- |
| Source | `UniXNU-whyred/drivers` |
| Upstream of the `native/` sources | `qine233/UniXNU-clover`, commit `4c5652e`, BSD-2-Clause-Patent |
| The `Whyred*.c` sources | the earlier whyred XNU port, kept as the record of what already ran |
| Licence text | one `LICENSE` per `native/` directory, copied unchanged |
| Copied as | byte-for-byte, verified by `drivers/unixnu-SHA256SUMS` |

## These files are references, not build inputs

Nothing under `unixnu/` is compiled. The kernel build only sees the patched
XNU tree, so a driver that actually runs must arrive as a numbered patch in
`xnu/patches/` and be mirrored by `tools/extract_drivers.py`. The UniXNU tree
keeps its own checksums so it is always clear which code is ours and which is
staged for a different build system.

If a file here is adapted into a whyred driver, the adapted code goes into a
patch; the reference file stays untouched.

## What the UniXNU tree covers

| UniXNU driver | Role | whyred match |
| --- | --- | --- |
| `WhyredSdhci`, `WhyredBlock` | eMMC host and `IOBlockStorageDevice` | same SDCC v5 controller this port already drives (`santoni_sdhci.c`) |
| `WhyredDisplay` | GOP handoff, MDP/DSI panel | same panel path; compare property sets before changing `AppleCLCD` |
| `WhyredGpu` | Adreno 509 clock and chip ID | same a5xx GPU, no firmware/ringbuffer yet |
| `WhyredTouch`, `WhyredInput`, `WhyredHidInput` | Synaptics DSX transport, probe and HID bridge | same controller; the port's `santoni_touch.c` is ahead on the bus experiments |
| `WhyredDsp` | ADSP over GLINK/SMEM | same ADSP; no codec path yet |
| `WhyredPower`, `WhyredPMGR`, `WhyredArmCpu` | PM660/PM660L rails, PMGR clock/reset, CPU/GIC/timer | same PMIC and clock tree |
| `WhyredKeyStore` | key-store event bridge | board-independent glue |

There is no Wi-Fi driver in the UniXNU tree yet, so `drivers/wifi/` still
references the topaz modem path in this repository.
