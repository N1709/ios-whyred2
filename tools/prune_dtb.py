#!/usr/bin/env python3
"""Shrink an appended DTB to the nodes the bootloader and shim actually read.

  prune_dtb.py IN.dtb OUT.dtb [--keep chosen,memory,cpus,...] [--list]

whyred's ABL rejects a single fastboot download over 6 MiB, and the vendor
sdm636-mtp-whyred DTB is 444 KiB of camera/display/modem nodes that neither
the ABL matcher nor the shim uses: the ABL matches the root qcom,msm-id /
qcom,board-id properties, and shim/fdt.c only walks /chosen (the payload is
embedded, so even that is a fallback). XNU gets its device tree from the ADT
inside the payload, not from this blob.

The output keeps every root property, the reservation map, and the top-level
nodes named by --keep (default: chosen, memory, cpus, reserved-memory,
aliases). Parse the result with tools/check_payload.py to confirm the board
ids survive.
"""
import argparse
import struct
import sys

FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9
DEFAULT_KEEP = ("chosen", "memory", "cpus", "reserved-memory", "aliases")


def align4(n):
    return (n + 3) & ~3


def read_cstr(blob, pos, what):
    nul = blob.find(b"\0", pos)
    if nul < 0:
        raise ValueError("unterminated %s at %#x" % (what, pos))
    return blob[pos:nul].decode("ascii", "replace"), nul + 1


def parse(blob):
    (magic, totalsize, off_struct, off_strings, off_rsv, version, last_comp,
     boot_cpuid, size_strings, size_struct) = struct.unpack_from(">10I", blob, 0)
    if magic != FDT_MAGIC:
        raise ValueError("bad FDT magic %#010x" % magic)
    if totalsize > len(blob):
        raise ValueError("totalsize %d exceeds the %d-byte file" % (totalsize, len(blob)))
    strings = blob[off_strings:off_strings + size_strings]
    pos = off_struct
    end = off_struct + size_struct if size_struct else totalsize

    def name_at(off):
        if off >= len(strings):
            raise ValueError("property name offset %#x outside the strings block" % off)
        return read_cstr(strings, off, "property name")[0]

    def node():
        nonlocal pos
        token = struct.unpack_from(">I", blob, pos)[0]
        if token != FDT_BEGIN_NODE:
            raise ValueError("expected FDT_BEGIN_NODE at %#x, got %#x" % (pos, token))
        pos += 4
        name, pos = read_cstr(blob, pos, "node name")
        pos = align4(pos)
        props, children = [], []
        while True:
            token = struct.unpack_from(">I", blob, pos)[0]
            pos += 4
            if token == FDT_PROP:
                length, nameoff = struct.unpack_from(">II", blob, pos)
                pos += 8
                props.append((name_at(nameoff), blob[pos:pos + length]))
                pos = align4(pos + length)
            elif token == FDT_BEGIN_NODE:
                pos -= 4
                children.append(node())
            elif token == FDT_END_NODE:
                return {"name": name, "props": props, "children": children}
            elif token == FDT_NOP:
                continue
            else:
                raise ValueError("unexpected token %#x at %#x" % (token, pos - 4))

    pos = off_struct
    root = node()
    if struct.unpack_from(">I", blob, pos)[0] != FDT_END:
        raise ValueError("struct block does not end with FDT_END")
    rsv = []
    p = off_rsv
    while True:
        addr, size = struct.unpack_from(">QQ", blob, p)
        p += 16
        if addr == 0 and size == 0:
            break
        rsv.append((addr, size))
    return root, rsv, version, last_comp, boot_cpuid


def build(root, rsv, version, last_comp, boot_cpuid):
    strings = bytearray()
    str_off = {}

    def stridx(name):
        if name not in str_off:
            str_off[name] = len(strings)
            strings.extend(name.encode("ascii") + b"\0")
        return str_off[name]

    struct_block = bytearray()

    def emit(n):
        struct_block.extend(struct.pack(">I", FDT_BEGIN_NODE))
        struct_block.extend(n["name"].encode("ascii") + b"\0")
        struct_block.extend(b"\0" * (align4(len(struct_block)) - len(struct_block)))
        for name, value in n["props"]:
            struct_block.extend(struct.pack(">III", FDT_PROP, len(value), stridx(name)))
            struct_block.extend(value)
            struct_block.extend(b"\0" * (align4(len(struct_block)) - len(struct_block)))
        for child in n["children"]:
            emit(child)
        struct_block.extend(struct.pack(">I", FDT_END_NODE))

    emit(root)
    struct_block.extend(struct.pack(">I", FDT_END))

    rsv_block = b"".join(struct.pack(">QQ", a, s) for a, s in rsv) + b"\0" * 16
    off_rsv = 40
    off_struct = align4(off_rsv + len(rsv_block))
    off_strings = align4(off_struct + len(struct_block))
    totalsize = align4(off_strings + len(strings))
    header = struct.pack(">10I", FDT_MAGIC, totalsize, off_struct, off_strings,
                         off_rsv, version, last_comp, boot_cpuid,
                         len(strings), len(struct_block))
    out = bytearray(header)
    out.extend(b"\0" * (off_rsv - len(out)))
    out.extend(rsv_block)
    out.extend(b"\0" * (off_struct - len(out)))
    out.extend(struct_block)
    out.extend(b"\0" * (off_strings - len(out)))
    out.extend(strings)
    out.extend(b"\0" * (totalsize - len(out)))
    return bytes(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst", nargs="?")
    ap.add_argument("--keep", default=",".join(DEFAULT_KEEP),
                    help="top-level node names to keep (default: %(default)s)")
    ap.add_argument("--list", action="store_true", help="list top-level nodes and sizes")
    args = ap.parse_args(argv)

    blob = open(args.src, "rb").read()
    root, rsv, version, last_comp, boot_cpuid = parse(blob)

    if args.list:
        print("%-24s %8s  %s" % ("root prop/node", "bytes", "kept"))
        for name, value in root["props"]:
            print("%-24s %8d  yes" % (name, len(value)))
        keep = set(n for n in args.keep.split(",") if n)
        for child in root["children"]:
            size = len(build({"name": "", "props": [], "children": [child]}, rsv,
                             version, last_comp, boot_cpuid))
            print("%-24s %8d  %s" % (child["name"] or "(unnamed)", size,
                                     "yes" if child["name"] in keep else "no"))
        return 0

    if not args.dst:
        ap.error("OUT.dtb is required unless --list is used")
    keep = set(n for n in args.keep.split(",") if n)
    kept = [c for c in root["children"] if c["name"] in keep]
    dropped = [c["name"] for c in root["children"] if c["name"] not in keep]
    out = build({"name": "", "props": root["props"], "children": kept}, rsv,
                version, last_comp, boot_cpuid)
    open(args.dst, "wb").write(out)
    print("kept %s" % (", ".join(c["name"] for c in kept) or "(none)"))
    print("dropped %d nodes: %s" % (len(dropped), ", ".join(dropped) or "(none)"))
    print("%s: %d -> %d bytes (%.1f%%)" % (args.dst, len(blob), len(out),
                                          100.0 * len(out) / len(blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
