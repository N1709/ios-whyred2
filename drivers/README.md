# drivers/

One folder per subsystem. Each holds the code this port actually runs (mirrored
from `xnu/patches/`) and, where useful, driver source borrowed from another
port as a reference.

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

The `*/hakios/` folders are different: they are read-only references copied
from the HakiOS project (see `THIRD_PARTY.md`). Nothing there is compiled;
they are kept byte-for-byte so a whyred driver can be written by adapting a
proven implementation instead of starting from no code.

## Domains

| Folder | Hardware | Mirrored source | Borrowed reference | State |
| --- | --- | --- | --- | --- |
| `storage/` | SDCC v5 eMMC `0x0c0c4000` | `santoni_sdhci.c` | - | working (`0142`+) |
| `display/` | `AppleCLCD` + ABL framebuffer `0x9d400000` | `SweetFramebuffer.cpp`, `SweetSurface.cpp` | `hakios/MiniIOMobileFramebuffer.cpp` | working, console only |
| `gpu/` | Adreno 509 (`0x05000900`) | none in tree | `hakios/` a660 driver + DRM bridge + IOSurface | not started |
| `wifi/` | WCN3990 (SNOC) + BT SLIMbus | `TopazModem.cpp` (topaz reference) | `hakios/` WCN6750 driver + `lx` Linux-host layer | not started |
| `touch/` | Novatek NT36672 (I2C) | `sweet_touch.c` (FT5452/Goodix reference) | `hakios/` Goodix/FT driver + input queue | not started |
| `audio/` | WCD9335 + ADSP | none in tree | `hakios/` ADSP + `IOAudioFamily` path | not started |
| `power/` | PM660/PM660L charger and gauge | `SweetBattery.cpp`, `sweet_i2c1.c` | `hakios/` `IOPMPowerSource` + PMIC | not started |

Hardware facts for the domains with no code yet are in `gpu/hw.md` and
`audio/hw.md`; the whyred DTS values come from `refs/whyred-android/`.

## Before writing a new driver

1. Read the domain README, then `THIRD_PARTY.md` for the hardware match.
2. Check whether the reference is for the same silicon. An a660 register or a
   WCN6750 firmware name on whyred is a boot failure, not a shortcut.
3. Write the driver at a path XNU builds, through a numbered patch, and mirror
   it with `tools/extract_drivers.py`. Leave the `hakios/` copy untouched.
