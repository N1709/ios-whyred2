# WhyredAudio

Source driver: `WhyredDsp` (ADSP over GLINK/SMEM).

State: **adapted source**. `native/WhyredDsp.cpp` (with `DspRegs.h` and
`DspCore.h`) is the native UniXNU ADSP driver, ported from
`qine233/UniXNU-clover` (commit `4c5652e`, BSD-2-Clause-Patent,
`native/LICENSE`). It is not a build input yet.

## Hardware contract

- ADSP over GLINK, with the shared heap at SMEM `0x86000000` + 2 MiB.
- Doorbell word at `0x17911008` (SMP2P ADSP).
- The board DTS routes audio (APR) and the WCN3990 control path through the same
  blocks.

## Whyred status

The clover constants already match whyred: SMEM `0x86000000`, its 2 MiB span,
and the `0x17911008` doorbell are the SDM660/636 values, not board-specific.
The read-only first mode applies unchanged.

## First milestone

Map the shared heap, read the ADSP state and the GLINK/SMP2P handshake, and log
what firmware left behind. No write, no firmware load.

## Still open

The audio codec on whyred is a WCD9335 over SLIMbus; clover's codec path is
different. Bring-up stays read-only until the ADSP is confirmed.
