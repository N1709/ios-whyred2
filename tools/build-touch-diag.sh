#!/bin/bash
# Pack the whyred touch-diag boot image from a kernel artifact.
#   tools/build-touch-diag.sh RUN_ID            -> images/shim-boot-touch-diag.img
#   tools/build-touch-diag.sh KERNEL_FILE
set -euo pipefail
cd "$(dirname "$0")/.."

arg=${1:?usage: build-touch-diag.sh RUN_ID|KERNEL}
if [ -f "$arg" ]; then
	kernel=$arg
else
	kernel=/tmp/opencode/xnu-kernel.${arg}
	bash tools/fetch-ci-kernel.sh "$arg" "$kernel"
fi
echo "kernel: $kernel ($(stat -c%s "$kernel") bytes)"

# Same command line as the existing diag image: muted console (the ring has
# the log), the ring on, and the touch probe with its 30 s F12 poll.
cmdl="emmcrw=1 cs_enforcement_disable=1 unentitled_ios_sim_launch=1 launchd_unsecure_cache=1 sweetfbtest=1 sweetlog=1 sweetouchi2c=1 sweetouchtouch=30 sweetouchreset=1"

rm -rf shim/out
ROOTDEV=emmcroot0 EXTRA_CMDLINE="$cmdl" KPRINTF_SPAM= KPRINTF_MUTE=1 \
	PANEL_NOCONSOLE=1 bash shim/build.sh "$kernel"
# zopfli only exists on the CI runner; gzip -9 already lands under the ABL
# download limit on this kernel, so fall back instead of failing.
if command -v zopfli >/dev/null; then
	zopfli --i15 -c shim/out/Image > shim/out/Image.z.gz
	ROOTDEV=emmcroot0 EXTRA_CMDLINE="$cmdl" KPRINTF_SPAM= KPRINTF_MUTE=1 \
		PANEL_NOCONSOLE=1 IMAGE_GZ="$PWD/shim/out/Image.z.gz" \
		bash shim/build.sh "$kernel"
fi

cp shim/out/shim-boot.img images/shim-boot-touch-diag.img
python3 tools/payload_cmdline.py --image images/shim-boot-touch-diag.img
ls -l images/shim-boot-touch-diag.img
