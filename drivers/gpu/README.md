# gpu

- **Hardware:** Adreno 509, `qcom,chipid = <0x05000900>`
  (`refs/whyred-android/dts_sdm636_whyred.dtsi`), GPU clock controller at
  `0x05065000` and `gpucc` at `0x05090000` on the SDM660/636 DTS.
- **Code:** none. This port draws only through `AppleCLCD`/`sweetfb`.
- **Not started:** no IOAccelerator, no firmware, no ringbuffer.
- **Next, in order:**
  1. Copy the Adreno firmware out of the phone's vendor partition and record
     the exact file names in this folder.
  2. Read the `msm_gpu`/`kgsl` nodes and the `a5xx` register layout from the
     whyred Android kernel (`refs/whyred-android/`) before writing IOKit code.
  3. Decide whether a minimal `IOAccelerator` that only satisfies the
     `IOSurface` users is enough for SpringBoard first; full 3D is a later,
     much larger project.

Borrowed reference: `hakios/` from HakiOS (small file set, read
`../THIRD_PARTY.md` first):
- `hakios.gpu.cpp` - the chip-independent DRM/msm UAPI bridge Mesa's freedreno
  speaks; reusable as the shape of our IOAccelerator user client;
- `hakios.adreno.c` - Adreno **642L (a660)** bring-up: GMU, zap shader,
  SMMU and TrustZone. whyred is **Adreno 509 (a5xx)**, so the register map and
  firmware names do not carry over; the step order does;
- `MiniIOSurface.cpp/.h` - another IOSurface stand-in to compare with
  `SweetSurface.cpp` before writing ours.
