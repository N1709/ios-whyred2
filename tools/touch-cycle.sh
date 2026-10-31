#!/bin/bash
# One touch-diagnostic cycle, no typing on the phone.
#
#   tools/touch-cycle.sh [IMAGE] [DWELL_SECONDS]
#
# The phone may be in fastboot, in recovery (adb) or already running. The
# script puts it in fastboot, boots the diagnostic image, waits for the probe
# to run, boots the OrangeFox recovery with `fastboot boot` (this ABL does not
# honour `fastboot reboot recovery`), and pulls the SYSTEM ring back.
#
# The touch probe runs before any input is needed, so this cycle does not ask
# for a tap: the QUP bring-up and the framing matrix are the answers.
set -uo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
img=${1:-$root/images/shim-boot-touch-diag.img}
dwell=${2:-75}
recovery=${RECOVERY_IMG:-$HOME/Downloads/Telegram Desktop/OrangeFox-R11.1-Stable-whyred/recovery.img}
ring=${RING:-/tmp/whyred-cycle.txt}

say() { printf '%s\n' "$*"; }

wait_fastboot() {
	for _ in $(seq 1 60); do
		[ -n "$(fastboot devices 2>/dev/null | awk '{print $1}')" ] && return 0
		sleep 1
	done
	return 1
}

wait_adb() {
	for _ in $(seq 1 60); do
		[ "$(adb get-state 2>/dev/null)" = "recovery" ] && return 0
		sleep 1
	done
	return 1
}

[ -f "$img" ] || { say "missing image: $img"; exit 1; }
[ -f "$recovery" ] || { say "missing recovery: $recovery"; exit 1; }

say "image   : $img"
say "sha256  : $(sha256sum "$img" | cut -c1-16)"
say "recovery: $recovery"

if [ "$(adb get-state 2>/dev/null)" = "recovery" ]; then
	say "phone is in recovery; rebooting to bootloader"
	adb reboot bootloader
	sleep 3
fi

if ! wait_fastboot; then
	say "no fastboot device: put the phone in fastboot and retry"
	exit 1
fi

say "boot: diagnostic image"
fastboot boot "$img" || exit 1

say "waiting ${dwell}s for the probe (no tap needed)"
sleep "$dwell"

say "waiting for the probe to reset the phone into fastboot"
say "(sweetouchreboot=1; fallback: hold Power + Vol-Down)"
for _ in $(seq 1 180); do
	[ -n "$(fastboot devices 2>/dev/null | awk '{print $1}')" ] && break
	sleep 1
done
if ! wait_fastboot; then
	say "no fastboot device yet; rerun with the phone in fastboot"
	exit 1
fi

say "boot: recovery via 'fastboot boot'"
fastboot boot "$recovery" || exit 1

if ! wait_adb; then
	say "recovery did not come up on adb; boot it by hand and rerun"
	exit 1
fi

say "pulling the SYSTEM ring"
bash "$root/tools/sweet-log-pull.sh" "$ring" >/dev/null || exit 1
say "ring: $ring ($(wc -c <"$ring") bytes)"
say ""
say "--- touch / i2c lines ---"
grep -n "WHYRED I2C\|framing matrix\|WHYRED TOUCH" "$ring" | tail -40
