# power

- **Hardware:** PM660/PM660L PMIC pair (charger, fuel gauge, regulators), plus
  PS_HOLD at `0x010ac000` which the shim already uses for reset.
- **Code:** none. The boot command line sets `sweetbattery=0`, so no charger or
  gauge driver runs.
- **Not started:** charger detection, fuel gauge, thermal mitigation, battery
  reporting to `IOPMrootDomain`.
- **Next:** not on the critical path for the lock screen; pick this up after
  SpringBoard, when the device must stay alive on battery.

Mirrors: `SweetBattery.cpp` and `sweet_i2c1.c` from the sweet/topaz power path;
the PM660 register programming still has to come from the whyred Android
sources.

Borrowed reference: `hakios/` from HakiOS (read `../THIRD_PARTY.md` first).
`hakios.battery.cpp` is a working `IOPMPowerSource` that powerd accepts, so it
is the shape our PM660 source should feed; `hakios.battery.c` itself talks to
a PM7325B through the ADSP and does not apply. `hakios.cpufreq.c` is SM7325 /
RPMh, not SDM636 / RPM.
