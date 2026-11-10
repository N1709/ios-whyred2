# WhyredKeyStore

State: **adapted source**. `native/NativeKeyStoreEvents.cpp` is the native
UniXNU key-store event bridge from `qine233/UniXNU-clover` (commit `4c5652e`,
BSD-2-Clause-Patent, `native/LICENSE`). It is not a hardware driver and has no
board constant. It is not a build input yet.

## Role

Deliver key-store/effaceable events to the kernel side of the boot chain so the
OS can persist secrets. On whyred this only becomes relevant after the storage
read path is stable; it must never write outside the `userdata` window.

## First milestone

Compile it against the whyred XNU build and confirm it registers without a
board-specific personality. Pair it with the read-only storage milestone.
