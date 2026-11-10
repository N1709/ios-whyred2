# audio

- **Hardware:** Qualcomm WCD audio codec on SLIMbus (the exact part must be
  read from the full whyred DTS; the mirrored board files only carry the
  `btfmslim_codec` node) and the ADSP.
- **Code:** none. The kernel boots with the Sweet audio/battery bring-up off
  (`sweetcpufreq=0 sweetbattery=0` on the command line).
- **Not started:** SLIMbus, codec probe, ADSP firmware, `IOAudioFamily`.
- **Next:** confirm the codec part and its SLIMbus master from the Android DTS,
  then decide whether a codec-only path (no ADSP) is enough for boot sounds;
  SpringBoard does not require audio to render.

Reference: `unixnu/WhyredDsp` (read `../THIRD_PARTY.md` first). It targets the
same SDM636 ADSP over GLINK/SMEM, but it does not carry a codec path yet; its
class layout, reset sequence and boot arguments are the starting point for a
WCD9335 driver. The codec registers and firmware still have to be confirmed
from the Android DTS.
