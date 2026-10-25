#!/bin/bash
# Download only as much of a CI `xnu-kernel` artifact as the kernel needs.
#
#   tools/fetch-ci-kernel.sh RUN_ID OUT_KERNEL
#
# The artifact is ~60 MB (kernel + dSYM + KDK symbol dumps); the kernel entry
# is ~7 MB compressed and early in the archive. This starts the download, asks
# tools/zip_partial.py after every few seconds, and kills curl as soon as the
# kernel is complete. Public repos work without a token via nightly.link.
#
# The repository defaults to `origin` (or $REPO=owner/name).
set -uo pipefail

run=${1:?usage: fetch-ci-kernel.sh RUN_ID OUT_KERNEL}
out=${2:?usage: fetch-ci-kernel.sh RUN_ID OUT_KERNEL}
here=$(cd "$(dirname "$0")" && pwd)

repo=${REPO:-}
if [ -z "$repo" ]; then
	url=$(git -C "$here/.." remote get-url origin 2>/dev/null || true)
	repo=$(echo "$url" | sed -e 's#^.*[:/]##' -e 's#\.git$##')
	owner=$(echo "$url" | sed -e 's#^.*[:/]\([^/]*\)/[^/]*$#\1#')
	[ -n "$owner" ] && repo="$owner/$repo"
fi
[ -n "$repo" ] || { echo "no repository: set REPO=owner/name" >&2; exit 1; }

link="https://nightly.link/$repo/actions/runs/$run/xnu-kernel.zip"
tmp=$(mktemp "${TMPDIR:-/tmp}/xnu-artifact.XXXXXX.zip")
trap 'kill $pid 2>/dev/null; rm -f "$tmp"' EXIT

echo "fetching $link" >&2
curl -sL --retry 5 --retry-delay 2 --max-time 3600 -o "$tmp" "$link" &
pid=$!

for _ in $(seq 1 240); do
	if python3 "$here/zip_partial.py" "$tmp" "$out"; then
		echo "kernel ready: $out" >&2
		exit 0
	fi
	case $? in
	1) exit 1 ;;
	esac
	if ! kill -0 "$pid" 2>/dev/null; then
		echo "download ended before the kernel entry was complete" >&2
		exit 1
	fi
	sleep 5
done
echo "timed out waiting for the kernel entry" >&2
exit 1
