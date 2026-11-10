# drivers/

One folder per subsystem. Each holds the code this port actually runs (mirrored
from `xnu/patches/`) and, where useful, the matching driver from the UniXNU
whyred port as a reference.

## Why there are two kinds of copy

XNU only builds files at paths it knows, and this port changes XNU through
`xnu/patches/*.patch`. The patch is therefore the canonical source. Editing a
mirror under `drivers/` would be lost on the next build.

`tools/extract_drivers.py` reconstructs every in-tree driver file from the
patch series and writes the mirror here:

```
python3 tools/extract_drivers.py          # refresh the mirrors
python3 tools/extract_drivers.py --check  # verify they match the patches
```

`tests/run_tests.sh` runs `--check`, so a patch and its mirror cannot drift.

The `unixnu/` tree is different: it is a read-only reference copied from the
`UniXNU-whyred` device repository (`drivers/`), byte-for-byte. Nothing there is
compiled; it is kept so a whyred driver can be adapted from the port that
already targets the same board instead of starting from no code. See
`THIRD_PARTY.md` for provenance and licensing.

## Domains

| Folder | Hardware | Mirrored source | UniXNU reference | State |
| --- | --- | --- | --- | --- |
| `storage/` | SDCC v5 eMMC `0x0c0c4000` | `santoni_sdhci.c` | `unixnu/WhyredSdhci`, `unixnu/WhyredBlock` | working (`0142`+) |
| `display/` | `AppleCLCD` + ABL framebuffer `0x9d400000` | `SweetFramebuffer.cpp`, `SweetSurface.cpp` | `unixnu/WhyredDisplay` | working, console only |
| `gpu/` | Adreno 509 (`0x05000900`) | none in tree | `unixnu/WhyredGpu` | not started |
| `wifi/` | WCN3990 (SNOC) + BT SLIMbus | `TopazModem.cpp` (topaz reference) | none yet | not started |
| `touch/` | Synaptics DSX / Novatek NT36672 (I2C) | `santoni_touch.c`, `sweet_touch.c` | `unixnu/WhyredTouch`, `unixnu/WhyredInput`, `unixnu/WhyredHidInput` | probing (`0184`+) |
| `audio/` | WCD9335 + ADSP | none in tree | `unixnu/WhyredDsp` | not started |
| `power/` | PM660/PM660L charger and gauge | `SweetBattery.cpp`, `sweet_i2c1.c` | `unixnu/WhyredPower`, `unixnu/WhyredPMGR`, `unixnu/WhyredArmCpu` | not started |

Hardware facts for the domains with no code yet are in `gpu/hw.md` and
`audio/hw.md`; the whyred DTS values come from `refs/whyred-android/`.

## Before writing a new driver

1. Read the domain README, then `THIRD_PARTY.md` for the hardware match.
2. Check whether the reference addresses the same silicon. whyred shares the
   SDM660/SDM636 family with the UniXNU drivers, but every register and rail
   name still has to be checked against `dts/whyred.dts`.
3. Write the driver at a path XNU builds, through a numbered patch, and mirror
   it with `tools/extract_drivers.py`. Leave the `unixnu/` copy untouched.
