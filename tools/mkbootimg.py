#!/usr/bin/env python3
"""
Standalone Android Boot Image Header Version 0 packing utility.
Targeted for Qualcomm MSM8937 (santoni) ABL firmware and fastboot boot.
"""
import argparse
import hashlib
import os
import struct
import sys

BOOT_MAGIC = b"ANDROID!"
BOOT_MAGIC_SIZE = 8
BOOT_NAME_SIZE = 16
BOOT_ARGS_SIZE = 512
BOOT_EXTRA_ARGS_SIZE = 1024


def parse_int(val):
    if val is None:
        return 0
    if isinstance(val, int):
        return val
    return int(val, 0)


def pad_bytes(data, page_size):
    remainder = len(data) % page_size
    if remainder != 0:
        return data + b"\0" * (page_size - remainder)
    return data


def main():
    parser = argparse.ArgumentParser(description="Pack Android boot image v0")
    parser.add_argument("--kernel", required=True, help="path to kernel image")
    parser.add_argument("--ramdisk", default=None, help="path to ramdisk image")
    parser.add_argument("--second", default=None, help="path to second stage binary")
    parser.add_argument("--cmdline", default="", help="kernel command line")
    parser.add_argument("--base", default="0x80000000", help="base address (hex or int)")
    parser.add_argument("--kernel_offset", default="0x00008000", help="kernel offset from base")
    parser.add_argument("--ramdisk_offset", default="0x01000000", help="ramdisk offset from base")
    parser.add_argument("--second_offset", default="0x00f00000", help="second offset from base")
    parser.add_argument("--tags_offset", default="0x00000100", help="tags offset from base")
    parser.add_argument("--pagesize", default=2048, type=parse_int, help="page size (default 2048)")
    parser.add_argument("--header_version", default=0, type=parse_int, help="header version (default 0)")
    parser.add_argument("--os_version", default=0, type=parse_int, help="OS version")
    parser.add_argument("--os_patch_level", default=None, help="OS patch level (ignored in v0)")
    parser.add_argument("--output", "-o", required=True, help="output boot image path")

    args = parser.parse_args()

    base = parse_int(args.base)
    kernel_addr = base + parse_int(args.kernel_offset)
    ramdisk_addr = base + parse_int(args.ramdisk_offset)
    second_addr = base + parse_int(args.second_offset)
    tags_addr = base + parse_int(args.tags_offset)
    page_size = args.pagesize

    with open(args.kernel, "rb") as f:
        kernel_data = f.read()
    kernel_size = len(kernel_data)

    ramdisk_data = b""
    if args.ramdisk and os.path.exists(args.ramdisk):
        with open(args.ramdisk, "rb") as f:
            ramdisk_data = f.read()
    ramdisk_size = len(ramdisk_data)

    second_data = b""
    if args.second and os.path.exists(args.second):
        with open(args.second, "rb") as f:
            second_data = f.read()
    second_size = len(second_data)

    # Compute SHA-1 digest across components
    sha = hashlib.sha1()
    sha.update(kernel_data)
    sha.update(struct.pack("<I", kernel_size))
    sha.update(ramdisk_data)
    sha.update(struct.pack("<I", ramdisk_size))
    sha.update(second_data)
    sha.update(struct.pack("<I", second_size))
    digest = sha.digest().ljust(32, b"\0")

    # Command line encoding
    cmdline_bytes = args.cmdline.encode("utf-8")
    cmd_main = cmdline_bytes[:BOOT_ARGS_SIZE].ljust(BOOT_ARGS_SIZE, b"\0")
    cmd_extra = cmdline_bytes[BOOT_ARGS_SIZE:BOOT_ARGS_SIZE + BOOT_EXTRA_ARGS_SIZE].ljust(BOOT_EXTRA_ARGS_SIZE, b"\0")

    name_bytes = b"\0" * BOOT_NAME_SIZE

    # Header v0 structure: 8 + 40 + 16 + 512 + 32 + 1024 = 1632 bytes
    header = struct.pack(
        "<8s10I16s512s32s1024s",
        BOOT_MAGIC,
        kernel_size,
        kernel_addr,
        ramdisk_size,
        ramdisk_addr,
        second_size,
        second_addr,
        tags_addr,
        page_size,
        args.header_version,
        args.os_version,
        name_bytes,
        cmd_main,
        digest,
        cmd_extra
    )
    assert len(header) == 1632, f"Invalid header size: {len(header)} != 1632"

    with open(args.output, "wb") as out_f:
        out_f.write(pad_bytes(header, page_size))
        out_f.write(pad_bytes(kernel_data, page_size))
        if ramdisk_size > 0:
            out_f.write(pad_bytes(ramdisk_data, page_size))
        if second_size > 0:
            out_f.write(pad_bytes(second_data, page_size))

    print(f"Packed {args.output}: kernel={kernel_size}B, ramdisk={ramdisk_size}B, page={page_size}B")


if __name__ == "__main__":
    main()
