# ios-whyred

**Apple's iOS 16 running on a Redmi Note 5 / Note 5 Pro.**

This is a port of Apple's open-source XNU kernel (xnu-8796) to the Xiaomi **Redmi Note 5 / Note 5 Pro** (`whyred`, Qualcomm **SDM636** / Snapdragon 636). On top of it runs the unmodified iOS 16.7.16 (20H392) userland of the iPhone X: launchd, the system daemons, backboardd and SpringBoard.

The same tree still builds the Redmi Note 7 (`lavender`, SDM660) target for comparison. WHYRED inherits the LAVENDER board code because the two SoCs share the GICv3, timer, SDCC v5 and continuous-splash layout; the panel is 1080x2160 instead of 1080x2340.

> Experimental bring-up. It installs into the `userdata` partition and so **erases Android's user data**. Nothing is flashed to the boot partitions; the kernel is started from RAM with `fastboot boot`.

## What works

| Area | State |
| --- | --- |
| Boot | Stock Xiaomi ABL → `fastboot boot` of our shim (boot image v0/v1) → XNU. Boot partitions are not touched |
| CPU | The four Cortex-A73 ("Gold", MPIDR `0x100`–`0x103`) cores, 16K pages, GICv3, timers. The Silver cores are parked |
| Memory | DRAM physical base `0x80000000`, safe kernel budget `0xa0000000` + 1 GiB |
| Storage | eMMC 5.1 host controller (Qualcomm SDHC v5 at `0x0c0c4000`, GIC SPI 110) at **8-bit high-speed** (CMD6 BUS_WIDTH=8 + HS_TIMING=1), EXT_CSD valid (122,142,720 sectors = 58.2 GiB), GPT parsed. A 16.0 GiB HFSX volume in `userdata` **mounts read-write** as `emmcroot0`; single-block and multi-block (`CMD18`/`CMD25` + `AUTO_CMD12`) transfers and the HFS+ journal replay have all run on hardware |
| iOS userland | iPhone X 16.7.16 root filesystem on eMMC; `launchd` is loaded from the HFSX root and starts. It is then killed by code signing (`CS_KILLED`), so the root volume is mounted but not yet trusted |
| Display | Kernel IOSurface + IOMobileFramebuffer (`AppleCLCD`); software CoreAnimation into ABL's continuous splash framebuffer at `0x9d400000` (1080×2160 32-bit `a8r8g8b8`). `0177` proves on hardware that MDSS/DSI still scan that memory out after hand-off: four colour bars written by the kernel appear on the panel |
| Touch | Controller **answers at Synaptics `0x20`** on BLSP1 I2C (probe 0184, RMI4 ID 0186); nothing is fed into `SweetHIDTouch` yet, so the lock screen still ignores touch |
| Device identity | iPhone X (`iPhone10,3` / `D22AP`): no Home button, gesture UI |

## Progress

**2026-10-31 - 0189 says the bit-bang cannot address the chip; 0190 moves the reads to the QUP.**
All seven framings 0189 tried came back `streams`: `0x00e9` returned
`12 0a 29 00 8f d4`, then `81 04 03 03 04 05`, then `05 01 01 01 0e 0f` - six
new bytes every time, no matter what address or how many address bytes were
sent. The bit-bang is therefore not fixable by timing: the controller never
sees an address phase at all.

`0190` adds `bsd/dev/sweet_i2c.c`, a BLSP1 QUP1 master, and points the DSX
reads at it behind `sweeti2c=1` (the bit-bang stays as the fallback, including
the address scan, which still finds `0x20`). The register set, clock tree, pin
mux and FIFO protocol were taken from the vendor `i2c-msm-v2.c` rather than
guessed, which corrected four things that would each have wasted a boot:

- `QUP_I2C_MASTER_CONFIG` (0x408) must be set to 1 for the v2 tags; without it
  the SE does not treat `0x8x` bytes as tags.
- a tag is `[START, addr<<1|r/w, tag, len]`, little endian in the FIFO word,
  and the stop bit is the `+1` variant (`0x83`/`0x87`), not the `0x82`/`0x85`
  one - 0189's driver had the two swapped.
- `MX_WRITE_COUNT` counts data + 4 tag bytes and `MX_READ_COUNT` counts data
  + 2 (the input FIFO echoes two tag bytes first), all in FIFO mode with
  `MX_INPUT/OUTPUT_COUNT` zero.
- the RCG is at `0x19024` with the divider in bits 0-4 and the parent in bits
  8-10; the branches enable with bit 17 (`BLSP1_AHB`, 0x52004) and bit 0
  (`I2C_APPS`, 0x19008) and report halt in bit 31.

The driver fills the FIFO in PAUSE, moves to RUN and polls `MAX_INPUT_DONE`,
so a wrong count prints `timeout OPER/ERR/STATUS` instead of hanging. The
per-boot table now also names `QUP1 (hardware I2C)` or `bit-bang` before the
framing matrix, so one ring shows which transport produced the bytes.

