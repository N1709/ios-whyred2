# WhyredCpu

Source driver: `WhyredArmCpu` (SDM660 CPU/GIC/timer override).

State: **reference source**. `native/ArmCpu.cpp`, `Info.plist` and `README.md`
are the SDM660 `ArmCpu` override from `qine233/UniXNU-clover` (commit
`4c5652e`, BSD-2-Clause-Patent, `native/LICENSE`).

whyred currently selects the catalog component `ArmCpu` at `93a5030` through
`UniXNU/components.toml`; that is the build input. This copy is kept because it
is the same SDM660 family and carries the GIC/timer/SMP work that a whyred
override would start from. Do not add it to `board.toml` without a diff against
the catalog revision.

## What it covers

- CPU platform device, GICv3 redistributor and architected timer.
- PPI numbering: whyred's DTS lists timer PPIs as 1/2/3/0 (INTIDs 17/18/19/16),
  the same numbering clover uses, so the shipped `armcpu-timer-intid=19` is the
  value to compare first.
- Optional SMP, self-IPI and SPI-IPI paths, all behind boot arguments.

## Next

Diff this file against the catalog `ArmCpu` component before deciding whether
whyred needs an override. Keep the comparison in `porting.toml` under `cpu`.
