#!/bin/sh
#
# recovery-dsx-test.sh - decide *why* every raw DSX read streams.
#
# Run on the host while the phone is in recovery with adb up.  It pushes
# tools/i2cprobe.c (if needed), forces the vendor driver back into continuous
# reporting mode, then reads the same three registers twice:
#
#   E9 -> F34 PDT entry      (27 00 09 00 21 34)
#   E3 -> F01 PDT entry      (2f 26 0d 04 01 01)
#   E9 -> F34 again, unless the address phase is ignored
#
# MODE 2 is the vendor driver's own framing once page 0 is selected: one
# address byte, a repeated start, then the data.  MODE 0 re-sends the page
# select (0xFF 0x00) before every read, which is what the XNU diag does and
# what the vendor driver deliberately does *not* do.
#
# Verdicts:
#   E9 -> E3 -> E9 gives F34, F01, F34  => the address phase works; the bug is
#       the repeated page-select write.  Fix XNU to cache the page.
#   E9 -> E3 -> E9 gives F34, F01, F12  => the pointer auto-advances and every
#       address write is ignored, even after a clean resume.  The chip is in
#       the dump/stream state, not app mode.
#   all 00                              => the chip is asleep; touch the panel
#       once and rerun.
#
# The driver MUST stay bound: removing it (or a failed rebind) disables
# vcc_i2c, which also feeds the external I2C pull-ups, and every read after
# that is floating/ff.
set -u

ADB=${ADB:-adb}
BIN=${BIN:-/data/local/tmp/i2cprobe}
BUS=${BUS:-1}
ADDR=${ADDR:-0x20}

say() { printf '%s\n' "$*"; }

command -v "$ADB" >/dev/null 2>&1 || { say "adb not found"; exit 1; }
$ADB get-state >/dev/null 2>&1 || { say "no adb device (recovery required)"; exit 1; }

if ! $ADB shell "test -e /sys/bus/i2c/devices/${BUS}-00${ADDR#0x}" 2>/dev/null; then
	say "no ${BUS}-0020 i2c device: is the DSX driver bound?"
	exit 1
fi

if [ ! -x "${PROBE:-/tmp/opencode/i2cprobe}" ]; then
	say "building i2cprobe"
	command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || {
		say "aarch64-linux-gnu-gcc missing; build i2cprobe yourself"
		exit 1
	}
	aarch64-linux-gnu-gcc -O2 -static -o /tmp/opencode/i2cprobe \
	    "$(dirname "$0")/i2cprobe.c" || exit 1
fi
# Push whenever the phone lacks a runnable copy: a freshly booted recovery does
# not keep /data/local/tmp, and checking only the host binary left every round
# failing with "No such file or directory".
if ! $ADB shell "test -x $BIN" 2>/dev/null; then
	$ADB push /tmp/opencode/i2cprobe "$BIN" >/dev/null || exit 1
	$ADB shell "chmod 755 $BIN" || exit 1
fi

say "== state before resume =="
$ADB shell "$BIN $BUS $ADDR 0 0xe9 6 2"

say "== forcing continuous reporting mode (no wakeup gesture) =="
$ADB shell "echo 0 > /sys/bus/i2c/devices/${BUS}-0020/wake_gesture 2>/dev/null; \
            echo 0 > /sys/bus/i2c/devices/${BUS}-0020/suspend 2>/dev/null; \
            sleep 1; \
            dmesg | tail -3"
say "do NOT touch the panel during the next step"

for round in 1 2 3; do
	for mode in 2 0; do
		say "== round $round mode $mode (E9, E3, E9) =="
		$ADB shell "for a in 0xe9 0xe3 0xe9; do \
		            printf '%s ' \$a; \
		            $BIN $BUS $ADDR 0 \$a 6 $mode; \
		            sleep 0.1; \
		        done"
	done
done

say "== done: compare the three E9 reads in each block =="