**2026-10-30 - the controller was in bootloader mode; 0189 tests the address framing.**
The touch controller had been parked in flash-programming mode, not stripped of
firmware: the page table was `F34 + F01` with no `F12`. A reset cycle after
`ENABLE_FLASH_PROG` boots it back into application firmware, and the Android
recovery driver then works normally again - `input2` with 10 `ABS_MT_SLOT`s and
real `ABS_MT_POSITION_X/Y` events in `getevent`, cleared by a boot of
`fastboot boot` recovery. The firmware was never missing either:
`tools/extract-ts-firmware.py` pulls the 128,512-byte DSX image out of the
recovery kernel, and `tools/sweet-ts-reflash.sh` drives the vendor
`doreflash` sysfs (`echo 2`, not `1`, is FORCE).

XNU still reads the same shifted stream, and 0188's matrix pinned it down: a
read of `0x0000` returns the entry that lives at `0x00e9`
(`27 00 09 00 21 34`), and a second read of the same address returns the next
six bytes, so the address phase never lands and the chip does no more than
stream from wherever its pointer sits. The created tooling is in `NOTES.md`.
`0189` therefore enumerates the framings nobody has tried - 16-bit address
little endian, the page select and the address in one write, the page select
with a two-byte address - and accepts a framing only when it returns the
`0x00e9` entry twice in a row and something else one slot above; the winner is
used for the rest of the probe. It also recognises the `F34 + F01, no F12`
table as bootloader mode and pulses reset once before giving up.

**2026-10-29 - 0187 and 0188: the read is the problem, not the address.**
`0187` replaced the RMI4 read with the Synaptics DSX one the vendor driver
uses (page select `0xFF <page>`, then one address byte, then a repeated
start). The result was still wrong, and the way it is wrong is the useful
part: reading `0x00e9` returns `27 00 09 00 21 34` and then `0x00e3`
continues the same stream with `2f 26 0d 04 01 01` instead of starting a new
page-table entry, and the first slot decodes as function `0x34` - the slot at
`0x00e9` always holds F01. So the address phase is not taking effect at all.
`0188` stops guessing and answers four questions in one boot, read-only:
does reading the same address twice repeat or advance; does a two-byte address
(the 0186 protocol) behave differently; does `0x00e8`/`0x00ea` shift the
answer; and does `0xFF <page>` reach page 1? It also makes the bit-bang
open-drain (release high, wait for the line) so a controller holding SCL low
while it fetches a register is no longer overridden, drives reset with the
timings the DT asks for (20 ms low, 200 ms high), and watches IRQ GPIO 67
during the touch poll: a level change proves the controller is scanning even
if I2C reads stay broken.

**2026-10-29 - whyred's touch controller answers.** Patch `0184` bit-bangs the
BLSP1 I2C bus as plain TLMM GPIO (SDA 2 / SCL 3) and scans every address. The
SYSTEM ring, pulled after a `fastboot boot` of the whyred recovery with
`tools/sweet-touch-answer.sh`, has the answer:

```
WHYRED TOUCH: rst cfg 000003c3 io 00000003; irq cfg 00000001 io 00000001
WHYRED TOUCH: sda2 ctl 000000c3 scl3 ctl 000000c3; idle sda 1 scl 1
WHYRED TOUCH: ack 0x20
WHYRED TOUCH: scan done, 1 address(es) acknowledged; sda 1 scl 1
```

So the controller is powered, out of reset (reset pad is an output driven
high, IRQ is an input idling high) and answering; the "no power / held in
reset" branch is dead. `0x20` is the Synaptics address, so **this unit is not
the Novatek `0x62`/`0x01` variant**. `0185` explains why none of this was
ever visible: the console stops being drawn at the framebuffer hand-off, so
the result is now painted into the scanout buffer instead of printed. `0186`
adds a read-only RMI4 identification pass (version register, function
descriptor table, F01 and F12 query blocks) to learn the product string and
the F12 control/data addresses before anything enables reporting.

Last updated 2026-10-29. **The iOS lock screen is on the panel.** With the
`0182` kernel the phone boots past the data-migration spinner into
SpringBoard's lock screen: the Apple mark holds through the kernel boot, then
the first iOS frame replaces it and the UI stays up (`e77`, first boot of
`0180`+`0181`+`0182`). That closes the original goal of this port - the iOS UI
is visible on whyred's own panel, from RAM, without touching a boot partition.

What the lock screen does **not** have is input. On whyred the HID services are
registered (`SWEET HID: touch registered`, `backboardd` opens
`SweetHIDTouch`) but nothing feeds them: `bsd/dev/sweet_touch.c` and
`bsd/dev/sweet_i2c1.c` are compiled only for `ARM64_BOARD_CONFIG_SWEET`
(topaz), so on whyred `SweetHIDTouch` is an empty digitizer. The facts a
driver needs, recovered from the whyred vendor DTS: touch sits on BLSP1 QUP1
I2C (`0x0c175000`, SDM636), Novatek `0x62` (IRQ GPIO 67, reset GPIO 66,
VDDIO PM660 LDO11) or Synaptics DSX `0x20` depending on the panel variant
(tianma/huaxing vs shenchao). None of the buses that address it exist in this
kernel yet - no TLMM GPIO, no BLSP I2C, no PM660 SPMI on the whyred side.

There is also no GPU driver: `backboardd` composites in software into the
`IOSurface` that `AppleCLCD` scans out, so the UI is correct but slow.
That is a separate project from the one this README tracks (panel + kernel).

