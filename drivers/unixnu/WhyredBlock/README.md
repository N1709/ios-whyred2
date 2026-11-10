# WhyredBlock

State: **adapted source**. `native/WhyredBlock.cpp` is the native UniXNU
`IOBlockStorageDevice` driver, ported from `qine233/UniXNU-clover` (commit
`4c5652e`, BSD-2-Clause-Patent, `native/LICENSE`). The class names and boot
arguments are whyred's; the clover README in `native/` keeps the original
design notes. It is not a build input yet.

## Role

Publish the eMMC as an `IOBlockStorageDevice` so `IOStorageFamily` and
`IOGUIDPartitionScheme` do the GPT work, then expose the `userdata` windows to
the kernel. This is the XNU-side half of the storage bring-up; `WhyredSdhci`
owns the host.

## Contract

- Backing card: eMMC behind `sdhc_1`, 4 KiB logical blocks.
- `userdata`: first LBA `0x2b8c00`, last LBA `0x1d7d6ff`, 109643 MiB.
- The OS and SystemOS windows must stay inside that entry.
- The backup GPT is after `last_lba` and must remain untouched.
- The previous whyred XNU storage read returned exactly those numbers, which is
  why they are in `board.toml` and this README.

## First milestone

Read the GPT header and entries through the host, verify both CRCs, print the
capacity and the `userdata` extent, and refuse to publish anything if the card
does not match the board contract. Writes come later, after the read-only boot
is stable.
