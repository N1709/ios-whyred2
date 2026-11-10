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

Reference: none yet. There is no Wi-Fi driver in the UniXNU whyred tree, and
the topaz modem path above is the only WLAN implementation in this repository.
WCN3990 is ath10k_snoc; port it from the Linux driver only after the modem and
its `vdd*` rails are proven on this board.