`0179` boots the same userspace as `0178`, and the
`SWEET` ring finally shows the presentation path end to end:
`exec backboardd` → `exec SpringBoard` → `backboardd` creates IOSurfaces →
`SWEET FB: display on` → `swap 3/4/7 layer 0 surface 3 1080x2160` → the
lock-screen daemons (`donotdisturbd`, `chronod`, `passd`, `liveactivitiesd`)
start. So SpringBoard is neither dead nor looping; it runs and spawns its
lock-screen services.

The ADT canvas is now the panel: `tools/mkadt.py` published the iPhone X
canvas (1125x2436) unless `--native` is passed, and `shim/build.sh` called it
without that flag. The blob on the phone now carries 1080x2160 for both
`canvas-*` and the `sweet-fbcon` node (verified byte-wise: the old blob holds
1125/2436, the new one only 1080/2160). Hardware shows the limit of that fix:
`backboardd` still creates `id 2 1125x2436` next to `id 3 1080x2160`. Those
1125 surfaces come from userland (SpringBoard lays out for the iPhone X it
believes it is running on), not from the ADT; the surface actually presented
is `id 3`, 1080x2160. So geometry is no longer the blocker.

What remains is that the new image presents only **three frames**
(`swap 3/4/7`) and then stops. The panel shows the data-migration spinner and
nothing moves. Two measurements must not be confused with that state: the
later ring (`/tmp/whyred-e76.txt`) was pulled from an **older hand-built
image** - its `boot-args` line has `cs_debug=2 -enable_kprintf_spam` and no
`sweetfb_stayon` / `sweetfb_noconsole` / `sweetfb_hz` - so its console text
over the GUI and its `display off` belong to that image. The new image's ring
(`/tmp/whyred-e75.txt`) says `display off ignored (sweetfb_stayon)` and its
console is muted, as intended.

`panel_console=hidden` on a manual run (or `PANEL_NOCONSOLE=1` by hand) adds
`sweetfb_noconsole=1`, which stops drawing the kernel console over the GUI on
the first swap while the SYSTEM ring keeps the text. `disable_kprintf_output`
was assumed to do this; it does nothing here, because this kernel has no
boot-arg by that name - only the symbol. The Apple logo from lavender
(`92f8e72`) is ported with whyred's own Y computed from the panel height, not
lavender's hard-coded 800. The shim's mark only survives the blink between
the jump and the XNU fbcon clear, so `0182` paints the same 208x256 bitmap
again in `AppleCLCD::start()` and mutes the console there when
`sweetfb_noconsole=1`: the panel then shows the mark for the whole kernel
boot, until the first iOS frame replaces it.

`0179`'s local panic was not the kernel: the hand-built images carried
`ignition_level=1 rootrw=1 wdt_*`, which makes libignition search a preboot
volume that does not exist (`ignition boot failed`) and pid 1 then dies with no
dyld cache. CI never had those switches; `payload_cmdline.py` now rejects
`ignition_level=` on every image so a hand build cannot reintroduce it.

Two kernel defects kept that state unmeasurable. `0180` answers
`DKIOCSYNCHRONIZE` on the SDHCI root: HFS calls it before advancing the journal
head, the driver fell through to `return ENOTTY`, and every checkpoint printed
`jnl: b(2, 0): flushing fs disk buffer returned 0x19` (0x19 is `ENOTTY`). The
card cache is never enabled, so returning success is honest. `0181` makes the
console flusher (`0143`) retry a busy card instead of stopping: `fsck` reads
the same card for minutes, a `CMD24` into that traffic times out, and the old
code returned, leaving the tail of the boot - the only place the late `exec`
lines live - out of the ring for good. The next boot with `sweetlog=1` can now
say whether `backboardd` really stops after three frames or whether the log
was simply truncated.

`0177` is the previous hardware-confirmed display state: it draws four colour
bars (red/green/blue/white) into the ABL framebuffer
at `0x9d400000` during the SANTONI probe, before `launchd`, and they appear
on the panel. That settles the display question — MDSS/DSI keep scanning the
boot framebuffer out after hand-off, so the black boot screen is the UART
console plus `kPEAcquireScreen` clearing the buffer, not missing display
hardware. It also proves the `0177` mapping fix: `0176` panicked with
`KERN_NO_SPACE` because `ml_io_map_wcomb()` allocates from the 8 MiB
`io_submap`, and the bars need 9,331,200 B.

The code-signing gate was cleared by `0174`: `SpringBoard` execs, opens
`IOPMrootDomain` and its `SWEET AKS` calls return `0x0`, with no
`csblob invalid` line. That closed the `csblob invalid: 85` /
`vm_shared_region_slide_page()` failures traced in `0173b`: the driver
returned bytes produced by a *different* request, because it copied the
shared bounce buffer after dropping its lock. `0174` keeps the copy inside
the transfer's critical section.

One thing still separated that daemon set from a usable screen: nothing
presented into the framebuffer yet. `0179` closed that - `backboardd`
presents 1080x2160 frames - and the remaining gap is the canvas geometry
described at the top of this section. Separately,
`installcoordinationd` failed with `NSPOSIXErrorDomain 13` creating its data
directory, the same EACCES that SWEET fixed by dropping `config_ext_resolver`
(`0057`); `0175` does that for WHYRED/LAVENDER. The boot-stage checklist
stays in the chat, not here.

