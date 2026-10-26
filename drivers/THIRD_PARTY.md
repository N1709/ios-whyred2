# Third-party driver references

The `*/hakios/` folders hold driver source taken from **HakiOS**
(<https://github.com/oosexgang/hackios-xnu-kernel>), which runs the iOS 16.5
userland on a Qualcomm SM7325 phone (POCO X5 Pro, `redwood`). They are here
because that project has already written the parts this port still lacks:
an Adreno IOAccelerator with a Mesa/freedreno path, a Linux-driver host layer
for Wi-Fi, a touch input queue, an `IOPMPowerSource`, and an ADSP audio path.

| | |
| --- | --- |
| Upstream | `https://github.com/oosexgang/hackios-xnu-kernel` |
| Commit | `88513176b1e96ecf0b764cd62c6f4e6ac71345e3` (2026-10-26) |
| Kernel files | Apple Public Source License 2.0 (`drivers/hakios-LICENSE.APSL`) |
| `wifi/hakios/lx/` | MIT (the layer itself); its `include/` carries Linux/FreeBSD headers under their own licences, see `drivers/hakios-THIRD_PARTY_LICENSES.md` |
| Not imported | the FocalTech firmware blob (`hakios.focaltech_fw.h`, upstream marks its licence "to verify"), and ath11k itself (fetched, not redistributed) |

## These files are references, not build inputs

Nothing under `*/hakios/` is compiled. The kernel build only sees the patched
XNU tree, so a driver that actually runs must arrive as a numbered patch in
`xnu/patches/` and be mirrored by `tools/extract_drivers.py`. These trees are
kept byte-for-byte as upstream wrote them (`SHA256SUMS` in each folder) so
they can be diffed and copied from, and so it is always clear which code is
ours and which is borrowed.

If a file here is adapted into a whyred driver, the adapted code goes into a
patch; the reference file stays untouched.

## What fits the whyred hardware

| HakiOS source | Redwood hardware | whyred hardware | Verdict |
| --- | --- | --- | --- |
| `gpu/hakios/hakios.gpu.cpp` | Adreno 642L, DRM/msm UAPI to Mesa | Adreno 509 (a5xx), no Mesa userspace yet | **structure reusable**, UAPI bridge is chip-independent |
| `gpu/hakios/hakios.adreno.c` | a660: GMU, zap, SMMU, 0x06030500 | a509: a5xx GMU/zap, different registers and firmware | **rewrite**, only the step order and the TrustZone calls carry over |
| `gpu/hakios/MiniIOSurface.*` | IOSurface stand-in | this tree already has `SweetSurface` | compare, do not merge blindly |
| `display/hakios/MiniIOMobileFramebuffer.cpp` | panel as bootloader left it | same idea as `AppleCLCD`/`sweetfb` | reference only |
| `touch/hakios/hakios.input.c` | `/dev/hakios-touch` sample queue | this tree publishes HID from `sweet_touch.c` | **pattern reusable**, device interface may be simpler than HID |
| `touch/hakios/hakios.goodix.c` | GT9916/FT3680 on SPI GENI | NT36672 on I2C QUP | **rewrite**, the bus and protocol are different |
| `wifi/hakios/lx/` | Linux mac80211/ath11k host layer | WCN3990 needs ath10k_snoc | **reusable layer**, ath10k port still to do |
| `wifi/hakios/hakios.lx.c` | kernel side of that layer | same | **reusable**, needs the WCN3990 bus/firmware backend |
| `wifi/hakios/hakios.wifi.c` | WCN6750 on WPSS | WCN3990 on its own subsystem | **rewrite**, different firmware and boot path |
| `wifi/hakios/hakios.apple80211.c` | `IO80211` glue | same idea | **reusable** once a driver produces frames |
| `audio/hakios/hakios.adsp.c` | ADSP GLINK/QMI, WCD938x | ADSP exists, codec is WCD9335 | **pattern reusable**, codec and firmware differ |
| `audio/hakios/hakios.sound.c` | `IOAudioFamily` stream | same | **reusable** once a codec path exists |
| `power/hakios/hakios.battery.cpp` | `IOPMPowerSource` consumer | same | **reusable**, needs a PM660 data source |
| `power/hakios/hakios.battery.c` | PM7325B via ADSP/GLINK | PM660 via SPMI | **rewrite** |
| `power/hakios/hakios.cpufreq.c` | SM7325, RPMh | SDM636, RPM | **rewrite** |
| `hakios-pull-firmware-redwood.sh` | redwood firmware paths | whyred vendor paths differ | **template** for a whyred pull script |

The point of the table is to stop a wrong import: a driver that names a660
registers on an a509 will not boot, and a WCN6750 firmware loader cannot load
`wlanmdsp.mbn`.
