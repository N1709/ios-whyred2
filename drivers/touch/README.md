# touch

- **Hardware:** Novatek NT36672 touch controller on the `tianma` and `huaxing`
  FHD video panels; some whyred units ship a Shenchao panel with a Synaptics
  controller. whyred has no external pull resistors on the bus.
- **Bus:** BLSP1 QUP1 (`0x0c175000`, GIC SPI 95), SDA GPIO 2, SCL GPIO 3
  (TLMM SOUTH tile, physical `0x03102000` / `0x03103000`).
- **TLMM:** `pinctrl@03000000` (vendor DTS); SOUTH tile at `+0x100000`,
  NORTH tile at `+0x900000`
- **Sideband:** reset GPIO 66, IRQ GPIO 67 (TLMM NORTH tile, physical
  `0x03942000` / `0x03943000`), VDDIO PM660 LDO11.
- **Addresses:** Novatek answers at `0x62` while its bootloader runs and at
  `0x01` once firmware runs; Synaptics answers at `0x20`.
- **Code:** `santoni_touch.c` (patch 0184) is step 1 only: TLMM GPIO plus
  bit-banged I2C at ~50 kHz, a full address scan and a Novatek page-0
  chip-ID read. It does not publish `IOHIDDevice` and does not use the QUP.

Boot-args:

| Arg | Effect |
| --- | --- |
| `sweetouchi2c=1` | run the probe (scan `0x08`..`0x77`, read the three chip-ID bytes of every address that ACKs) |
| `sweetouchreset=1` | additionally drive reset (GPIO 66) low 10 ms and high 60 ms first |

Without `sweetouchreset` the probe never writes GPIO 66/67, so the log also
reports the state ABL handed over (`rst cfg/io`, `irq cfg/io`).

What the log says:

- `id 0x01 36 xx xx` / `id 0x62 36 xx xx` - controller alive, bus and pull-ups
  good. Next: QUP driver for polling, then the HID report path.
- `0 address(es) acknowledged` with `sda 1 scl 1` - bus is idle and pulled up
  but the controller is silent: power (PM660 LDO11) or reset is the suspect.
- `0 address(es) acknowledged` with `sda 0` - something holds the bus low;
  the nine-clock recovery line appears when that was tried.

Order for the next steps:

1. Prove the chip answers (0184).
2. Read the report descriptor / point data over the bit-banged bus and decode
   one finger, with the same `SweetHIDTouch::touch()` entry point 0183 uses.
3. Only then replace polling with the QUP1 engine if the frame rate needs it.

Mirror: `sweet_touch.c`. This is the FT5452/Goodix GENI driver used by the
sweet/topaz boards, **not** an NT36672 driver - it is here as a reference for
the attach/IRQ/report structure.

Borrowed reference: `hakios/` from HakiOS (read `../THIRD_PARTY.md` first).
- `hakios.input.c/.h` - a small `/dev/hakios-touch` queue that hands touch
  samples to user space; useful if publishing an `IOHIDDevice` for NT36672
  turns out to be more than SpringBoard needs.
- `hakios.goodix.c` - Goodix GT9916 / FocalTech FT3680 on a GENI **SPI** bus.
  NT36672 is on **I2C** and has a different report protocol, so this is a
  reference for power sequencing, polling and report decoding only.
