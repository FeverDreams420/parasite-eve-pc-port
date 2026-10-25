#!/usr/bin/env python3
"""Put the disc paths into settings.ini next to the built exe.

    write-settings.py <settings.ini> <disc 1 .cue> [<disc 2 .cue>]

Sets disc= (and disc2= when Disc 2 is given); every other line of an
existing settings.ini is kept. An empty Disc 2 leaves disc2= as it was.
"""
import sys


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__.strip(), file=sys.stderr)
        return 1
    ini, disc1 = sys.argv[1], sys.argv[2]
    disc2 = sys.argv[3] if len(sys.argv) == 4 else ""
    try:
        with open(ini) as f:
            lines = f.read().splitlines()
    except FileNotFoundError:
        lines = ["# Parasite Eve settings"]
    keys = ["disc"] + (["disc2"] if disc2 else [])
    kept = [l for l in lines
            if not any(l.startswith(k + "=") or l.startswith("# " + k + ":") for k in keys)]
    kept += ["# disc: your Disc 1 image (Disc 2 is found next to it)", "disc=" + disc1]
    if disc2:
        kept += ["# disc2: your Disc 2 image when it is not next to Disc 1 (none = don't ask)",
                 "disc2=" + disc2]
    with open(ini, "w") as f:
        f.write("\n".join(kept) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
