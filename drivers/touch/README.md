# touch

- **Hardware:** Novatek NT36672 touch controller, two panel variants in the
  whyred DTS (`tianma` and `huaxing` FHD video panels). I2C address, interrupt
  and reset GPIOs are in the full whyred panel DTS, not in the board overrides
  mirrored under `refs/whyred-android/`.
- **Code:** none.
- **Not started:** no I2C controller driver attach, no HID/`IOHIDDevice`
  publication.
- **Next, in order:**
  1. Read the panel DTS and record the I2C bus, address, IRQ and reset GPIO in
     this file.
  2. Bring up the QUP I2C controller and prove a register read from the
     controller with a kernel print.
  3. Only then publish an `IOHIDDevice`; iOS needs the multitouch report
     descriptor to match what SpringBoard expects.

Mirror: `sweet_touch.c`. This is the FT5452/Goodix GENI driver used by the
sweet/topaz boards, **not** an NT36672 driver - it is here as a reference for
the attach/IRQ/report structure.
