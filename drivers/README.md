# drivers/

One folder per subsystem, so the port's driver work is still findable after the
patch list grows past 0173.

## Rule

The kernel is built from a patched XNU tree, so driver *code* must live at a
path XNU already builds (for example `bsd/dev/santoni_sdhci.c`) and reach the
tree through a numbered patch in `xnu/patches/`. The folder here is the index
and the place for anything that is not compiled into XNU (register tables,
firmware notes, host-side test vectors).

Each domain folder records:

- the exact hardware block and its address/IRQ from the whyred DTS,
- the patch and in-tree file that implement it,
- what is proven on the phone and what is not,
- the next experiment, if the domain is still untouched.

## Domains

| Folder | Hardware | State |
| --- | --- | --- |
| `storage/` | SDCC v5 eMMC at `0x0c0c4000` | working (`0142`+) |
| `display/` | `AppleCLCD` + ABL framebuffer at `0x9d400000` | working, console only |
| `gpu/` | Adreno 509 (`0x05000900`) | not started |
| `wifi/` | WCN3990 (SNOC) + Bluetooth over SLIMbus | not started |
| `touch/` | Novatek NT36672 (I2C) | not started |
| `audio/` | Qualcomm WCD codec on SLIMbus + ADSP | not started |
| `power/` | PM660/PM660L charger and fuel gauge | not started |

Add a new folder with a `README.md` before writing the first line of a new
driver; keep the folder name lowercase and the hardware name in the file.
