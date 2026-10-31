# a machine profile (tisple machine --profile F.txt[@C]) added up by the program's functions
# and source lines, from its listing (tisple build --asm).
#
# run: python3 scripts/profile.py prog.asm profile.txt [--lines N] [--fns N]
#   functions: as compiled (an inlined one counts in its caller) and as written (the source
#   function each line is in, inlined or not); lines: the hottest source lines. Code run from
#   aux memory where the listing has main memory's code is that code (with_aux code that may
#   run with RAMRD on is in both memories). Code the listing does not have (lib/dhgr's runs at
#   $1E00, lib/unpack, the rom) shows by where and address range.

import os
import re
import sys
from collections import defaultdict

HEADER = re.compile(r"^(\S.*?)  \$([0-9A-F]{4}), (\d+) bytes( in the language card's bank 1)?$")
CODE = re.compile(r"^  ([0-9A-F]{4})  ")
SOURCE = re.compile(r"^    ; (.+?):(\d+): (.*?)  \[\d+ bytes\]$")
FN = re.compile(r"^\(fn (\S+)")

# code outside the listing, by where and address
KNOWN = [
    ("main aux", 0x1C00, 0x1D8A, "lib/unpack"),
    ("main", 0x1D8A, 0x1DDD, "lib/auxcard"),
    ("main aux", 0x1E00, 0x1F11, "lib/dhgr runs"),
]


def read_listing(path):
    # address -> (compiled function, source file, source line) for main and card1/card2
    places = {}
    fn, bank1, src = None, False, (None, 0)
    for line in open(path):
        line = line.rstrip("\n")
        m = HEADER.match(line)
        if m:
            fn, bank1, src = m.group(1), m.group(4) is not None, (None, 0)
            continue
        m = SOURCE.match(line)
        if m:
            src = (m.group(1), int(m.group(2)))
            continue
        m = CODE.match(line)
        if m and fn:
            address = int(m.group(1), 16)
            where = "card1" if bank1 else ("card2" if address >= 0xD000 else "main")
            places[(where, address)] = (fn, src[0], src[1])
    return places


def source_fns(path, root):
    # line number -> the (fn ...) it is in, for one source file
    full = path if os.path.isabs(path) else os.path.join(root, path)
    names, current = {}, None
    try:
        lines = open(full).read().split("\n")
    except OSError:
        return names
    for n, text in enumerate(lines, 1):
        m = FN.match(text)
        if m:
            current = m.group(1)
        names[n] = current
    return names


def short(path):
    return path.split("/lib/")[-1] if "/lib/" in path else path


def main():
    args = sys.argv[1:]
    lines_shown, fns_shown = 25, 25
    if "--lines" in args:
        i = args.index("--lines")
        lines_shown = int(args[i + 1])
        del args[i:i + 2]
    if "--fns" in args:
        i = args.index("--fns")
        fns_shown = int(args[i + 1])
        del args[i:i + 2]
    listing, profile = args
    root = os.getcwd()  # the listing's source paths are from where it was built
    places = read_listing(listing)
    files = {}
    compiled, written, by_line = defaultdict(int), defaultdict(int), defaultdict(int)
    total = 0
    for text in open(profile):
        where, address, cycles = text.split()
        address, cycles = int(address, 16), int(cycles)
        total += cycles
        place = places.get((where, address))
        if place is None and where == "aux" and address < 0xC000:
            place = places.get(("main", address))  # mirrored with_aux code
        if place is None:
            name = "%s $%04X" % (where, address & 0xFF00)
            for wheres, lo, hi, known in KNOWN:
                if where in wheres.split() and lo <= address < hi:
                    name = known + " (" + where + ")"
            compiled[name] += cycles
            written[name] += cycles
            continue
        fn, src, line = place
        compiled[fn] += cycles
        if src is None:
            written[fn] += cycles
            continue
        if src not in files:
            files[src] = source_fns(src, root)
        in_fn = files[src].get(line)
        written["%s.%s" % (short(src).replace(".tsp", ""), in_fn)] += cycles
        by_line["%s:%d" % (short(src), line)] += cycles
    frames = total / 17030
    print("%d cycles (%.1f frames)" % (total, frames))
    for title, table, count in (("compiled functions", compiled, fns_shown),
                                ("source functions", written, fns_shown),
                                ("source lines", by_line, lines_shown)):
        print("\n" + title)
        for name, cycles in sorted(table.items(), key=lambda kv: -kv[1])[:count]:
            print("  %5.1f%%  %9d  %s" % (100.0 * cycles / total, cycles, name))


main()
