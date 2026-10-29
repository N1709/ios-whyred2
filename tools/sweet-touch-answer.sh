#!/bin/bash
# Pull the SYSTEM ring from a booted recovery and print what the touch probe
# said. Boot the whyred recovery with `fastboot boot` (never flash).
#
#   tools/sweet-touch-answer.sh [/tmp/whyred-touch-answer.txt]
set -euo pipefail

cd "$(dirname "$0")/.."
out=${1:-/tmp/whyred-touch-answer.txt}

adb wait-for-device
adb root >/dev/null 2>&1 || true
adb wait-for-device

bash tools/sweet-log-pull.sh "$out"

echo
echo "== WHYRED TOUCH =="
grep -n "WHYRED TOUCH" "$out" || echo "(no WHYRED TOUCH line in the ring)"
echo
echo "== landmarks =="
grep -nE "WHYRED SDHCI: driver|WHYRED FB: test pattern 4 bars|WHYRED FB: readback" "$out" || true
