# audio hardware notes

Facts from `refs/whyred-android/dts_sdm660_whyred.dtsi`. No audio driver code
exists in the tree; the boot line runs with the Sweet battery/cpufreq bring-up
off and nothing touches the audio path.

| Item | Value |
| --- | --- |
| Audio SLIMbus | `slim@151c0000`, `compatible = "qcom,slim-ngd"`, reg names `slimbus_physical`, `slimbus_bam_physical` |
| Bluetooth SLIMbus | `slim@15240000`, `compatible = "qcom,slim-ngd"`, slave `btfmslim_codec` (`qcom,btfmslim_slave`) |
| Codec | in `msm-audio.dtsi` (not mirrored here); confirm the exact WCD part before writing code |
| ADSP | see the `adsp`/`qcom,adsp` node in the full whyred DTS |

Open questions before code:

- codec part number and its SLIMbus master;
- whether codec-only playback (no ADSP firmware) is enough for boot sounds;
- SpringBoard rendering does not depend on audio, so this stays off the
  critical path.
