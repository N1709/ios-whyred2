# WhyredDisplay

State: **adapted source**. `native/` holds the native UniXNU display stack
(`NativeMobileDisplay`, `NativeLegacyDisplay`, `NativeGOPPresenter`) from
`qine233/UniXNU-clover` (commit `4c5652e`, BSD-2-Clause-Patent,
`native/LICENSE`). It reads the GOP framebuffer dimensions from the firmware
console, so the 1080x2160 mode needs no code change; the DSI/MDP panel path is
still planned. It is not a build input yet.

## Two stages

1. **Firmware framebuffer.** The vendor bootloader leaves a contiguous buffer at
   `0x9d400000` (`cont_splash_region`, 36 MiB). The first UniXNU milestone
   presents through the firmware GOP and does not touch MDP or DSI.
2. **Native panel.** 1080x2160 panel behind the Qualcomm MDP/DSI block
   (MDP `0xc900000`, DSI controller `0xc994000`). Vendor panel candidates:
   `td4310_tianma`, `nt36672_tianma`, `nt36672_huaxing`, `td4310_shenchao`.

The panel model is not confirmed from a live device yet. Confirm it before
writing panel init data.

## Why the framebuffer first

Adreno 509 has no native XNU driver, and the vendor MDP5/DSI stack is not the
SM8550 SDE/DSI path used by the newer components. A working GOP handoff keeps
display out of the first boot's critical path.

## Contract

- Framebuffer: `0x9d400000` + 36 MiB, already reserved in `dts/whyred.dts`.
- Mode: 1080x2160, 32-bit (stride 4320 bytes, 9,331,200 bytes total).
- Keep the splash reservation out of XNU allocations.

## Host model verification

`native/tests/whyred_display_host.cpp` verifies:
- Framebuffer geometry fits inside the 36 MiB (`0x9d400000 + 0x02400000`) continuous splash memory.
- GOP wire ABI structs (`NGPMode`, `NGPPresent`, `NGPTransfer`, `NGPWait`, `NGPSwap`, `NGPComplete`) match exact byte layouts.
- 4-bar test pattern generation (`0xffe02020`, `0xff20e020`, `0xff2020e0`, `0xffe8e8e8`) from earlier whyred XNU testing (`sweetfbtest=1`).
- NativeLegacyDisplay request layer states, single/blank layer swaps, pixel filling, and frame scan accounting.
