#!/usr/bin/env python3
"""Extract a board's appended DTB from a Qualcomm boot image (Image.gz-dtb).

The stock ABL matches an appended DTB against qcom,msm-id/qcom,board-id before
it boots the kernel, so the whyred image needs a whyred DTB (and lavender a
lavender one).  Any boot/recovery image built for the phone carries it: either
a full Android boot image (header v0-v2) or a bare Image.gz-dtb.

Usage:
    extract_appended_dtb.py twrp-whyred.img                 # -> resources/whyred_dtb.bin
    extract_appended_dtb.py boot.img --board lavender -o /tmp/lavender_dtb.bin
"""
import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_payload as cp  # noqa: E402

ANDROID_MAGIC = b"ANDROID!"


def kernel_bounds(data):
    """Return (start, end) of the kernel section, or the whole file at 0."""
    if data[:8] != ANDROID_MAGIC:
        return 0, len(data)
    (kernel_size, _kernel_addr, _ramdisk_size, _ramdisk_addr, _second_size,
     _second_addr, _tags_addr, page_size, _version, _os_version) = \
        struct.unpack_from("<10I", data, 8)
    page_size = page_size if page_size >= 2048 else 2048
    start = page_size
    if start + kernel_size > len(data):
        raise ValueError("boot image claims a %d-byte kernel at %#x, past the end "
                         "of the %d-byte file" % (kernel_size, start, len(data)))
    return start, start + kernel_size


def gzip_end(data, start, end):
    """Offset where the gzip stream ends and the appended DTB table begins."""
    if data[start:start + 2] != b"\x1f\x8b":
        return start                      # already a bare DTB table
    decomp = zlib.decompressobj(16 + zlib.MAX_WBITS)
    try:
        decomp.decompress(data[start:end])
    except zlib.error as exc:
        raise ValueError("gzip kernel at %#x is corrupt: %s" % (start, exc))
    if not decomp.eof:
        raise ValueError("gzip kernel at %#x is truncated" % start)
    return end - len(decomp.unused_data)


def extract(path, board):
    with open(path, "rb") as f:
        data = f.read()
    start, end = kernel_bounds(data)
    table = gzip_end(data, start, end)
    entries, _ = cp.scan_dtbs(data, table, end)
    if not entries:
        raise ValueError("%s has no appended DTB after the kernel (looked at %#x..%#x)"
                         % (path, table, end))
    hits = cp.matching_dtbs(entries, board)
    if not hits:
        spec = cp.BOARDS[board]
        wanted = ("qcom,msm-id %#x and qcom,board-id %s"
                  % (spec["msm_id"], " or ".join("%#x" % b for b in spec["board_ids"])))
        raise ValueError("%s has no %s DTB (wanted %s); found: %s"
                         % (path, board, wanted,
                            "; ".join(cp.describe(e) for e in entries[:6])))
    return data[hits[0]["offset"]:hits[0]["offset"] + hits[0]["totalsize"]], hits[0], len(entries)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", help="Android boot image or Image.gz-dtb")
    ap.add_argument("--board", default="whyred", choices=sorted(cp.BOARDS))
    ap.add_argument("-o", "--output",
                    help="output file (default: resources/BOARD_dtb.bin)")
    args = ap.parse_args(argv)

    try:
        blob, entry, count = extract(args.image, args.board)
    except ValueError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    out = args.output or os.path.join(cp.REPO, "resources", "%s_dtb.bin" % args.board)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(blob)
    print("ok   %s: %d bytes from %d DTB%s at +%#x (%s)"
          % (out, len(blob), count, "" if count == 1 else "s",
             entry["offset"], cp.describe(entry)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
