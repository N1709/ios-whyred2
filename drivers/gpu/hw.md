# gpu hardware notes

Facts taken from `refs/whyred-android/dts_sdm636_whyred.dtsi` and
`dts_sdm660_whyred.dtsi`. There is no GPU driver code in the tree yet; this
file exists so the first implementation starts from addresses, not guesses.

| Item | Value |
| --- | --- |
| GPU | Adreno 509 |
| Chip ID | `0x05000900` (`&msm_gpu { qcom,chipid = <0x05000900>; }`) |
| GPU clock controller | `clock-controller@5065000`, `compatible = "qcom,gpu-sdm660"` |
| GPU CC | `compatible = "qcom,gpucc-sdm660"` (see the SDM660 DTS for its address) |
| IOMMU alias | `qcom,alias-name = "gpu"` |

Open questions before code:

- firmware file names and where the vendor partition stores them;
- which `kgsl` register blocks the Adreno 509 needs (read `msm_gpu`/`kgsl` from
  the Android kernel in `refs/whyred-android/`);
- whether a minimal IOAccelerator that only serves `IOSurface` is enough for
  SpringBoard, before attempting full 3D.
