#!/bin/bash
# sweet-ts-reflash.sh - reload the Synaptics DSX application firmware through
# the vendor driver's sysfs interface, from a booted whyred recovery.
#
# Context: the controller on this unit is stuck in bootloader mode, so its PDT
# only advertises F34 (firmware update) + F01 (device control) and there is no
# F12, i.e. no touch - in Android, in recovery and in iOS alike. The vendor
# driver refuses to update on its own because it compares the bootloader's
# firmware ID against the image ID and concludes the image is older.
#
# The image is the one embedded in the recovery kernel; extract it first with:
#     tools/extract-ts-firmware.py <recovery.img>
# which writes out/ts-fw-whyred.img. The file is 128512 bytes, v06 header,
# product E753, firmware ID 2732044 - the exact image the driver already
# parsed at boot (see dmesg: "Image firmware ID = 2732044").
#
# This script only talks to the touch controller. No partition, no userdata,
# no boot image is written.
#
# Usage: bash tools/sweet-ts-reflash.sh [--image FILE] [--force|--normal]
#   --image FILE  local firmware image (default: out/ts-fw-whyred.img)
#   --force       'doreflash 2' (FORCE, default: needed because the device
#                 firmware ID is higher than the image's)
#   --normal      'doreflash 1' (NORMAL, lets fwu_go_nogo decide)

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
IMAGE="$HERE/../out/ts-fw-whyred.img"
MODE=force

while [ $# -gt 0 ]; do
	case "$1" in
	--image) IMAGE=$2; shift 2 ;;
	--force) MODE=force; shift ;;
	--normal) MODE=normal; shift ;;
	*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

say() { printf '%s\n' "$*"; }

say "waiting for adb..."
for _ in $(seq 1 180); do
	adb shell true >/dev/null 2>&1 && break
	sleep 1
done
adb shell true >/dev/null 2>&1 || { say "no adb device"; exit 1; }

NODE=$(adb shell 'for d in /sys/class/input/input*; do [ -e "$d/doreflash" ] && echo "$d" && break; done' \
	| tr -d '\r' | head -n1)
[ -n "$NODE" ] || { say "no synaptics input node with doreflash"; exit 1; }
say "touch node: $NODE"

if [ -f "$IMAGE" ]; then
	FW_SIZE=$(stat -c %s "$IMAGE")
	say "--- pushing $IMAGE ($FW_SIZE bytes) ---"
	adb push "$IMAGE" /tmp/ts-fw.img >/dev/null || exit 1
	DEV_IMAGE=/tmp/ts-fw.img
	DEV_SIZE=$(adb shell "stat -c %s $DEV_IMAGE" | tr -d '\r')
	[ "$DEV_SIZE" = "$FW_SIZE" ] || { say "push size mismatch: $DEV_SIZE != $FW_SIZE"; exit 1; }
	# The first 16 bytes must be a DSX v05/v06 header: major version at 7.
	HDRVER=$(adb shell "dd if=$DEV_IMAGE bs=1 skip=7 count=1 2>/dev/null | od -An -tu1" | tr -d ' \r')
	say "header version: $HDRVER"
	case "$HDRVER" in
	5|6) ;;
	*) say "not a DSX v05/v06 image, refusing"; exit 1 ;;
	esac
else
	say "no local image at $IMAGE"
	say "run: tools/extract-ts-firmware.py <recovery.img>"
	exit 1
fi

say "--- current PDT (expect F34 + F01 + END, no F12) ---"
for a in 233 227 221; do
	printf '0x%03x: ' "$a"
	adb shell "dd if=$NODE/rmidev/data bs=1 skip=$a count=6 2>/dev/null" | od -An -tx1
done

say "--- loading image into the driver ---"
adb shell "echo $FW_SIZE > $NODE/imagesize" || exit 1
adb shell "dd if=$DEV_IMAGE of=$NODE/data bs=4096 2>/dev/null" || exit 1

say "--- starting update ($MODE) ---"
# enum update_mode { NORMAL = 1, FORCE = 2, LOCKDOWN = 8 };
if [ "$MODE" = "force" ]; then
	adb shell "echo 2 > $NODE/doreflash"
else
	adb shell "echo 1 > $NODE/doreflash"
fi
say "doreflash rc=$?"

say "--- dmesg ---"
adb shell 'dmesg | grep -i -E "synaptics|fwu|F34|flash|bootloader" | tail -n 40'

say "--- PDT after update (want an entry ending in 0c = F12) ---"
for a in 233 227 221 215; do
	printf '0x%03x: ' "$a"
	adb shell "dd if=$NODE/rmidev/data bs=1 skip=$a count=6 2>/dev/null" | od -An -tx1
done
