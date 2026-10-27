#!/usr/bin/env python3
"""Host test for tools/payload_cmdline.py.

The point of the tool is that the shim reads the command line from the payload,
not from the boot.img header. Build a decoy boot.img whose header says one
thing and whose payload says another, and check that the tool reports the
payload's line.
"""

import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "payload_cmdline.py"
MAGIC = b"SWEETXNU"
CMDLINE_OFF = 0x48
CMDLINE_LEN = 608


def make_payload(cmdline):
    b = bytearray(0x1000)
    b[:8] = MAGIC
    struct.pack_into("<I", b, 8, 1)       # version
    line = cmdline.encode()
    b[CMDLINE_OFF:CMDLINE_OFF + len(line)] = line
    return bytes(b)


def gzip_member(data):
    c = zlib.compressobj(9, zlib.DEFLATED, 16 + zlib.MAX_WBITS)
    return c.compress(data) + c.flush()


def make_image(payload, header_cmdline):
    # A decoy magic in the shim body, before the real payload.
    shim = bytearray(0x2000)
    shim[0x100:0x108] = MAGIC
    body = bytes(shim) + payload
    gz = gzip_member(body)
    header = bytearray(4096)
    header[:8] = b"ANDROID!"
    struct.pack_into("<I", header, 8, len(gz))
    hdr = header_cmdline.encode()
    header[64:64 + len(hdr)] = hdr
    return bytes(header) + gz


def run(*args):
    return subprocess.run([sys.executable, str(TOOL), *args],
                          capture_output=True, text=True)


def main():
    good = "rd=emmcroot0 emmcrw=1 cs_debug=2"
    payload = make_payload(good)
    image = make_image(payload, "rd=md0")

    with tempfile.TemporaryDirectory() as td:
        pp = pathlib.Path(td) / "payload.bin"
        ip = pathlib.Path(td) / "boot.img"
        pp.write_bytes(payload)
        ip.write_bytes(image)

        r = run("--payload", str(pp), "--require", "rd=emmcroot0")
        assert r.returncode == 0, r.stderr
        assert good in r.stdout, r.stdout

        # The image's header says rd=md0; the payload must win.
        r = run("--image", str(ip), "--require", "emmcrw=1")
        assert r.returncode == 0, r.stderr
        assert "rd=emmcroot0" in r.stdout, r.stdout

        r = run("--image", str(ip), "--require", "cs_debug=2",
                "--require", "launchd_unsecure_cache=1")
        assert r.returncode == 1, r.stdout + r.stderr
        assert "missing from the command line" in r.stderr, r.stderr

        r = run("--image", str(ip))
        assert "rd=md0 boots the ramdisk probe" not in r.stdout + r.stderr
        assert "rd=emmcroot0" in r.stdout

        # --reject proves a "fast" image carries no diagnostic switch.
        r = run("--image", str(ip), "--reject", "cs_debug=2")
        assert r.returncode == 1, r.stdout + r.stderr
        assert "unexpected in the command line: cs_debug=2" in r.stderr
        r = run("--image", str(ip), "--reject=-enable_kprintf_spam")
        assert r.returncode == 0, r.stderr

    print("test_payload_cmdline: payload wins over the boot.img header")
    return 0


if __name__ == "__main__":
    sys.exit(main())
