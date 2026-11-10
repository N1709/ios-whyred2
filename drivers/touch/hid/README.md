# HidInput

State: **adapted source**. `native/NativeHidInput.cpp` is the generic native
UniXNU HID input driver from `qine233/UniXNU-clover` (commit `4c5652e`,
BSD-2-Clause-Patent, `native/LICENSE`). It contains no clover hardware
constant; it publishes the input device that a board touch driver feeds. It is
not a build input yet.

## Role

Bridge the board touch driver's reports to `IOHIDFamily`, so userland sees a
normal touchscreen. The Synaptics/Novatek probe itself lives in
`../Input` and `../Touch`.

## First milestone

Load it together with `Input` once the touch read path is stable, and
confirm that a contact shows up as a HID device without a board-specific
personality.