`0178` repairs the SYSTEM console ring that 0177 exposed: the log boot wrote
26 blocks of real console text but never a header, so every `sweet-log-pull`
refused the area and the log looked empty. The header is now written before
the first ring block and a header failure no longer stops the flusher; the
pull tool can also decode a headerless ring.

The 0177 UART capture settles the daemon question: `SpringBoard` execs, opens
`AppleKeyStore` and `IOPMrootDomain` (`0x0`) and its AKS calls succeed, so
the black screen is not a dead SpringBoard. `0179` then showed
`backboardd` creating surfaces and presenting them, which moved the missing
piece from "presentation" to "the canvas the frames are drawn for".

| Milestone | Progress | Notes |
| --- | --- | --- |
| Kernel boots, console, framebuffer, userspace `init` as PID 1 | **100%** | Gold cores, GICv3, timers, `AppleCLCD` at 1080x2160, ramdisk init runs as PID 1 and ticks every 5 s. Proven on hardware since `0172`. `0177` adds the missing proof that the panel scans this framebuffer out after hand-off: four bars written by the kernel are visible. The boot screen is still black by design because the console is routed to the UART and `kPEAcquireScreen` clears the buffer |
| eMMC command, data and mount path | **100%** | 8-bit HS, EXT_CSD, GPT, 58.2 GiB, single- and multi-block transfers, journal replay, HFSX mounted read-write. The last defect was the bounce buffer being copied outside its lock (the source of the wrong code-signing bytes); `0174` owns every byte while the transfer holds the lock |
| Root filesystem from eMMC (`launchd`, real daemons) | **100%** | `launchd` runs as PID 1; `bindfs` lets the whole boot-task chain (`mount-phase-1`, `mount-phase-2`, `fips`, ...) exit successfully, and the LaunchDaemons start (`watchdogd`, `lockdownd`, `backboardd`, `amfid`, `imagent`, ...) |
| Code signing / AMFI (trust cache, entitlements) | **90%** | Confirmed on hardware: `SpringBoard` (a platform binary) execs, queries IOPMrootDomain and its `SWEET AKS` calls return `0x0`, with no `csblob invalid`. `0179` adds the same proof for `backboardd` in the full `sweetlog` ring (`exec backboardd`, IOSurface create/present). Remaining for 100%: removing the `unentitled_ios_sim_launch` / `launchd_unsecure_cache` bring-up switches |
| Daily-driver usable (touch, Wi-Fi, sound, battery) | **15%** | NT36672 touch, WCN3990 Wi-Fi and ADSP audio are still untouched |
| **Kernel readiness for an eMMC boot** | **100%** | Mount, bind mounts, platform-binary exec, the full boot-task chain and `SpringBoard` are proven on hardware by `0174`; `0175` only removes a permission bug on the userspace side |
| **Overall port (weighted)** | **~90%** | The kernel and the panel scan-out are done; what is left is userspace presentation (`backboardd`/`SpringBoard` opening `AppleCLCD`) and the peripherals |

The percentages describe this port, not the upstream `ios-sweet` project. They
move only when a change is confirmed on the phone, not when a patch compiles.

## Hardware architecture & differences

| Component | Redmi Note 5 / Note 5 Pro (whyred / SDM636) |
| --- | --- |
| CPU | Octa-core Kryo 260 (4x Cortex-A73 Gold @ 1.8 GHz + 4x Cortex-A53 Silver @ 1.6 GHz) |
| 16K Granule | Supported on Gold cluster (`0x100`–`0x103`); Silver cluster is parked via PSCI `CPU_ON` |
| Interrupt Controller | ARM GICv3: GICD `0x17a00000`, GICR `0x17b00000` |
| Timers | VTIMER INTID 19 (GIC_PPI 3), PTIMER INTID 18 (GIC_PPI 2), 19.2 MHz |
| Storage | eMMC 5.1 (`sdhc_1` at `0x0c0c4000`, Qualcomm SDCC v5) |
| Framebuffer | `0x9d400000`, 1080x2160, 32-bit `a8r8g8b8`, stride 4320 bytes |
| Power / Reset | PS_HOLD `0x010ac000`, IMEM restart reason `0x146bf65c` |
| Touch | Novatek NT36672 at `0x62` (or Synaptics DSX at `0x20` on the shenchao panel) on BLSP1 QUP1 I2C `0x0c175000`; IRQ GPIO 67, reset GPIO 66, VDDIO PM660 LDO11. **Not enabled**: whyred has no TLMM/GPIO, BLSP I2C or PM660 SPMI driver in this kernel yet |

## How it boots

```
ABL (stock Xiaomi bootloader, unlocked)
  -> fastboot boot shim-boot.img        (Android boot image, RAM only)
     -> shim on Silver CPU0: prints MMFR0/MIDR, wakes Gold CPU 0x100 via PSCI, parks
        -> shim on Gold: GICv3 setup, timer unmask, locates payload
           -> XNU Mach-O + Apple Device Tree (ADT) + boot-args
              -> XNU -> SDHCI eMMC driver -> HFS+ root in userdata
                 -> dyld + shared cache -> launchd -> backboardd + SpringBoard
```

