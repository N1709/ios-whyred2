#!/bin/sh
# Read the SANTONI console log (kernel patch 0143) from a phone booted into
# recovery (fastboot boot recovery/recovery.img; adb root shell).
#   tools/sweet-log-pull.sh OUT.txt
#
# The log lives in the first 8193 blocks (512B) of the idle SYSTEM partition:
# block 0 is the "SANTONILOG1" header, then a 4 MiB ring. 0047's older
# USERDATA-tail ring ("SWEETLOG") is still accepted as a fallback.
#
# The by-name entries are symlinks into /dev/block/mmcblk0pNN, so the resolved
# path never contains "system": pick the branch from the *name* that matched,
# not from the target (that mistake sent earlier runs to the userdata branch).
set -eu
out=${1:?usage: sweet-log-pull.sh OUT.txt}

adb get-state >/dev/null 2>&1 || {
    echo "no adb device: boot recovery (fastboot boot recovery.img) first" >&2
    exit 1
}

sel=$(adb shell 'for n in system userdata; do
	for d in /dev/block/bootdevice/by-name/$n /dev/block/by-name/$n; do
		if [ -e "$d" ]; then
			echo "$n $(readlink -f "$d" 2>/dev/null || echo "$d")"
			exit 0
		fi
	done
done' | tr -d '\r')
[ -n "$sel" ] || { echo "system or userdata block device not found" >&2; exit 1; }
kind=${sel%% *}
dev=${sel#* }

case $kind in
system)
    echo "SYSTEM partition ($dev): reading 8193 blocks (512B header + 4 MiB ring)..." >&2
    adb exec-out "dd if=$dev bs=512 count=8193 2>/dev/null" > "$out.bin"
    echo "$dev: read $(stat -c %s "$out.bin") bytes" >&2
    if ! head -c 11 "$out.bin" | grep -q SANTONILOG1; then
        # 0177 left 26 blocks of real console text and a blank sector 0: the
        # header write after the first flush failed, so refusing the area threw
        # the log away. Decode the ring without the header when the start looks
        # like a boot log; 0178 writes the header before the first block.
        echo "no SANTONILOG1 header at the start of SYSTEM; trying the ring anyway" >&2
        if python3 "$(dirname "$0")/sweet-log.py" "$out.bin" "$out" --salvage; then
            exit 0
        fi
        echo "The flusher never wrote: that boot did not have sweetlog=1," >&2
        echo "or its log never started. Look for this line on the console:" >&2
        echo "  WHYRED LOG: console log flusher active on SYSTEM partition" >&2
        echo "Raw area kept at $out.bin (sector 0: $(dd if="$out.bin" bs=16 count=1 2>/dev/null | od -An -c | tr -s ' '))" >&2
        exit 2
    fi
    ;;
*)
    bytes=$(adb shell blockdev --getsize64 "$dev" | tr -d '\r')
    skip=$((bytes / 4096 - 1025))
    echo "USERDATA tail ($dev): $bytes bytes, reading blocks $skip.." >&2
    adb exec-out "dd if=$dev bs=4096 skip=$skip count=1025 2>/dev/null" > "$out.bin"
    echo "$dev: read $(stat -c %s "$out.bin") bytes" >&2
    ;;
esac

python3 "$(dirname "$0")/sweet-log.py" "$out.bin" "$out"
