# Driver sources

Board-specific driver work for whyred, one directory per driver, following the
driver tree used by the clover port. Each directory carries a README with the
hardware contract, the verified facts and the next milestone.

These are **staged sources**, not yet build inputs. In format 2 the resolver
selects drivers from `UniXNU/components.toml` and the board's
`[components] include`; a new driver becomes selectable when it is published as
a component repository and added to the catalog. Until then, code that must ship
inside the boot kit goes to `package/`, which becomes `kits/whyred` in the
assembled tree. Keep both in sync when a driver is promoted.

Every driver from the SDM660 `qine233/UniXNU-clover` tree is present here.

| Driver | Role | clover source | State |
| --- | --- | --- | --- |
| `WhyredSdhci` | eMMC host at `0xc0c4000` | `CloverSdhci` | XNU source + native `native/WhyredSdhci.cpp` |
| `WhyredBlock` | `IOBlockStorageDevice` and GPT windows | `CloverBlock` | native `native/WhyredBlock.cpp` |
| `WhyredDisplay` | GOP handoff, then MDP/DSI panel | `NativeMobileDisplay` | native `native/` |
| `WhyredTouch` | Synaptics DSX on BLSP1 QUP1 | — | XNU source `WhyredTouch.c` |
| `WhyredInput` | native touch transport and probe | `CloverInput` | native `native/WhyredInput.cpp` |
| `WhyredHidInput` | HID device bridge | `NativeHidInput` | native `native/NativeHidInput.cpp` |
| `WhyredPower` | PM660/PM660L rails | `CloverPower` | native `native/WhyredPower.cpp` |
| `WhyredPMGR` | `ApplePMGR` nub and clock/reset scheduling | `CloverPMGR` | native `native/WhyredPMGR.cpp` |
| `WhyredDsp` | ADSP over GLINK/SMEM | `CloverDsp` | native `native/WhyredDsp.cpp` |
| `WhyredGpu` | Adreno 509 clock and chip ID | `CloverGpu` | native `native/WhyredGpu.cpp` |
| `WhyredKeyStore` | key-store event bridge | `NativeKeyStoreEvents` | native `native/NativeKeyStoreEvents.cpp` |
| `WhyredArmCpu` | SDM660 CPU/GIC/timer override | `ArmCpu` | reference `native/ArmCpu.cpp` (catalog `ArmCpu` is the build input) |

Two source families are staged here:

- `Whyred*.c` came from the earlier whyred XNU port and records what already
  ran on the phone.
- `native/` came from the SDM660 `qine233/UniXNU-clover` drivers (commit
  `4c5652e`, BSD-2-Clause-Patent). Classes, symbols and boot arguments are
  renamed to whyred. The constants already changed for whyred are listed in
  each `README.md`; everything else is still clover's until checked against
  `dts/whyred.dts` and `soc/sdm660`.

Neither family enters the build until the promotion checklist is done.

Reusable drivers belong in their own component repository. A directory here is
whyred-specific wiring: addresses, panel, touch controller, rail names and the
board's own timing data.

## Promotion checklist

1. Move the directory to a new `UniXNU-<Driver>` repository; keep this README as
   the board contract.
2. Add the component to `UniXNU/components.toml` with its canonical ID,
   dependencies, repository and revision.
3. Add the ID to `board.toml` `[components] include`.
4. Remove the staged copy here, or keep it as a `local.toml` override during
   development.
5. Record the change in `porting.toml` and rerun `scripts/check-whyred.sh`.
