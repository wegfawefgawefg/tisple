#!/usr/bin/env python3
"""Looks for Apple's ROMs and DOS in files that are about to go public.

    web/rom_check.py PATH...        files or folders; exit 1 when something is found

Tisple runs on its own boot rom, mouse firmware and font, so nothing it ships needs a byte
of Apple's. This never reads Apple's rom files to compare against (they stay on the desktop,
~/.local/share/tisple/roms); it looks for byte runs only Apple's code has, and for files
named or sized like a rom dump. A hit is something to look at, not proof.
"""

import os
import sys

# runs of bytes from Apple's code and text: high-bit ASCII is how the II stores screen text
SIGNATURES = [
    ("Applesoft token table (END FOR NEXT)", bytes.fromhex("454ec4464fd24e4558d4")),
    ("Apple ][ banner, high-bit", bytes.fromhex("c1d0d0ccc5a0dddb")),
    ("Apple //e banner, high-bit", bytes(c | 0x80 for c in b"Apple //e")),
    ("APPLE, high-bit", bytes.fromhex("c1d0d0ccc5")),
    ("Disk II boot rom nibble table loop", bytes.fromhex("8a0a243cf010053c49ff297eb0084ad0fb")),
    ("DOS error text, high-bit (LANGUAGE NOT AVAILABLE)",
     bytes(c | 0x80 for c in b"LANGUAGE NOT AVAILABLE")),
    ("DOS error text, high-bit (FILE LOCKED)", bytes(c | 0x80 for c in b"FILE LOCKED")),
]

ROM_NAMES = ("apple2", "apple2e", "apple2plus", "disk2", "mouseinterface", "341-")
ROM_EXTENSIONS = (".rom", ".bin", ".a2r")
ROM_SIZES = (256, 2048, 4096, 8192, 12288, 16384, 20480, 32768)


def files_under(path):
    if os.path.isfile(path):
        yield path
        return
    for root, dirs, names in os.walk(path):
        dirs[:] = [d for d in dirs if d != ".git"]
        for name in names:
            yield os.path.join(root, name)


def check_file(path):
    found = []
    lower = os.path.basename(path).lower()
    size = os.path.getsize(path)
    if lower.endswith(".rom"):
        found.append("named like a rom")
    if any(lower.startswith(n) for n in ROM_NAMES) and lower.endswith(ROM_EXTENSIONS):
        found.append("named like an Apple rom dump")
    if lower.endswith(ROM_EXTENSIONS) and size in ROM_SIZES:
        found.append(f"{size} bytes, a rom's size")
    with open(path, "rb") as f:
        data = f.read()
    for label, run in SIGNATURES:
        at = data.find(run)
        if at >= 0:
            found.append(f"{label} at offset {at}")
    return found


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip())
        return 2
    hits = 0
    count = 0
    for path in sys.argv[1:]:
        for name in files_under(path):
            count += 1
            for what in check_file(name):
                print(f"{name}: {what}")
                hits += 1
    print(f"rom check: {count} files, {hits} hits")
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
