#!/bin/sh
# Read the SWEET / SANTONI console log (kernel patches 0047 / 0143) from a phone
# booted into recovery (fastboot boot recovery/recovery.img; adb root shell).
#   tools/sweet-log-pull.sh OUT.txt
#
# Santoni: pulls 8193 blocks (512B) from the dedicated idle SYSTEM partition.
# Sweet: pulls the last 1025 blocks (4 KiB) of the USERDATA partition.
set -eu
out=${1:?usage: sweet-log-pull.sh OUT.txt}
dev=$(adb shell 'for d in /dev/block/bootdevice/by-name/system /dev/block/by-name/system /dev/block/bootdevice/by-name/userdata /dev/block/by-name/userdata; do [ -e $d ] && readlink -f $d && break; done' | tr -d '\r')
[ -n "$dev" ] || { echo "system or userdata block device not found" >&2; exit 1; }

if echo "$dev" | grep -q "system"; then
    echo "Found SYSTEM partition ($dev): pulling 8193 blocks (4 MiB + 512B header)..." >&2
    adb exec-out "dd if=$dev bs=512 count=8193 2>/dev/null" > "$out.bin"
    echo "$dev: read 8193 blocks ($(stat -c %s "$out.bin") bytes)" >&2
else
    bytes=$(adb shell blockdev --getsize64 "$dev" | tr -d '\r')
    skip=$((bytes / 4096 - 1025))
    adb exec-out "dd if=$dev bs=4096 skip=$skip count=1025 2>/dev/null" > "$out.bin"
    echo "$dev: $bytes bytes, read blocks $skip.. ($(stat -c %s "$out.bin") bytes)" >&2
fi

python3 "$(dirname "$0")/sweet-log.py" "$out.bin" "$out"
