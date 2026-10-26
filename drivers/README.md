# drivers/

One folder per subsystem: the mirrored driver source plus the hardware notes
that are not compiled into XNU.

## Why there are two copies

XNU only builds files at paths it knows, and this port changes XNU through
`xnu/patches/*.patch`. The patch is therefore the canonical source. Editing a
copy under `drivers/` would be lost on the next build.

`tools/extract_drivers.py` reconstructs every driver file from the patch series
and writes the mirror here, so the code can be read without checking out XNU:

```
python3 tools/extract_drivers.py          # refresh the mirrors
python3 tools/extract_drivers.py --check  # verify they match the patches
```

`tests/run_tests.sh` runs `--check`, so a patch and its mirror cannot drift.

## Domains

| Folder | Hardware | Mirrored source | State |
| --- | --- | --- | --- |
| `storage/` | SDCC v5 eMMC `0x0c0c4000` | `santoni_sdhci.c` | working (`0142`+) |
| `display/` | `AppleCLCD` + ABL framebuffer `0x9d400000` | `SweetFramebuffer.cpp`, `SweetSurface.cpp` | working, console only |
| `touch/` | Novatek NT36672 (I2C) | `sweet_touch.c` (FT5452/Goodix reference) | NT36672 not started |
| `wifi/` | WCN3990 (SNOC) + BT SLIMbus | `TopazModem.cpp` (topaz WLAN reference) | WCN3990 not started |
| `power/` | PM660/PM660L charger and gauge | `SweetBattery.cpp`, `sweet_i2c1.c` (sweet/topaz path) | not started |
| `gpu/` | Adreno 509 (`0x05000900`) | none in tree | not started, `hw.md` has the facts |
| `audio/` | WCD codec + ADSP | none in tree | not started, `hw.md` has the facts |

The touch, wifi and power mirrors are the code this tree already has for other
boards; they are references for structure, not a whyred implementation.
