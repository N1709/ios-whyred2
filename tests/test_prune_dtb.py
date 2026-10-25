#!/usr/bin/env python3
"""Host test for tools/prune_dtb.py.

The pruned blob must still parse as a valid FDT, keep the root qcom,msm-id /
qcom,board-id properties that ABL matches on, and drop the nodes that were not
named in --keep.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)

import check_payload                                        # noqa: E402
import prune_dtb                                            # noqa: E402
from test_payload import make_fdt                            # noqa: E402


def main():
    src = make_fdt(0x159, [0x30008])
    root, rsv, version, last_comp, boot_cpuid = prune_dtb.parse(src)

    soc = {"name": "soc", "props": [("#address-cells", b"\0\0\0\2")],
           "children": [{"name": "camera@1", "props": [("reg", b"\0" * 4000)],
                         "children": []}]}
    chosen = {"name": "chosen", "props": [("linux,initrd-start", b"\0" * 8)],
              "children": []}
    root["children"] = [soc, chosen]
    blob = prune_dtb.build(root, rsv, version, last_comp, boot_cpuid)

    info = check_payload.parse_fdt(blob)
    assert info["msm_ids"] == [0x159], info
    assert info["board_ids"] == [0x30008], info
    assert any(n["name"] == "soc" for n in root["children"])

    with tempfile.TemporaryDirectory() as td:
        src_path = os.path.join(td, "in.dtb")
        dst_path = os.path.join(td, "out.dtb")
        with open(src_path, "wb") as f:
            f.write(blob)
        subprocess.check_call([
            sys.executable, os.path.join(HERE, "..", "tools", "prune_dtb.py"),
            src_path, dst_path, "--keep", "chosen"])
        with open(dst_path, "rb") as f:
            pruned = f.read()

    assert len(pruned) < len(blob) // 2, (len(pruned), len(blob))
    assert b"camera@1" not in pruned
    assert b"chosen" in pruned
    info = check_payload.parse_fdt(pruned)
    assert info["msm_ids"] == [0x159], info
    assert info["board_ids"] == [0x30008], info
    print("test_prune_dtb: all checks passed")


if __name__ == "__main__":
    main()
