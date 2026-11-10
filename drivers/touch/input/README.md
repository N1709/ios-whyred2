# Input

State: **adapted source**. `native/Input.cpp` is the native UniXNU touch
probe, ported from the clover `CloverInput` driver
(`qine233/UniXNU-clover`, commit `4c5652e`, BSD-2-Clause-Patent,
`native/LICENSE`). It is not a build input yet.

## Hardware contract

- Touch controller: Synaptics DSX (RMI4) at I2C `0x20`. Some units run a
  Novatek part that answers `0x62` in bootloader mode and `0x01` after firmware
  load; the probe covers all three addresses.
- Bus: BLSP1 QUP1 at `0xc175000`, GIC SPI 95 (`INTID 127`).
- Pins: SDA GPIO 2, SCL GPIO 3 (TLMM SOUTH tile, `0x03000000`); reset GPIO 66,
  IRQ GPIO 67 (TLMM NORTH tile, `0x03900000`).
- Rail: `vcc_i2c` is `pm660_l11`; it also powers the I2C pull-ups.
- board has no external pull resistors on the bus.

##  constants already applied

| Constant | clover | board |
| --- | --- | --- |
| `kQupPhys` | `0x0c177000` | `0x0c175000` |
| `kQupIntid` | `129` (SPI 97) | `127` (SPI 95) |
| SDA/SCL | GPIO 10/11 | GPIO 2/3 |
| reset / IRQ | GPIO 66/67 | GPIO 66/67 |
| candidates | Goodix `0x5d`/`0x14`, FocalTech `0x38` | Synaptics `0x20`, Novatek `0x62`/`0x01` |

The transport and pin plumbing are board's now. The candidate protocol tables
unify both panel touch controller families: Synaptics DSX (RMI4/RMI5) at `0x20` and
Novatek NT36672 at `0x62`/`0x01`.

## Host model verification

`native/tests/board_input_host.cpp` verifies:
- Bus and pin mapping: TLMM south GPIO 2/3 (SDA/SCL), north GPIO 66/67 (RST/IRQ), BLSP1 QUP1 `0x0c175000`, INTID 127 (`SPI 95`).
- Synaptics DSX (RMI4/RMI5) PDT parsing: F01 device control, F12 multi-touch data base `0x06`, F34 flash function detection.
- Product ID string extraction (`kDsxProductIdOffset = 11`, `kDsxProductIdSize = 10`).
- Multi-touch F12 packet decoding (`input_dsx_parse_f12_report`): single and multi-finger contact tracking with stable slot IDs.
- Touch coordinate mapping and clamping to the board 1080x2160 panel (`input_dsx_touch_map_coords`).
- Novatek NT36672 chip ID validation and distinction between bootloader mode (`0x62`) and normal runtime mode (`0x01`).
- Hardware reset timings: 20 ms active low, 200 ms delay, 50 ms settle.
- QUP v2 tag generation: page select write tags, repeated start register read tags.

## First milestone

Probe `0x20` and `0x62`/`0x01`, read the PDT or chip ID, and log the controller
identity. Touch reporting is mapped to 1080x2160 panel coordinates.
