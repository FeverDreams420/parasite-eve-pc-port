#!/usr/bin/env python3
"""Copy the two files the build needs out of a Disc 1 image.

Usage: extract_disc.py <Disc 1 .cue or .bin> <tree>

Writes <tree>/assets/USA/main.exe (SLUS_006.62) and
<tree>/disc/extracted/PE.IMG, and checks the executable's SHA-1.

The .bin is a raw image with 2352-byte sectors (Mode 2 Form 1). Each sector's
2048 bytes of user data start at byte 24, so the ISO 9660 file system is read
through a view that skips the sector headers. Needs pycdlib.
"""
import hashlib
import io
import os
import pathlib
import re
import sys

import pycdlib

RAW = 2352
USER_OFFSET = 24
USER = 2048
MAIN_EXE_SHA1 = '452fb033f2eaa4b18aa20a5bca60b8125af3a37b'


class UserDataView(io.RawIOBase):
    """Read-only file object over the 2048-byte user data of a raw .bin."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        self.size = os.path.getsize(path) // RAW * USER
        self.pos = 0

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, offset, whence=io.SEEK_SET):
        if whence == io.SEEK_SET:
            self.pos = offset
        elif whence == io.SEEK_CUR:
            self.pos += offset
        else:
            self.pos = self.size + offset
        return self.pos

    def readinto(self, b):
        want = min(len(b), max(0, self.size - self.pos))
        done = 0
        while done < want:
            sector, off = divmod(self.pos, USER)
            n = min(USER - off, want - done)
            self.f.seek(sector * RAW + USER_OFFSET + off)
            chunk = self.f.read(n)
            if len(chunk) != n:
                raise IOError('short read from the .bin')
            b[done:done + n] = chunk
            done += n
            self.pos += n
        return done

    def close(self):
        self.f.close()
        super().close()


def bin_from_cue(cue):
    """The first FILE of the .cue (the data track)."""
    text = cue.read_text(errors='replace')
    m = re.search(r'^\s*FILE\s+"([^"]+)"', text, re.M | re.I)
    if not m:
        m = re.search(r'^\s*FILE\s+(\S+)', text, re.M | re.I)
    if not m:
        sys.exit('error: no FILE line in %s' % cue)
    return cue.parent / m.group(1)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[2])
    image = pathlib.Path(sys.argv[1])
    tree = pathlib.Path(sys.argv[2])
    if image.suffix.lower() == '.cue':
        image = bin_from_cue(image)
    if not image.is_file():
        sys.exit('error: %s not found' % image)
    if os.path.getsize(image) % RAW:
        sys.exit('error: %s is not a raw 2352-byte-sector image' % image)

    view = UserDataView(image)
    iso = pycdlib.PyCdlib()
    iso.open_fp(view)
    outputs = [
        ('/SLUS_006.62;1', tree / 'assets/USA/main.exe'),
        ('/PE.IMG;1', tree / 'disc/extracted/PE.IMG'),
    ]
    for src, dst in outputs:
        dst.parent.mkdir(parents=True, exist_ok=True)
        tmp = dst.with_name(dst.name + '.part')
        with open(tmp, 'wb') as out:
            iso.get_file_from_iso_fp(out, iso_path=src)
        tmp.replace(dst)
        print('%s -> %s' % (src, dst))
    iso.close()
    view.close()

    sha1 = hashlib.sha1((tree / 'assets/USA/main.exe').read_bytes()).hexdigest()
    if sha1 != MAIN_EXE_SHA1:
        sys.exit('error: main.exe SHA-1 is %s, expected %s (not the USA Disc 1?)'
                 % (sha1, MAIN_EXE_SHA1))
    print('main.exe SHA-1 OK')


if __name__ == '__main__':
    main()
