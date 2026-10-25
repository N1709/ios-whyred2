#!/usr/bin/env python3
"""Host tests for tools/check_payload.py and the DTB table parser it exports.

Builds synthetic FDT blobs (no dtc/libfdt needed on the runner) and a tiny
Mach-O header, then checks that the whyred/lavender matching rules, the
concatenated Image.gz-dtb table walk and the CLI exit statuses behave.
"""
import contextlib
import io
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))
import check_payload as cp  # noqa: E402

FDT_MAGIC = 0xD00DFEED


def u32(*values):
    return struct.pack(">%dI" % len(values), *values)


def pad4(blob):
    return blob + b"\0" * (-len(blob) % 4)


def make_fdt(msm_id, board_ids, model="Qualcomm Technologies, Inc. SDM 636 PM660 + PM660L E7S"):
    props = [
        ("#address-cells", u32(2)),
        ("#size-cells", u32(2)),
        ("model", model.encode() + b"\0"),
        ("compatible", b"qcom,sdm636-mtp\0qcom,sdm636\0\0"),
        ("qcom,msm-id", u32(msm_id, 0)),
        ("qcom,board-id", u32(*[v for b in board_ids for v in (b, 0)])),
    ]
    names = []
    strings = b""
    for name, _value in props:
        names.append((name, len(strings)))
        strings += name.encode() + b"\0"
    body = u32(1) + b"\0"                       # FDT_BEGIN_NODE "" (root)
    body = pad4(body)
    for (_name, _value), (prop, offset) in zip(props, names):
        body += u32(3, len(_value), offset) + _value
        body = pad4(body)
    body += u32(2)                              # FDT_END_NODE
    body += u32(9)                              # FDT_END
    off_struct = 0x38                           # 40-byte header + 16-byte rsvmap
    off_strings = off_struct + len(body)
    total = off_strings + len(strings)
    header = u32(FDT_MAGIC, total, off_struct, off_strings, 0x28, 17, 16, 0,
                 len(strings), len(body))
    return header + b"\0" * 16 + body + strings


def make_macho(path, magic=0xFEEDFACF, cputype=cp.CPU_TYPE_ARM64, filetype=cp.MH_EXECUTE):
    header = struct.pack("<IiiIIIII", magic, cputype, 0, filetype, 0, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(header + b"\0" * (cp.MIN_KERNEL_BYTES + 1))


def expect_value_error(fn, needle):
    try:
        fn()
    except ValueError as exc:
        assert needle in str(exc), "message %r lacks %r" % (str(exc), needle)
        return
    raise AssertionError("expected ValueError containing %r" % needle)


def main():
    whyred = make_fdt(0x159, (0x30008, 0x10008))
    lavender = make_fdt(0x13D, (0x20008,), model="Qualcomm Technologies, Inc. SDM 660")

    parsed = cp.parse_fdt(whyred)
    assert parsed["msm_ids"] == [0x159], parsed
    assert parsed["board_ids"] == [0x30008, 0x10008], parsed
    assert parsed["model"].endswith("E7S"), parsed
    assert parsed["totalsize"] == len(whyred)
    print("ok   parse_fdt reads msm-id/board-id/model from a synthetic DTB")

    # A boot image appends a whole table; the board match must pick the right
    # entry even when it is not first and the blob is not 4-byte aligned.
    table = b"\xAA" + lavender + whyred
    entries, end = cp.scan_dtbs(table, 1)
    assert len(entries) == 2 and end == len(table), (len(entries), end, len(table))
    hits = cp.matching_dtbs(entries, "whyred")
    assert len(hits) == 1 and hits[0]["offset"] == 1 + len(lavender), hits
    assert cp.matching_dtbs(entries, "lavender")[0]["offset"] == 1
    assert "SDM 636" in cp.describe(hits[0])
    print("ok   scan_dtbs walks an unaligned table and matching_dtbs selects by board")

    expect_value_error(lambda: cp.parse_fdt(b"nope" * 5), "no room for an FDT header")
    expect_value_error(lambda: cp.parse_fdt(b"\0\0\0\0" * 20), "bad FDT magic")
    expect_value_error(lambda: cp.parse_fdt(whyred[:-64]), "does not fit")
    broken = bytearray(whyred)
    struct.pack_into(">I", broken, 0x38, 0x7)   # invalid token at the root
    expect_value_error(lambda: cp.parse_fdt(bytes(broken)), "bad FDT token")
    print("ok   malformed, truncated and short blobs are rejected")

    with tempfile.TemporaryDirectory() as tmp:
        dtb = os.path.join(tmp, "whyred_dtb.bin")
        with open(dtb, "wb") as f:
            f.write(whyred)
        assert cp.check_dtb(dtb, "whyred").startswith(dtb)
        expect_value_error(lambda: cp.check_dtb(dtb, "lavender"), "has no lavender")

        kernel = os.path.join(tmp, "kernel")
        make_macho(kernel)
        size, warnings = cp.check_kernel(kernel, "whyred")
        assert size > cp.MIN_KERNEL_BYTES and warnings == [], (size, warnings)
        expect_value_error(lambda: cp.check_kernel(dtb, "whyred"), "not a 64-bit Mach-O")
        stub = os.path.join(tmp, "stub")
        make_macho(stub, filetype=1)
        expect_value_error(lambda: cp.check_kernel(stub, "whyred"), "expected MH_EXECUTE")
        make_macho(stub, cputype=0x01000007)
        expect_value_error(lambda: cp.check_kernel(stub, "whyred"), "expected arm64")
        print("ok   check_kernel accepts an arm64 executable and rejects stubs/wrong CPUs")

        argv = ["-q", "--board", "whyred", "--kernel", kernel, "--dtb", dtb]
        quiet = (io.StringIO(), io.StringIO())
        with contextlib.redirect_stdout(quiet[0]), contextlib.redirect_stderr(quiet[1]):
            assert cp.main(argv) == 0, "valid whyred pair must pass"
            assert cp.main(["-q", "--board", "whyred", "--dtb", dtb]) == 0
            assert cp.main(["-q", "--board", "lavender", "--dtb", dtb]) == 1
            assert cp.main(["-q", "--board", "whyred", "--kernel", dtb]) == 1
    print("ok   CLI exits 0 for a matching board and 1 for a mismatch")
    print("test_payload: all checks passed")


if __name__ == "__main__":
    main()