There is no serial console. The kernel draws its console into ABL's framebuffer; with `sweetfb_noconsole=1` it stops after the first iOS frame (panics are still drawn). The kernel also keeps sweet's persistent log ring at the end of `userdata` and its user stack sampler (`SMP name tN pc .. bt ..`).

## How to do it yourself

You need a Redmi Note 5 / Note 5 Pro (`whyred`, SDM636) with an **unlocked bootloader**, a Linux PC with `fastboot`, and a GitHub account for the kernel build. **Everything on `userdata` is lost.** Boot, vendor_boot, firmware and other partitions are never written. On Xiaomi ABL, `fastboot boot` is often stubbed out; the tested path is the `recovery` partition (`fastboot flash recovery` + `fastboot reboot recovery`), leaving the Android `boot` partition intact.

### 1. Build the kernel

The kernel builds on GitHub's macOS runners (macos-14, Xcode 15) with [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build); newer Xcode's `iig` does not match the xnu-8796 IORPC ABI. Fork the repo and run, on the `whyred` branch:

```sh
gh workflow run xnu.yml -R YOU/ios-whyred --ref whyred
gh run download RUN_ID -R YOU/ios-whyred -n xnu-kernel -D xnu/out
```

The workflow defaults to `MACHINE_CONFIG=WHYRED`; pass
`-f machine_config=LAVENDER` to build the Redmi Note 7 kernel instead.

A build takes about 8 minutes. If the artifact upload times out on GitHub's side, rerun the job. Check that the kernel has no LSE left (`llvm-objdump -d --mattr=+v8.5a ... | grep -P '\t(cas|ldadd|ldset|ldclr|swp|ldapr)'`; the only hit should be the `K256` SHA table).

### 2. Build the iOS root image

