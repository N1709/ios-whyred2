# wifi

- **Hardware:** Qualcomm WCN3990 (`compatible = "qca,wcn3990"` in
  `refs/whyred-android/dts_sdm660_whyred.dtsi`), Bluetooth on the SLIMbus
  (`btfmslim_codec`).
- **Code:** none.
- **Not started:** no firmware download, no SNOC/PCIe transport, no 802.11
  MAC, no `IO80211` family.
- **Notes for the first attempt:**
  - The Android driver is `ath10k_snoc`; the firmware files it loads on this
    phone must be copied out of the vendor partition and named here.
  - Bluetooth is a separate SLIMbus client and can be brought up later; it is
    not needed for the iOS boot.
  - Check the regulator and clock list in the DTS before writing attach code:
    the WCN3990 needs its `vdd*` rails up before the first QMI handshake.

Mirror: `TopazModem.cpp`. The topaz stack (scan, HTT, station, join) is not a
WCN3990 driver, but it is the only WLAN implementation in this tree and shows
how the modem/WLAN IOUserClient is wired.

Borrowed reference: `hakios/` from HakiOS (read `../THIRD_PARTY.md` first).
- `lx/` is the reusable part: an MIT layer that compiles an unmodified Linux
  wireless driver against LinuxKPI-style headers and hosts it in XNU. HakiOS
  uses it for ath11k on a WCN6750; whyred's WCN3990 is **ath10k_snoc**, so the
  layer is the same idea with a different driver underneath.
- `hakios.lx.c` is the kernel side of that layer (memory, locks, work, bus).
- `hakios.wifi.c` is the WCN6750 back end: WPSS firmware loading, GLINK/QMI,
  TrustZone. WCN3990 has a different firmware (`wlanmdsp.mbn`) and boot path,
  so this file is a reference, not a port.
- `hakios.apple80211.c` and `hakios.wlan.cpp` show how frames reach
  `IO80211`; that glue is hardware-independent.
