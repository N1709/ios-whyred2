#!/bin/bash
# Build the freestanding shim and wrap it in an Android boot.img v0 for MSM8937 (santoni).
set -euo pipefail
cd "$(dirname "$0")"

CC="${CC:-clang}"
OBJCOPY="${OBJCOPY:-llvm-objcopy}"
READELF="${READELF:-llvm-readelf}"
NM="${NM:-llvm-nm}"
LD="${LD:-ld.lld}"
HOST_CC="${HOST_CC:-clang}"

CFLAGS="-target aarch64-linux-gnu -O2 -ffreestanding -nostdlib -fno-pic -fno-jump-tables -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"

mkdir -p out

for f in start.S shim.c console.c mmu.c fdt.c loader.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done

# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$($READELF -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }

$LD -T shim.ld --build-id=none -N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o -o out/shim.elf

$OBJCOPY -O binary out/shim.elf out/Image

# Pad to image_size so .bss/stacks exist as zeroed bytes.
size=$((16#$($NM out/shim.elf | awk '$3=="_end"{print $1}')))
truncate -s "$size" out/Image

# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio -> diagnostics.
if [ -n "${1:-}" ]; then
	truncate -s $(( (size + 0xfff) & ~0xfff )) out/Image
	cat "$1" >> out/Image
	python3 -c 'import os,struct,sys; f=open(sys.argv[1],"r+b"); f.seek(16); f.write(struct.pack("<Q", os.path.getsize(sys.argv[1])))' out/Image
fi

if command -v cpio >/dev/null 2>&1; then
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
else
	python3 -c 'import gzip; h=b"07070100000000000000000000000000000000000000010000000000000000000000000000000000000000000000000000000b00000000TRAILER!!!\0\0\0\0"; open("out/ramdisk.bin","wb").write(gzip.compress(h + b"\0"*(512-len(h)), compresslevel=9))'
fi

# Append Santoni Qualcomm device tree table (29 DTBs including msm8940-xiaomi-santoni)
# so Qualcomm LK finds the DTB at kernel_addr + image_size
if [ -f ../resources/santoni_appended_dtbs.bin ]; then
	cat ../resources/santoni_appended_dtbs.bin >> out/Image
fi

# Packaging: wrap in boot.img v0 using tools/mkbootimg.py
python3 ../tools/mkbootimg.py \
	--kernel out/Image \
	--ramdisk out/ramdisk.bin \
	--output out/shim-boot.img \
	--base 0x80000000 \
	--pagesize 2048 \
	--header_version 0 \
	--cmdline "console=null"

# Host render of the console layout
$HOST_CC -O2 -Wall -I. -o out/host_test console.c host_test.c && ./out/host_test

ls -l out/Image out/shim-boot.img
