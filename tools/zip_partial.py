#!/usr/bin/env python3
"""Extract kernel.development.<board> from a partially downloaded zip.

  zip_partial.py PARTIAL.zip OUT_KERNEL

The GitHub Actions `xnu-kernel` artifact is ~60 MB because it also carries the
dSYM, the KDK symbol dumps and check_patch.log, while the kernel itself is
~7 MB compressed and sits early in the archive. This walks the local file
headers, inflates entry by entry, and stops as soon as the kernel is complete,
so a slow download can be killed the moment the kernel is on disk (the CI
download path has been observed at 60 KB/s).

An entry is only accepted when its data descriptor (or the local header, when
bit 3 of the flags is clear) agrees with the bytes that were inflated: same
CRC-32, same compressed size, same uncompressed size. A partial download that
ends mid-stream must never be written out as a kernel, and the sizes also
prove the extracted file is the whole entry and not a prefix of it. When the
archive carries SHA256SUMS the kernel is checked against it as well.

Exit status: 0 kernel written, 2 kernel not complete yet (need more bytes),
1 malformed archive / kernel absent.
"""
import hashlib
import struct
import sys
import zlib

MAGIC = b"PK\x03\x04"
DESC = b"PK\x07\x08"
ZIP64 = 0xFFFFFFFF
CPU_TYPE_ARM64 = 0x0100000C
MH_EXECUTE = 2
MIN_KERNEL_SIZE = 4 << 20


def parse_descriptor(data, pos):
    """Return (crc, csize, usize, next_pos), or None if not all here yet."""
    avail = len(data) - pos
    if data[pos:pos + 4] == DESC:
        pos += 4
        avail -= 4
    if avail < 12:
        return None
    crc, csize, usize = struct.unpack_from("<III", data, pos)
    pos += 12
    if csize == ZIP64 or usize == ZIP64:
        if len(data) - pos < 16:
            return None
        csize, usize = struct.unpack_from("<QQ", data, pos)
        pos += 16
    return crc, csize, usize, pos


def entries(data, sums):
    """Yield (name, plain) per entry; plain is None when the entry is short."""
    pos = 0
    while pos + 30 <= len(data) and data[pos:pos + 4] == MAGIC:
        # 0 sig, 4 version, 6 flags, 8 method, 10 mtime, 12 mdate, 14 crc,
        # 18 compressed size, 22 uncompressed size.
        flag, method = struct.unpack_from("<HH", data, pos + 6)
        crc, csize, usize = struct.unpack_from("<III", data, pos + 14)
        nlen, elen = struct.unpack_from("<HH", data, pos + 26)
        if pos + 30 + nlen + elen > len(data):
            return
        name = data[pos + 30:pos + 30 + nlen].decode("utf-8", "replace")
        start = pos + 30 + nlen + elen

        if method == 0:
            if csize == 0 or start + csize > len(data):
                yield name, None, False
                return
            plain = data[start:start + csize]
            end = start + csize
            if flag & 0x8:
                desc = parse_descriptor(data, end)
                if desc is None:
                    yield name, None, False
                    return
                end = desc[3]
            if name.endswith("SHA256SUMS"):
                sums.update(parse_sums(plain))
            yield name, plain, True
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
            yield name, None, False
            return
        used = len(data[start:]) - len(out.unused_data)
        end = start + used

        if flag & 0x8:
            desc = parse_descriptor(data, end)
            if desc is None:
                yield name, None, False
                return
            dcrc, dcsize, dusize, end = desc
        else:
            dcrc, dcsize, dusize = crc, csize, usize
        if dcrc != zlib.crc32(plain) or dcsize != used or dusize != len(plain):
            raise ValueError(
                "%s: entry does not match its descriptor (crc %08x/%08x, "
                "compressed %d/%d, plain %d/%d)"
                % (name, zlib.crc32(plain), dcrc, used, dcsize,
                   len(plain), dusize))

        if name.endswith("SHA256SUMS"):
            sums.update(parse_sums(plain))
        yield name, plain, True
        pos = end


def parse_sums(blob):
    table = {}
    for line in blob.decode("utf-8", "replace").splitlines():
        parts = line.split()
        if len(parts) == 2 and len(parts[0]) == 64:
            table[parts[1].lstrip("*")] = parts[0].lower()
    return table


def looks_like_kernel(plain):
    if len(plain) < MIN_KERNEL_SIZE:
        return "only %d bytes" % len(plain)
    magic = struct.unpack_from("<I", plain, 0)[0]
    if magic not in (0xFEEDFACF, 0xCFFAEDFE):
        return "not a Mach-O"
    cputype, _sub, filetype = struct.unpack_from("<iiI", plain, 4)
    if cputype != CPU_TYPE_ARM64:
        return "cputype %#x is not arm64" % cputype
    if filetype != MH_EXECUTE:
        return "filetype %d is not MH_EXECUTE" % filetype
    if b"Darwin Kernel Version" not in plain:
        return "no Darwin Kernel Version string"
    return None


def main(argv):
    if len(argv) != 3:
        sys.exit("usage: zip_partial.py PARTIAL.zip OUT_KERNEL")
    with open(argv[1], "rb") as f:
        blob = f.read()
    sums = {}
    try:
        for name, plain, _complete in entries(blob, sums):
            if "/" in name or not name.startswith("kernel.development."):
                continue
            if plain is None:
                return 2
            problem = looks_like_kernel(plain)
            if problem:
                sys.exit("%s: not a usable kernel (%s)" % (name, problem))
            digest = hashlib.sha256(plain).hexdigest()
            if name in sums and sums[name] != digest:
                sys.exit("%s: sha256 %s does not match SHA256SUMS (%s)"
                         % (name, digest, sums[name]))
            with open(argv[2], "wb") as f:
                f.write(plain)
            print("%s: %d bytes sha256 %s%s" % (
                name, len(plain), digest,
                "" if name in sums else " (no SHA256SUMS entry yet)"))
            return 0
    except ValueError as exc:
        sys.exit(str(exc))
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
