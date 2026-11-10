# Driver provenance

The driver folders under `drivers/` hold source from the **UniXNU-whyred**
device repository (`https://github.com/N1709/UniXNU-whyred`, `drivers/`). These
are board drivers for the same SDM660/SDM636 family, staged there before
promotion into UniXNU components.

| | |
| --- | --- |
| Source | `UniXNU-whyred/drivers` |
| Upstream of the `native/` sources | `qine233/UniXNU-clover`, commit `4c5652e`, BSD-2-Clause-Patent |
| Licence text | one `LICENSE` per `native/` directory |

## These files are not build inputs here

Nothing under a driver subfolder is compiled in this repository. The kernel
build only sees the patched XNU tree, so a driver that actually runs must
arrive as a numbered patch in `xnu/patches/` and be mirrored by
`tools/extract_drivers.py`.

If a file here is adapted into the boot kernel, the adapted code goes into a
patch; the driver source stays as the board's record.

## What the drivers cover

| Folder | Role | hardware match |
| --- | --- | --- |
| `audio/dsp` | ADSP over GLINK/SMEM | same ADSP; no codec path yet |
| `gpu/adreno` | Adreno 509 clock and chip ID | same a5xx GPU, no firmware/ringbuffer yet |
| `touch/dsx`, `touch/input`, `touch/hid` | Synaptics DSX transport, probe and HID bridge | same controller; the port's `santoni_touch.c` is ahead on the bus experiments |
| `power/pmic` | PM660/PM660L rails and `IOPMPowerSource` | same PMIC |
| `power/pmgr` | PMGR clock and reset scheduling | same clock tree |
| `power/cpu` | CPU/GIC/timer override | same SoC |
| `key-store/keystore` | key-store event bridge | board-independent glue |

Display and storage have no driver folder: this port uses the kernel's
built-in `AppleCLCD`/`sweetfb` and `santoni_sdhci.c` instead. There is also no
Wi-Fi driver in the UniXNU tree yet, so `drivers/wifi/` still refers to the
topaz modem path in this repository.
