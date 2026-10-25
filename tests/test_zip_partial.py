#!/usr/bin/env python3
"""Host test for tools/zip_partial.py.

The fetch path is only allowed to write a kernel once the entry's own
descriptor proves the whole entry was inflated: a truncated download must
return 2 and leave nothing behind, a prefix of the kernel must never be
accepted, a wrong SHA256SUMS entry must fail, and a stub Mach-O must not be
mistaken for the kernel.
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "tools", "zip_partial.py")

KERNEL = "kernel.development.whyred"


def fake_kernel(size=5 << 20):
    """An arm64 MH_EXECUTE big enough to pass looks_like_kernel()."""
    blob = bytearray(size)
    struct.pack_into("<IiiIIII", blob, 0, 0xFEEDFACF, 0x0100000C, 0, 2, 0, 0, 0)
    blob[4096:4096 + len(b"Darwin Kernel Version 22.6.0: test\0")] = \
        b"Darwin Kernel Version 22.6.0: test\0"
    return bytes(blob)


def run(archive, out):
    proc = subprocess.run([sys.executable, TOOL, archive, out],
                          capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


def main():
    kernel = fake_kernel()
    digest = hashlib.sha256(kernel).hexdigest()
    sums = ("%s  %s\n" % (digest, KERNEL)).encode()

    with tempfile.TemporaryDirectory() as td:
        full = os.path.join(td, "full.zip")
        with zipfile.ZipFile(full, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("SHA256SUMS", sums)
            z.writestr("dSYM/kernel.development.whyred/Contents/Info.plist", b"x")
            z.writestr(KERNEL, kernel)

        out = os.path.join(td, "out.bin")
        rc, text = run(full, out)
        assert rc == 0, (rc, text)
        assert os.path.getsize(out) == len(kernel), os.path.getsize(out)
        assert "sha256 " + digest in text, text

        # Half the archive: the kernel entry is cut mid-stream.
        blob = open(full, "rb").read()
        half = os.path.join(td, "half.zip")
        with open(half, "wb") as f:
            f.write(blob[:len(blob) // 2])
        cut = os.path.join(td, "cut.bin")
        rc, text = run(half, cut)
        assert rc == 2, (rc, text)
        assert not os.path.exists(cut), "a partial download wrote a kernel"

        # Every prefix of the real file must be rejected the same way.
        for frac in (0.1, 0.5, 0.9):
            part = os.path.join(td, "part.bin")
            with open(part, "wb") as f:
                f.write(blob[:int(len(blob) * frac)])
            rc, _text = run(part, cut)
            assert rc in (0, 2), rc
            if rc == 2:
                assert not os.path.exists(cut)
            else:
                assert os.path.getsize(cut) == len(kernel)

        # A SHA256SUMS table that disagrees must not be written out.
        bad = os.path.join(td, "bad.zip")
        with zipfile.ZipFile(bad, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("SHA256SUMS", ("%s  %s\n" % ("0" * 64, KERNEL)).encode())
            z.writestr(KERNEL, kernel)
        rc, text = run(bad, cut)
        assert rc == 1 and "SHA256SUMS" in text, (rc, text)

        # A dSYM DWARF file has kernel section names but is not the kernel.
        stub = os.path.join(td, "stub.zip")
        with zipfile.ZipFile(stub, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr(KERNEL, kernel[:8192])
        rc, text = run(stub, cut)
        assert rc == 1 and "not a usable kernel" in text, (rc, text)

    print("test_zip_partial: all checks passed")


if __name__ == "__main__":
    main()
