# WhyredTouch

State: **staged source**. `WhyredTouch.c` is the step-1 touch probe from the
earlier whyred XNU port: TLMM bit-banged I2C, bus scan, and the Synaptics RMI4
page-description read. It is not a build input yet (see `../README.md`).

## Hardware contract

- Controller: Synaptics DSX (RMI4) at I2C address `0x20`. Some units answer a
  Novatek part at `0x62` while its bootloader runs and `0x01` after firmware
  load; the scan covers both.
- Bus: BLSP1 QUP1 at `0xc175000`, GIC SPI 95.
- Pins: SDA GPIO 2, SCL GPIO 3 (TLMM SOUTH tile, `+0x100000`); reset GPIO 66,
  IRQ GPIO 67 (TLMM NORTH tile, `+0x900000`).
- Rail: `vcc_i2c` is `pm660_l11`; it also powers the I2C pull-ups.
- whyred has no external pull resistors on the bus.

## Verified from the previous port

- `/dev/input/event2` captured `ABS_MT_POSITION_X 0x1e9`,
  `ABS_MT_POSITION_Y 0x78a` and `BTN_TOUCH` down/up.
- The full PDT decoded as F34, F01 and F12, with F12 data base `0x06`.
- Unbinding the touch device disabled `vcc_i2c` and the pull-ups; reads then
  floated at `0xff`. Keep the rail on for the whole test.

## Source

`WhyredTouch.c` came from the previous whyred XNU port
(`ios-whyred2/drivers/touch/`). Its guard is `ARM64_BOARD_CONFIG_WHYRED`.
Boot arguments:

| Arg | Effect |
| --- | --- |
| `sweetouchi2c=1` | run the probe and read the chip ID bytes |
| `sweetouchreset=1` | drive reset GPIO 66: low 20 ms, high 200 ms, 50 ms settle |
| `sweetouchwait=SECONDS` | hold the panel with the result (default 15 s) |
| `sweetouchhalt=1` | paint the answer into the scanout buffer and stop |

The `sweetouch*` argument names are historical; keep them until the driver is
promoted so old boot logs stay comparable.

## Open question

A same-register read stream returned the PDT instead of repeating the entry.
The reason is not known; the driver must not assume the controller is returning
stale data until that is explained.

## First milestone

Enable the rail, probe `0x20`, read the PDT once, and log the page description
without writing configuration. Touch reporting comes after the read path is
stable.