Download the iPhone X (iPhone10,3) iOS 16.7.16 (20H392) IPSW from Apple. On Linux, with `apfs-fuse` (build it from [sgan81/apfs-fuse](https://github.com/sgan81/apfs-fuse) if your distribution has no package), `hfsprogs` and sudo:

```sh
# OS, SystemOS and AppOS disk images (see BuildManifest.plist, D22AP Erase identity)
sudo bash tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G
sudo mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot
sudo umount /mnt/iosroot
img2simg ios16-root.img ios16-root.simg
```

The device tree builder copies `/product` from the real iPhone X device tree. Take `Firmware/all_flash/DeviceTree.d22ap.im4p` from the IPSW, decompress its LZFSE payload (`ipsw img4 im4p extract`) and save it as `research/ipsw-x/d22ap.adt`.

### 3. Build the boot image

Needs `aarch64-linux-gnu-gcc` and AOSP `mkbootimg.py` (set `MKBOOTIMG` if it is not at the default path in `shim/build.sh`).

The stock ABL matches the appended DTB against `qcom,msm-id`/`qcom,board-id`
before it boots the kernel, so a whyred image needs the whyred DTB. Extract it
from any boot or recovery image built for the phone (stock MIUI `boot.img`,
TWRP/OrangeFox recovery, or a ROM `boot.img`):

```sh
# -> resources/whyred_dtb.bin (msm-id 0x159, board-id 0x30008, "SDM 636 ... E7S")
python3 tools/extract_appended_dtb.py twrp-whyred.img --board whyred
```

Put `kernel.development.whyred` from the CI `xnu-kernel` artifact in
`xnu/out/` (or pass its path: `BOARD=whyred shim/build.sh /path/kernel`), then:

```sh
BOARD=whyred shim/build.sh      # -> shim/out/shim-boot.img
```

The script runs `tools/mkadt.py --board whyred`, packs the kernel with
`tools/mkpayload.py` and appends `resources/whyred_dtb.bin` when present.
Before packing, `tools/check_payload.py` verifies that the kernel is an arm64
Mach-O and that the DTB carries this board's msm-id/board-id: a wrong-machine
kernel or DTB stops the build instead of failing later in fastboot.
Build the lavender image with `BOARD=lavender shim/build.sh`.

| Boot-arg | Effect |
| --- | --- |
| `rd=ufsroot0 ufsrw=1 rootrw=1` | Root on `userdata`, writable |
| `cpus=4` | All four Gold cores; `cpus=1` is the safe fallback |
| `sweetfb_noconsole=1` | Stop drawing kernel text over the GUI after the first iOS frame. Leave it out to debug (much slower: every console line is drawn and scrolled by the CPU) |
| `sweetfb_hz=30` | Panel refresh rate of the CPU frame copy |
| `sweettouch=0` | Disable the touch driver |
| `sweethw=1` | Also start sweet's SM7150 key and USB drivers (**unsafe on topaz**, not ported) |
| `launchd_unsecure_cache=1`, `wdt=-1 wdt_test=no_user_checkin` | launchd without AMFI; no hardware watchdog |

### 4. Flash the root and boot

The CI artifact (`xnu-kernel.zip`) is **not** flashed: it only contains
`kernel.development.whyred` (plus `SHA256SUMS`) for `shim/build.sh`. The file
to flash is the packaged boot image, `shim/out/shim-boot.img`.

```sh
fastboot -S 512M flash userdata ios16-root.simg   # ~5 minutes; do not unplug
# whyred: Xiaomi ABL stubs fastboot boot, so use the recovery slot
fastboot flash recovery shim/out/shim-boot.img
fastboot reboot recovery
# or, on a bootloader that allows it:
# fastboot boot shim/out/shim-boot.img
```

The shim screen (red) shows `MMFR0`, the Gold hand-off and `SPEED silver=… gold=…` for 8 seconds, then XNU starts. whyred does not show an Apple logo: that logo is drawn by iBoot, which is not in this chain, and the kernel console is on the UART while `kPEAcquireScreen` clears the boot framebuffer. Until the display path is up the panel is black on purpose (see `NOTES.md`). To get back to fastboot, hold Power + Volume down.

A manual run ("Run workflow") has a `build_mode` choice — `both`, `normal` or `log`; a push builds both:

| Image | Command line | Use |
| --- | --- | --- |
| `shim-boot.img` | no `sweetfbtest` (the four-bar scanout probe is opt-in), no `sweetlog`, no `cs_debug=2`, no kprintf spam | daily boot; nothing is recorded to the SYSTEM ring |
| `shim-boot-log.img` | the same plus `sweetlog=1 cs_debug=2 -enable_kprintf_spam` | when a line is missing; records the 4 MiB SYSTEM ring and every validated code page, several times slower at 115200 baud |

The log image writes the console to the 4 MiB SYSTEM ring, so its full boot can be pulled after a black-screen boot with `tools/sweet-log-pull.sh`. The normal image leaves the ring untouched and has no logging overhead.

`fb_test_pattern` (a manual-run choice, default `off`) adds `sweetfbtest=1`: four colour bars written into the boot framebuffer before BSD init. It was the scanout proof before the lock screen existed; turn it on only to check that MDSS still scans ABL's splash out.

### Touch (whyred)

Measured on this phone, not assumed: the controller answers at I2C **`0x20`
(Synaptics)** on BLSP1 QUP1, SDA GPIO 2 / SCL GPIO 3 (TLMM SOUTH), reset GPIO
66 and IRQ GPIO 67 (TLMM NORTH). The 0184 boot recorded `rst cfg 000003c3 io
00000003` (GPIO output, driven high - ABL hands it over out of reset), `irq
cfg 00000001 io 00000001` (input, idling high) and `idle sda 1 scl 1`: the
internal pull-up is enough at ~50 kHz. Novatek `0x62`/`0x01` does not answer
on this unit; the Tianma/Huaxing variants are where that address is expected.
The Shenchao variant is the Synaptics one.

Patch `0184` (`bsd/dev/santoni_touch.c`) re-muxes the two bus lines as GPIO
and bit-bangs I2C, because neither a TLMM GPIO driver nor a QUP I2C master
exists in this kernel yet. `0186` guessed RMI4 (two-byte register address at
`0x000e`); `0187` switched to the protocol the vendor driver actually uses
(page select `0xFF <page>`, one address byte, table at `0x00e9`), and both
returned the same continuous byte stream, one that shifts with the read
history instead of with the address. `0188` therefore treats the read itself
as the suspect: it makes SCL open-drain with clock-stretch waiting, pulses
reset with the DT timings, and runs a read-only matrix (same address twice,
one vs two address bytes, `0x00e8`/`0x00ea`, page 1, no page select) plus an
IRQ witness on GPIO 67 during the F12 poll.

| Boot-arg | Effect |
| --- | --- |
| `sweetouchi2c=1` | run the probe: scan `0x08`..`0x77`, then dump the RMI4 identification |
| `sweeti2c=1` | after the address scan, bring up the BLSP1 QUP1 hardware master and read the DSX through it; the bit-bang remains the fallback if the clocks or the SE do not come up |
| `sweetouchreset=1` | additionally drive reset (GPIO 66) per the DT: low 20 ms, high 200 ms, then 50 ms settle before the scan |
| `sweetouchwait=SECONDS` | hold the boot after the scan (default 15 s, 0 = no hold, max 60); the panel is usually muted by then, so prefer the ring |
| `sweetouchtouch=SECONDS` | poll F12 touch data for that long: top band of the panel is blue while waiting, green on the first change or IRQ edge, and every change goes to the ring (max 60) |
| (automatic) | before identification, 0189 prints every address framing it knows (`page|a1`, `page+a1`, `page|a2be`, `page|a2le`, `a1`, `a2be`, `a2le`) against `0x00e9`, marks each one `addressed`, `streams` or `failed`, and keeps the first that returns the same six bytes twice with a different entry one slot above (`using frame ...`) |
| (automatic) | if the page table has F01 but no F12, 0189 treats the part as bootloader-mode, pulses reset once and scans again |
| `sweetouchhalt=1` | paint the answer into the scanout buffer and stop the boot there (top quarter: white = first ack `0x01`, yellow = `0x62`, cyan = `0x20`, magenta = other, red = none; bottom green if anything answered) |

Without `sweetouchreset` the probe does not write GPIO 66/67, so the log also
records the reset/IRQ pad state ABL handed over.

The controller state is not a software problem any more: an earlier session left
it parked in flash-programming (bootloader) mode, where the PDT ends at
`F34 + F01` and there is no `F12`. A reset cycle after `ENABLE_FLASH_PROG`
brings it back to application mode, and the Android driver then works normally
again - `input2` with `ABS_X` 0-1079 / `ABS_Y` 0-2159 / 10 `ABS_MT_SLOT`s and
real `ABS_MT_POSITION_X/Y` events in `getevent`. The firmware image itself was
never missing; `tools/extract-ts-firmware.py` pulls it out of the recovery
kernel (`out/ts-fw-whyred.img`, 128,512 B) and `tools/sweet-ts-reflash.sh`
drives the vendor `doreflash` sysfs, where `echo 2` (not `1`) is FORCE.
`NOTES.md` has the full sequence.

**Reading the result without a serial cable.** Every test image carries
`sweetlog=1`, so the 4 MiB console ring on SYSTEM keeps the probe lines even
though the panel stops drawing them. Boot the whyred recovery with
`fastboot boot` (never flash, never format), then run
`tools/sweet-touch-answer.sh`: it pulls the ring and prints only the
`WHYRED TOUCH` lines plus the boot landmarks. That is how the `ack 0x20`
above was read.

### Debugging tips

- Stack samples (`SMP name[pid] slide S mh M` then `SMP name tN pc .. bt ..`): symbolicate shared-cache addresses with `ipsw dyld a2s dyld_shared_cache_arm64 <addr - S>`.
- `SWEET TOUCH: ...` lines show each touch bring-up step (clocks, SE2 firmware check, chip id `54:52`).

## Roadmap

1. **Touch**: the controller answers at Synaptics `0x20` on BLSP1 QUP1 I2C
   (`0x0c175000`, IRQ GPIO 67, reset GPIO 66, VDDIO PM660 LDO11) and reports
   F12 again after the bootloader-mode recovery. 0184/0186/0187/0188 prove the
   bus and identify the chip with bit-banged GPIO; next is a clean read of the
   F12 data registers from XNU (the 0188 matrix exists to explain the shifting
   stream), then the QUP I2C master (GCC `BLSP1_AHB` + `QUP1_I2C_APPS`) and
   PM660 SPMI for LDO11 so the bit-bang can be retired. `sweet_hid_touch()` and
   `SweetHIDTouch` are already shared with the topaz port and need no change.
2. **Buttons**: power and volume keys on the PM660/PM660L PON block, volume up on a PMIC GPIO.
3. **Smoother display**: higher `sweetfb_hz`; check whether the Gold cores run at a low clock without cpufreq (the shim's speed probe) and raise it if so.
4. **Narrow the GIC fix** to the register that actually matters, then move it into XNU.
5. **Real-time clock** from the PMIC RTC or passed in by the shim.
6. **All 8 cores (experiment, later):** build XNU with 4K pages (drop `__ARM_16K_PG__` from `SWEET.h`, as XNU's own `BCM2837` Raspberry Pi 3 config does on Cortex-A53) and describe two clusters, Gold as `P` and Silver as `E`, so the AMP scheduler uses both. Risk: the iOS 16 userland was only ever run on 16K pages (hard-coded 16K in malloc, JIT guard pages, IOSurface, commpage layout), and the 16K assumptions in the shim and sweet's patches have to be checked too. The 16K Gold-only kernel stays as the fallback.
7. **Silver cores as a coprocessor** (idea): run bare-metal shim code on them for the panel frame copy instead of XNU.
8. **3/4/6 GB variants:** the ADT uses a fixed safe 1 GiB budget at `0xa0000000`, so the same image works on all whyred RAM variants. A larger budget needs the shim to read the DDR ranges from ABL's `/memory` node.
9. **Wi-Fi:** bring up the WCN3990 firmware through TrustZone/SMEM/QMI in an XNU driver. Expose it to iOS first as an IONetworkingFamily interface, then emulate the IO80211 interface that `wifid` talks to.
10. **Speaker:** ADSP/AudioReach through TrustZone, then an XNU audio driver that CoreAudio can output to.
11. **GPU:** the whyred Adreno 509 is close to the Adreno 610 work in [MakrSas/topaz-woa](https://github.com/MakrSas/topaz-woa) (`TopazGpuW`); a kernel msm/DRM-style user client is the long-term path for QuartzCore's OpenGL ES renderer.

## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm), boot image v4, Gold hand-off, GIC setup, payload loader |
| `xnu/patches/` | XNU patch series (applied in order by CI): sweet's 0001–0079, santoni/lavender/whyred 0080+ |
| `.github/workflows/xnu.yml` | Kernel CI build (defaults to `WHYRED`) |
| `shim/board.h` | Per-board panel/SoC constants for whyred and lavender |
| `tools/mkadt.py` | Apple Device Tree for whyred/lavender (Gold cores, memory map, MMIO) and the iPhone X identity; `--board whyred` selects 1080x2160 |
| `tools/mkpayload.py` | Packs kernel + ADT + boot-args into the shim payload |
| `tools/mkiosroot.sh`, `tools/sweet-root-tweaks.py` | Build and adjust the HFS+ iOS root |
| `tools/sweet-crash.py`, `sweet-log.py` | Symbolicate crash reports, read the persistent log |
| `tools/extract-ts-firmware.py` | Pull the Synaptics DSX image out of any boot image (finds the `const unsigned char[]` in the kernel blob), validate the header, write `out/ts-fw-whyred.img` |
| `tools/sweet-ts-reflash.sh` | Push that image to the vendor driver over the `doreflash` sysfs (`imagesize` -> `data` -> `doreflash 2` = FORCE) |
| `tools/build-touch-diag.sh` | Pack the whyred touch-diag image from a CI kernel (muted console, ring on, `sweetouchi2c` + 30 s F12 poll, `rd=emmcroot0`) |
| `NOTES.md` | Engineering log and active handoff |

Not in git: partition backups, live device trees, Apple firmware and root images, build trees.

## whyred/lavender kernel patches

| # | What |
| --- | --- |
| 0142 | SDHCI eMMC 5.1 driver for the Qualcomm SDCC v5 controller |
| 0143 | Persistent console log on the SYSTEM partition |
| 0144 | 4 KiB page support for the 16 KiB-signed dyld shared cache |
| 0145 | LAVENDER machine config (`ARM64_BOARD_CONFIG_LAVENDER`, 16K GICv3 board) |
| 0146 | Disable topaz MDP/DSI MMIO on lavender |
| 0147 | SDCC v5 takeover and BSD-root mount without the IOBSD 30 s stall |
| 0148 | WHYRED machine config; reuses the LAVENDER board code and enables the built-in libpthread/HFS/BearSSL providers |
| 0149 | Report the failing SDHCI probe step, CMD1 result and controller state |
| 0150 | Keep ABL's working card after a stuck `RESET_ALL` on SDCC v5 |
| 0151 | Keep ABL's bus width and name data-phase failures |
| 0152 | Read EXT_CSD in 1-bit after CMD0, print CMD8's R1 and the settled bus |
| 0153 | Try EXT_CSD at 1-bit and 8-bit through PIO and SDMA, dump the transfer registers |
| 0154 | Probe the first data phase across width/engine/preset and keep the winning setup |
| 0155 | Poll `PRESENT_STATE` for data-ready and transfer-complete; this core does not latch those bits in `INT_STATUS` |
| 0156 | Fix `PRESENT_STATE[11:8]` (active = 9/8), drain a stuck `DAT_INHIBIT`, keep `AUTO_CMD12` paths, skip CMD5 in TRAN |
| 0157 | Try SDMA before PIO for EXT_CSD, reject all-zero FIFO blocks and validate REV/STRUCTURE/SEC_COUNT before keeping a setup |
| 0184 | Bit-bang BLSP1 I2C on TLMM GPIO 2/3 and scan the touch addresses (found `0x20`) |
| 0185 | Paint the probe answer into the scanout buffer and halt; the console is muted at the framebuffer hand-off |
| 0186 | RMI4 identification at `0x20`: version, function table, F01/F12 query blocks (read-only) - wrong protocol |
| 0187 | Synaptics DSX protocol (page select at `0xFF`, PDT at `0x00e9`) plus an F12 data poll - still a shifted stream |
| 0188 | Open-drain bit-bang with clock-stretch wait, DT reset timing, read matrix, IRQ edge witness |
| 0189 | Seven address framings tried against `0x00e9`, auto-selected by repetition, plus a bootloader-mode reset retry |
| 0190 | BLSP1 QUP1 hardware I2C master (vendor v2 tag protocol, GCC RCG/branches, TLMM mux 19) behind `sweeti2c=1`, bit-bang kept as fallback |

The earlier 0080-0141 topaz/santoni patches remain in the series for the
other devices.

## Key hardware facts (whyred)

- SoC SDM636: 4× Kryo 260 Silver (A53 class, MPIDR `0x0`–`0x3`, 1.6 GHz) + 4× Kryo 260 Gold (A73 class, `0x100`–`0x103`, 1.8 GHz). Only Gold has the 16K granule.
- GICv3 at `0x17a00000` (redistributors `0x17b00000`, 8 PEs, stride `0x20000`), virtual timer INTID 19, physical timer INTID 18, 19.2 MHz.
- eMMC 5.1 SDCC v5 at `0x0c0c4000`, GIC SPI 110.
- Panel: 1080×2160 DSI video mode, continuous splash at `0x9d400000` (`cont_splash_mem` in the vendor DTS), 32-bit `a8r8g8b8`, stride 4320.
- Touch: **Synaptics at I2C `0x20`** on this unit (bus verified by 0184: `ack 0x20`, `idle sda 1 scl 1`, reset released). Novatek NT36672 at `0x62`/`0x01` is the other panel variant. No report path into HID yet.
- Vendor DTS source: [LineageOS/android_kernel_xiaomi_sdm660](https://github.com/LineageOS/android_kernel_xiaomi_sdm660), `arch/arm64/boot/dts/vendor/qcom/xiaomi/longcheer/whyred/`.

## Credits

Based on [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet), which did the hard part: XNU on a Qualcomm phone with the iPhone X userland, and the lavender port this whyred target is derived from. Built on Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), Qualcomm/Xiaomi's published Linux kernel sources, and the hardware work in [MakrSas/topaz-woa](https://github.com/MakrSas/topaz-woa). iOS itself is Apple's and is not distributed here.
