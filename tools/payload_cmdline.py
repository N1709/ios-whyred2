#!/usr/bin/env python3
"""Print the command line the shim actually passes to XNU.

The shim does not use the boot.img command line: it copies the line stored in
the payload (offset 0x48 of the SWEETXNU header). A boot.img can therefore look
right and still boot `rd=md0`, which is exactly the mistake this tool exists to
catch.

    tools/payload_cmdline.py --payload shim/out/payload.bin
    tools/payload_cmdline.py --image shim/out/shim-boot.img
    tools/payload_cmdline.py --image shim-boot.img --require rd=emmcroot0 \
        --require cs_debug=2
"""

import argparse
import struct
import sys
import zlib

PAGE = 4096
MAGIC = b"SWEETXNU"
CMDLINE_OFF = 0x48
CMDLINE_LEN = 608


def kernel_image(data):
    """Return the decompressed shim image from an Android boot.img."""
    if data[:8] != b"ANDROID!":
        sys.exit("not an Android boot image (no ANDROID! magic)")
    kernel_size = struct.unpack_from("<I", data, 8)[0]
    start = PAGE
    end = start + kernel_size
    if end > len(data):
        sys.exit("boot image is shorter than its kernel_size")
    # The kernel field is a gzip member followed by the appended DTB, so a
    # plain gzip.decompress() would reject the DTB as a second member.
    z = zlib.decompressobj(16 + zlib.MAX_WBITS)
    return z.decompress(data[start:end]) + z.flush()


def payload_offset(buf):
    # The shim binary itself contains the magic as a constant, so take the last
    # occurrence: the payload is appended after the shim.
    off = buf.rfind(MAGIC)
    if off < 0:
        sys.exit("no SWEETXNU payload found")
    if off + CMDLINE_OFF + CMDLINE_LEN > len(buf):
        sys.exit("payload is truncated")
    return off


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--payload", metavar="FILE", help="shim/out/payload.bin")
    src.add_argument("--image", metavar="FILE", help="a built shim-boot.img")
    ap.add_argument("--require", action="append", default=[], metavar="TEXT",
                    help="exit 1 unless TEXT appears in the command line "
                         "(repeatable)")
    ap.add_argument("--reject", action="append", default=[], metavar="TEXT",
                    help="exit 1 if TEXT appears in the command line; used to "
                         "prove the fast image has no diagnostic switches "
                         "(repeatable)")
    args = ap.parse_args()

    with open(args.payload or args.image, "rb") as f:
        data = f.read()
    if args.image:
        data = kernel_image(data)
    off = payload_offset(data)
    raw = data[off + CMDLINE_OFF:off + CMDLINE_OFF + CMDLINE_LEN]
    line = raw.split(b"\0")[0].decode("latin1")

    print(f"payload at 0x{off:x}, {len(data) - off} bytes")
    print(f"xnu command line: {line}")
    if " rd=md0 " in f" {line} ":
        print("note: rd=md0 boots the ramdisk probe, not the iOS root",
              file=sys.stderr)

    missing = [need for need in args.require if need not in line]
    if missing:
        for need in missing:
            print(f"missing from the command line: {need}", file=sys.stderr)
        return 1
    unwanted = [bad for bad in args.reject if bad in line]
    if unwanted:
        for bad in unwanted:
            print(f"unexpected in the command line: {bad}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
