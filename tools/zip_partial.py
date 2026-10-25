#!/usr/bin/env python3
"""Extract kernel.development.<board> from a partially downloaded zip.

  zip_partial.py PARTIAL.zip OUT_KERNEL

The GitHub Actions `xnu-kernel` artifact is ~60 MB because it also carries the
dSYM, the KDK symbol dumps and check_patch.log, while the kernel itself is
~7 MB compressed and sits early in the archive. This walks the local file
headers, inflates entry by entry, and stops as soon as the kernel is complete,
so a slow download can be killed the moment the kernel is on disk (the CI
download path has been observed at 60 KB/s).

Exit status: 0 kernel written, 2 kernel not complete yet (need more bytes),
1 malformed archive / kernel absent.
"""
import struct
import sys
import zlib

MAGIC = b"PK\x03\x04"
DESC = b"PK\x07\x08"


def entries(data):
    pos = 0
    while pos + 30 <= len(data) and data[pos:pos + 4] == MAGIC:
        flag, method = struct.unpack_from("<HH", data, pos + 6)
        nlen, elen = struct.unpack_from("<HH", data, pos + 26)
        name = data[pos + 30:pos + 30 + nlen].decode("utf-8", "replace")
        start = pos + 30 + nlen + elen
        if method == 0:
            yield name, None, start, None
            end = data.find(MAGIC, start)
            if end < 0:
                return
            pos = end
            continue
        if method != 8:
            raise ValueError("%s: unsupported compression method %d" % (name, method))
        out = zlib.decompressobj(-15)
        try:
            plain = out.decompress(data[start:])
        except zlib.error as exc:
            raise ValueError("%s: corrupt deflate stream (%s)" % (name, exc))
        if not out.eof:
            yield name, plain, start, None      # truncated: more bytes needed
            return
        yield name, plain, start, len(data[start:]) - len(out.unused_data)
        # data descriptor (flag bit 3) or sizes in the local header
        pos = start + (len(data[start:]) - len(out.unused_data))
        if data[pos:pos + 4] == DESC:
            pos += 16
        elif not flag & 0x8:
            pos = start + (len(data[start:]) - len(out.unused_data))
        else:
            pos += 16


def main(argv):
    if len(argv) != 3:
        sys.exit("usage: zip_partial.py PARTIAL.zip OUT_KERNEL")
    blob = open(argv[1], "rb").read()
    try:
        for name, plain, _start, _used in entries(blob):
            if not name.startswith("kernel.development."):
                continue
            if plain is None:
                return 2
            if plain[:4] not in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf"):
                sys.exit("%s is not a Mach-O" % name)
            with open(argv[2], "wb") as f:
                f.write(plain)
            print("%s: %d bytes" % (name, len(plain)))
            return 0
    except ValueError as exc:
        sys.exit(str(exc))
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
