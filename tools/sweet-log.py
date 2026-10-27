#!/usr/bin/env python3
"""Decode the SWEET / SANTONI persistent console log (kernel patches 0047 / 0143).

  sweet-log.py AREA.bin [OUT.txt] [--salvage]

AREA.bin is extracted via tools/sweet-log-pull.sh:
- Santoni: First 8193 blocks (512B) of the idle SYSTEM partition.
  Block 0 is the 512B header ("SANTONILOG1", u32 version, u32 ring size,
  u64 total bytes, u64 flush seq), followed by the 4 MiB circular ring buffer.
- Sweet: Last 1025 blocks (4096B) of USERDATA partition.
  Block 0 is the 4096B header ("SWEETLOG", ...), followed by the ring.

Byte n of the console stream is at ring offset n % size.

--salvage reads an area whose header never reached the disk. The 0177 boot
left 26 blocks of real text and a zero sector 0, because the header write that
followed the first flush failed; refusing that area threw the log away. In
salvage mode the ring starts at 512 and the text is the leading run of blocks
that contain printable characters (the flusher writes whole blocks, the rest
of the area is zero or stale partition data).
"""
import struct
import sys

KERNEL_MARKS = (b'Darwin Kernel Version', b'iBoot version', b'WHYRED SDHCI')

args = [a for a in sys.argv[1:] if not a.startswith('--')]
flags = [a for a in sys.argv[1:] if a.startswith('--')]
if not args:
    sys.exit('usage: sweet-log.py AREA.bin [OUT.txt] [--salvage]')

data = open(args[0], 'rb').read()
salvaged = False

if data.startswith(b'SANTONILOG1'):
    magic, version, size, total, seq = struct.unpack_from('<12sIIQQ', data, 0)
    hdr_size = 512
elif data.startswith(b'SWEETLOG'):
    magic, version, size, total, seq = struct.unpack_from('<8sIIQQ', data, 0)
    hdr_size = 4096
else:
    if '--salvage' not in flags:
        sys.exit('no SWEETLOG or SANTONILOG1 header (log never flushed, or wrong area)')
    # Headerless area: only accept it if the start really looks like a boot log,
    # otherwise a stale ext4 region would decode as pages of noise.
    head = data[512:512 + 8192]
    if not any(m in head for m in KERNEL_MARKS):
        sys.exit('no SANTONILOG1 header and the ring start is not a kernel log '
                 '(log never flushed, or wrong area)')
    salvaged = True
    hdr_size = 512
    size = len(data) - hdr_size
    blocks = 0
    while blocks * 512 + 512 <= size:
        blk = data[hdr_size + blocks * 512:hdr_size + (blocks + 1) * 512]
        if not any(0x20 <= b < 0x7f or b in (0x09, 0x0a, 0x0d) for b in blk):
            break
        blocks += 1
    total = blocks * 512
    print(f'# no SANTONILOG1 header (sector 0 is blank): salvaged {total} bytes '
          f'from the {blocks}-block ring prefix', file=sys.stderr)

ring = data[hdr_size:hdr_size + size]
if total <= size:
    text = ring[:total]
else:
    start = total % size
    text = ring[start:] + ring[:start]

text = text.rstrip(b'\0')
out = text.decode('utf-8', 'replace').replace('\r', '')
if not salvaged:
    magic_str = magic.decode('latin1', 'replace').strip('\x00')
    print(f'# {magic_str}: version {version}, ring {size}, total {total} bytes, flush #{seq}'
          f'{" (wrapped: oldest bytes lost)" if total > size else ""}', file=sys.stderr)

if len(args) > 1:
    with open(args[1], 'w', encoding='utf-8') as f:
        f.write(out)
else:
    sys.stdout.write(out)
