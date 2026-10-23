#!/usr/bin/env python3
"""Read an F12 recording (record-NNNNN.rgb, recorder.c / PsyZ Psyz_VideoRecordStart).

Each frame: 16-byte header (uint32 present number, uint32 game frame,
float ms since the start, uint32 0), then 480x270 RGB.

Usage: record_view.py record-NNNNN.rgb OUT_DIR [--sheet] [--from N --to M]
writes OUT_DIR/present-PPPPP-frame-FFFFF.png per present (--sheet: one
contact sheet instead, 6 per row, half size, labelled)."""
import argparse, os, struct, sys
from PIL import Image, ImageDraw

W, H = 480, 270

def frames(path):
    size = 16 + W * H * 3
    with open(path, 'rb') as f:
        while True:
            hdr = f.read(16)
            if len(hdr) < 16:
                return
            present, frame, t, _ = struct.unpack('<IIfI', hdr)
            px = f.read(W * H * 3)
            if len(px) < W * H * 3:
                return
            yield present, frame, t, Image.frombytes('RGB', (W, H), px)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rgb')
    ap.add_argument('out')
    ap.add_argument('--sheet', action='store_true')
    ap.add_argument('--from', dest='lo', type=int, default=0)
    ap.add_argument('--to', dest='hi', type=int, default=1 << 30)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    sel = [x for x in frames(a.rgb) if a.lo <= x[0] <= a.hi]
    if not a.sheet:
        for p, fr, t, im in sel:
            im.save(os.path.join(a.out, 'present-%05d-frame-%05d.png' % (p, fr)))
    else:
        cols, tw, th = 6, W // 2, H // 2 + 14
        rows = (len(sel) + cols - 1) // cols
        sheet = Image.new('RGB', (cols * tw, max(1, rows) * th))
        d = ImageDraw.Draw(sheet)
        for i, (p, fr, t, im) in enumerate(sel):
            x, y = (i % cols) * tw, (i // cols) * th
            sheet.paste(im.resize((tw, H // 2)), (x, y + 14))
            d.text((x + 2, y + 1), 'p%d f%d %.0fms' % (p, fr, t), fill=(255, 255, 0))
        sheet.save(os.path.join(a.out, 'sheet.png'))
    print('%d frames' % len(sel))

if __name__ == '__main__':
    main()
