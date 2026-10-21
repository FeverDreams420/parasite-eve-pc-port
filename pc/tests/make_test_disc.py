#!/usr/bin/env python3
"""Build a tiny MODE2/2352 test disc (ISO 9660) for the host's CD code.

It holds the given files at the given paths; nothing else (no game data is
generated - pass your own SLUS_006.62 if you want the boot to start).
Usage: make_test_disc.py OUT.bin  ISO_PATH=LOCAL_FILE_OR_SIZE ...
  e.g. SLUS_006.62=assets/USA/main.exe PE.IMG=@65536 FMV1/PEDISC01.IDF=@2048
(@N makes a zero-filled file of N bytes)
"""
import struct, sys

def both16(v): return struct.pack('<H', v) + struct.pack('>H', v)
def both32(v): return struct.pack('<I', v) + struct.pack('>I', v)

def dirrec(lba, size, name, isdir):
    n = name.encode()
    rec = struct.pack('<BB', 0, 0) + both32(lba) + both32(size) + bytes(7) + bytes([2 if isdir else 0, 0, 0]) + both16(1) + bytes([len(n)]) + n
    if len(rec) % 2:
        rec += b'\0'
    return bytes([len(rec)]) + rec[1:]

def main():
    out = sys.argv[1]
    files = {}
    for a in sys.argv[2:]:
        path, src = a.split('=', 1)
        data = bytes(int(src[1:])) if src.startswith('@') else open(src, 'rb').read()
        files[path.upper()] = data
    dirs = {''}
    for p in files:
        parts = p.split('/')
        for i in range(1, len(parts)):
            dirs.add('/'.join(parts[:i]))
    sectors = {}
    nxt = 22
    dir_lba = {}
    for d in sorted(dirs):
        dir_lba[d] = nxt; nxt += 1
    file_lba = {}
    for p, data in sorted(files.items()):
        file_lba[p] = nxt; nxt += max(1, (len(data) + 2047) // 2048)
    total = nxt
    for d in sorted(dirs):
        parent = d.rsplit('/', 1)[0] if '/' in d else ''
        body = dirrec(dir_lba[d], 2048, '\0', True) + dirrec(dir_lba[parent], 2048, '\1', True)
        for sd in sorted(x for x in dirs if x and (x.rsplit('/', 1)[0] if '/' in x else '') == d and x != d):
            body += dirrec(dir_lba[sd], 2048, sd.split('/')[-1], True)
        for p in sorted(files):
            pd = p.rsplit('/', 1)[0] if '/' in p else ''
            if pd == d:
                body += dirrec(file_lba[p], len(files[p]), p.split('/')[-1] + ';1', False)
        sectors[dir_lba[d]] = body.ljust(2048, b'\0')
    for p, data in files.items():
        for i in range(max(1, (len(data) + 2047) // 2048)):
            sectors[file_lba[p] + i] = data[i * 2048:(i + 1) * 2048].ljust(2048, b'\0')
    pvd = bytearray(2048)
    pvd[0] = 1; pvd[1:6] = b'CD001'; pvd[6] = 1
    pvd[80:88] = both32(total)
    pvd[156:156 + 34] = dirrec(dir_lba[''], 2048, '\0', True)
    sectors[16] = bytes(pvd)
    term = bytearray(2048); term[0] = 255; term[1:6] = b'CD001'; term[6] = 1
    sectors[17] = bytes(term)
    with open(out, 'wb') as f:
        for lba in range(total):
            raw = bytearray(2352)
            raw[0] = 0; raw[1:11] = b'\xff' * 10; raw[11] = 0
            m = lba + 150
            bcd = lambda v: (v // 10) << 4 | v % 10
            raw[12:16] = bytes([bcd(m // 4500), bcd(m // 75 % 60), bcd(m % 75), 2])
            raw[16:24] = bytes([0, 0, 8, 0] * 2)
            raw[24:24 + 2048] = sectors.get(lba, bytes(2048))
            f.write(raw)
    print('wrote', out, total, 'sectors')

main()
