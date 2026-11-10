# drivers/

Two kinds of source live here:

- **Domain mirrors** (`storage/`, `display/`, `gpu/`, `wifi/`, `touch/`,
  `audio/`, `power/`) hold the code this port actually runs, reconstructed
  from `xnu/patches/`, plus the hardware notes for each subsystem.
- **Whyred drivers** (`Whyred*/`) are the board drivers from the UniXNU-whyred
  tree: `WhyredStorage`, `WhyredBlock`, `WhyredDisplay`, `WhyredGpu`,
  `WhyredTouch`, `WhyredInput`, `WhyredHidInput`, `WhyredAudio`, `WhyredPower`,
  `WhyredPMGR`, `WhyredCpu` and `WhyredKeyStore`. They are not build inputs
  here yet; the domain README says which one matches its hardware.

## The patch series is canonical

XNU only builds files at paths it knows, and this port changes XNU through
`xnu/patches/*.patch`. Editing a mirror under `drivers/` would be lost on the
next build.

`tools/extract_drivers.py` reconstructs every in-tree driver file from the
patch series and writes the mirror here:

```
python3 tools/extract_drivers.py          # refresh the mirrors
python3 tools/extract_drivers.py --check  # verify they match the patches
```

`tests/run_tests.sh` runs `--check`, so a patch and its mirror cannot drift.
The `Whyred*/` directories are kept as the port's own driver sources; when one
becomes a build input it arrives through a numbered patch like every other
driver.

## Domains

| Folder | Hardware | Mirrored source | Whyred driver | State |
| --- | --- | --- | --- | --- |
| `storage/` | SDCC v5 eMMC `0x0c0c4000` | `santoni_sdhci.c` | `WhyredStorage`, `WhyredBlock` | working (`0142`+) |
| `display/` | `AppleCLCD` + ABL framebuffer `0x9d400000` | `SweetFramebuffer.cpp`, `SweetSurface.cpp` | `WhyredDisplay` | working, console only |
| `gpu/` | Adreno 509 (`0x05000900`) | none in tree | `WhyredGpu` | not started |
| `wifi/` | WCN3990 (SNOC) + BT SLIMbus | `TopazModem.cpp` (topaz reference) | none yet | not started |
| `touch/` | Synaptics DSX / Novatek NT36672 (I2C) | `santoni_touch.c`, `sweet_touch.c` | `WhyredTouch`, `WhyredInput`, `WhyredHidInput` | probing (`0184`+) |
| `audio/` | WCD9335 + ADSP | none in tree | `WhyredAudio` | not started |
| `power/` | PM660/PM660L charger and gauge | `SweetBattery.cpp`, `sweet_i2c1.c` | `WhyredPower`, `WhyredPMGR`, `WhyredCpu` | not started |

Hardware facts for the domains with no code yet are in `gpu/hw.md` and
`audio/hw.md`; the whyred DTS values come from `refs/whyred-android/`.

## Before writing a new driver

1. Read the domain README, then `THIRD_PARTY.md` for the hardware match.
2. Check whether the reference addresses the same silicon. whyred shares the
   SDM660/SDM636 family with the Whyred drivers, but every register and rail
   name still has to be checked against `dts/whyred.dts`.
3. Write the driver at a path XNU builds, through a numbered patch, and mirror
   it with `tools/extract_drivers.py`.
