#!/usr/bin/env python3
"""Extract the Synaptics DSX touch firmware image embedded in a whyred boot image.

The OrangeFox recovery kernel (and the stock Android boot image) carry the
touch firmware as a `const unsigned char[]` array, because the vendor driver is
built with CONFIG_TOUCHSCREEN_SYNAPTICS_DSX_FW_IMAGE. That is why
`/sys/class/input/inputN/doreflash` has an image to parse even though no file
exists under /vendor/firmware.

The array is a plain DSX v05/v06 image: a 0x100 byte header followed by the
bootloader, UI firmware, UI configuration and (when present) the display
configuration. See `struct image_header_05_06` and
`fwu_parse_image_header_05_06()` in the vendor driver.

Usage:
    tools/extract-ts-firmware.py RECOVERY.IMG [-o OUTDIR] [--firmware-id N]
                                    [--list]

Default output: out/ts-fw-whyred.img (out/ is git-ignored).
"""

import argparse
import os
import re
import struct
import zlib

MAGIC = b"ANDROID!"
IMAGE_AREA_OFFSET = 0x100
BLOCK = 256


def parse_boot(data):
    """Return (kernel_bytes, page_size) from an Android boot image, v0-v2."""
    if data[:8] != MAGIC:
        raise SystemExit("not an Android boot image (no ANDROID! magic)")
    kernel_size = struct.unpack_from("<I", data, 8)[0]
    page_size = struct.unpack_from("<I", data, 36)[0] or 2048
    # v1/v2 use the same kernel offset, only the header size differs.
    return data[page_size:page_size + kernel_size], page_size


def find_kernel(payload):
    """Decompress the kernel if needed; return the raw kernel image."""
    # gzip / zlib, both handled by wbits=47 (auto-detect gzip or zlib header).
    try:
        return zlib.decompressobj(47).decompress(payload)
    except zlib.error:
        pass
    for magic, name in ((b"\xfd7zXZ\x00", "xz"), (b"\x02\x21\x4c\x18", "lz4")):
        if payload.startswith(magic):
            raise SystemExit("kernel is %s-compressed; not supported" % name)
    if payload[:1] == b"\x1f":
        raise SystemExit("kernel looks gzip but would not decompress")
    return payload  # already a raw Image/zImage


def u32(b, off):
    return struct.unpack_from("<I", b, off)[0]


def decode_candidate(blob, off):
    """Validate a v05/v06 header at blob[off:]; return a dict or None."""
    if off + 0x54 > len(blob):
        return None
    hdr = blob[off:off + 0x54]
    options = hdr[6]
    version = hdr[7]
    if version not in (5, 6):
        return None

    fw_size = u32(hdr, 0x08)
    cfg_size = u32(hdr, 0x0C)
    if fw_size == 0 or fw_size % BLOCK or fw_size > 0x400000:
        return None
    if cfg_size == 0 or cfg_size % BLOCK or cfg_size > 0x100000:
        return None

    product_id = hdr[0x10:0x18].split(b"\x00")[0]
    if not product_id or len(product_id) > 8:
        return None
    if not all(32 <= c < 127 for c in product_id):
        return None

    boot_addr, boot_size, ui_addr, ui_size = struct.unpack_from("<IIII", hdr, 0x20)
    if boot_size % BLOCK or ui_size != fw_size:
        return None
    if ui_addr != IMAGE_AREA_OFFSET + (boot_size if options & 0x02 else 0):
        return None

    dsp_addr, dsp_size = struct.unpack_from("<II", hdr, 0x40)
    has_disp = bool(options & 0x02) or bool(options & 0x08)  # bootloader / tddi
    if has_disp:
        if dsp_size % BLOCK or dsp_addr < ui_addr + fw_size:
            return None
        total = dsp_addr + dsp_size
    else:
        total = IMAGE_AREA_OFFSET + boot_size + fw_size + cfg_size

    if total % BLOCK or off + total > len(blob):
        return None

    ds_id = hdr[0x30:0x40].split(b"\x00")[0].decode("ascii", "replace")
    return {
        "offset": off,
        "size": total,
        "product_id": product_id.decode("ascii"),
        "ds_id": ds_id,
        "header_version": version,
        "options": options,
        "firmware_size": fw_size,
        "config_size": cfg_size,
        "bootloader_size": boot_size,
        "disp_config_size": dsp_size,
        "firmware_id": u32(hdr, 0x50) if options & 0x01 else 0,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image", help="boot/recovery image with embedded DSX firmware")
    ap.add_argument("-o", "--outdir", default=os.path.join(os.path.dirname(__file__), "..", "out"))
    ap.add_argument("--firmware-id", type=lambda s: int(s, 0),
                    help="only extract the image with this firmware_id")
    ap.add_argument("--list", action="store_true", help="list candidates, do not write")
    args = ap.parse_args()

    data = open(args.image, "rb").read()
    kernel, page = parse_boot(data)
    blob = find_kernel(kernel)

    candidates = []
    # Cheap pre-filter: major_header_version (byte 7) is 5 or 6, and both
    # firmware_size (0x08) and config_size (0x0C) are block multiples, so
    # their low bytes are 0.
    for m in re.finditer(rb"(?s).{7}[\x05\x06]\x00.{3}\x00.{3}", blob):
        cand = decode_candidate(blob, m.start())
        if cand:
            candidates.append(cand)

    if not candidates:
        raise SystemExit("no embedded DSX firmware image found")

    print("%d candidate image(s):" % len(candidates))
    for c in candidates:
        print("  offset 0x%08x  size %6d  fw_id %d  product %s  ds_id %s  hdr v%d" % (
            c["offset"], c["size"], c["firmware_id"], c["product_id"], c["ds_id"],
            c["header_version"]))

    if args.list:
        return

    if args.firmware_id is not None:
        selected = [c for c in candidates if c["firmware_id"] == args.firmware_id]
        if not selected:
            raise SystemExit("no candidate with firmware_id %d" % args.firmware_id)
    else:
        selected = candidates[:1]

    os.makedirs(args.outdir, exist_ok=True)
    for c in selected:
        out = os.path.join(args.outdir, "ts-fw-whyred.img")
        with open(out, "wb") as f:
            f.write(blob[c["offset"]:c["offset"] + c["size"]])
        print("wrote %s (%d bytes, firmware_id %d)" % (out, c["size"], c["firmware_id"]))
        print("sha256:", end=" ")
        import hashlib
        print(hashlib.sha256(open(out, "rb").read()).hexdigest())


if __name__ == "__main__":
    main()
