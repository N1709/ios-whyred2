# WhyredPMGR

State: **adapted source**. `native/WhyredPMGR.cpp` is the native UniXNU
`ApplePMGR` platform driver from `qine233/UniXNU-clover` (commit `4c5652e`,
BSD-2-Clause-Patent, `native/LICENSE`). It is not a build input yet.

## Role

Publish the `ApplePMGR` nub and schedule the board drivers (`Sdhci`, `Input`,
`Power`) from the `bsd_init` hook. The device repository has no XNU patch slot
for an IOKit personality, so each board driver exports a C entry point and this
file calls it.

## Whyred status

The class, symbols and boot arguments are renamed to whyred. The scheduling
order and the PMGR slot numbers are the SDM660 shape and are kept; the per-board
driver list has to match the drivers that whyred actually enables.

## First milestone

Register the nub and print the driver schedule without calling a driver that is
not built yet. The storage driver is the first caller.

## Still open

Which clock/reset operations the whyred platform needs, and whether RPM or
direct GCC writes own them. Decide that from the vendor DTS before adding a
handler.
