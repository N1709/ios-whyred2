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
