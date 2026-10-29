"""Building one random-race program (t.tsp and t.c from `tisple race-gen`) with one compiler.

Each compiler gets the setting that won the kernel race: cc65 -Oirs -Cl, LLVM-MOS -O2,
Oscar64 -O2. A build gives the bytes to load, where to start, main's entry, t.init's entry
(the clock starts when it returns), t.checksum's entry (where it stops), the code objects
for counting bytes and the listing.
"""

import os
import subprocess
import sys

import toolchains
from toolchains import BuildError

SETTINGS = {
    "cc65": ["-Oirs", "-Cl"],
    "llvm-mos": ["-O2"],
    "oscar64": ["-O2"],
    "tisple": [],
}
# Tisple's code may not overlap the hi-res page at $2000-$3FFF; tisple race-gen lays the
# program out at the same origin for the interpreter (src/race/race_gen_command.cpp)
TISPLE_ORIGIN = 0x4000
CC65_ORIGIN = 0x0800  # toolchains/cc65-random.cfg
CHECKSUM_ADDRESS = 0xBF00  # chk: the u16 t.checksum leaves (tests/fuzz_gen.hpp)
OPAQUE_ADDRESS = 0xBC00  # opaque: a u16 the race writes as 0, which t.init adds to everything
# a build takes a second or two; Oscar64 now and then never finishes (a bug, counted as one)
BUILD_TIMEOUT = 60


class Build:
    def __init__(self):
        self.loads = []  # (address, bytes)
        self.start = 0
        self.entry = 0
        self.after = 0
        self.stop = 0
        self.objects = []  # (name, first, last)
        self.listing = None


def run(cmd, cwd, what, timeout=BUILD_TIMEOUT):
    try:
        proc = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        raise BuildError("%s timed out after %d s" % (what, timeout))
    if proc.returncode != 0:
        raise BuildError("%s failed:\n%s\n%s" % (what, " ".join(cmd), proc.stdout[-3000:]))
    return proc.stdout


def build_cc65(src, outdir):
    cmd = [os.path.join(toolchains.CC65, "bin", "cl65"), "-t", "sim6502",
           "-C", os.path.join(toolchains.HERE, "toolchains", "cc65-random.cfg")]
    cmd += SETTINGS["cc65"] + ["-I", toolchains.INCLUDE, "-DRACE_CC65"]
    cmd += ["-m", "k.map", "-Ln", "k.lbl", "-l", "k.lst", "-o", "k.bin", src]
    run(cmd, outdir, "cl65")
    b = Build()
    with open(os.path.join(outdir, "k.bin"), "rb") as f:
        b.loads.append((CC65_ORIGIN, f.read()))
    labels = toolchains._cc65_labels(os.path.join(outdir, "k.lbl"))
    b.start = labels["startup"]
    b.entry = labels["_main"]
    b.after = labels["_t_init"]
    b.stop = labels["_t_checksum"]
    with open(os.path.join(outdir, "k.map")) as f:
        b.objects = split_by_labels(toolchains._cc65_objects(f.read()), labels)
    b.listing = os.path.join(outdir, "k.lst")
    return b


def split_by_labels(objects, labels):
    """cc65's map has one code object per module: the program's module is cut at its
    functions' labels (_main, _t_...), so bytes count functions as for the others"""
    starts = sorted(a for name, a in labels.items() if name == "_main" or name.startswith("_t_"))
    out = []
    for name, first, last in objects:
        cuts = [a for a in starts if first <= a <= last]
        if not cuts:
            out.append((name, first, last))
            continue
        if cuts[0] > first:
            out.append((name, first, cuts[0] - 1))
        for i, a in enumerate(cuts):
            end = cuts[i + 1] - 1 if i + 1 < len(cuts) else last
            out.append(("%s %04X" % (name, a), a, end))
    return out


def build_llvm_mos(src, outdir):
    cmd = [os.path.join(toolchains.LLVM_MOS, "bin", "mos-sim-clang"), "-mcpu=mos6502"]
    cmd += SETTINGS["llvm-mos"] + ["-I", toolchains.INCLUDE, "-DRACE_LLVM_MOS", "-w"]
    cmd += ["-o", "k.bin", src]
    run(cmd, outdir, "mos-sim-clang")
    b = Build()
    with open(os.path.join(outdir, "k.bin"), "rb") as f:
        blocks = toolchains._sim_blocks(f.read())
    b.loads = [x for x in blocks if x[0] < 0xFFFA]
    nm = run([os.path.join(toolchains.LLVM_MOS, "bin", "llvm-nm"), "-n", "-S", "--defined-only",
              "k.bin.elf"], outdir, "llvm-nm")
    b.objects, symbols = toolchains._nm_objects(nm)
    b.start = symbols["_start"]
    b.entry = symbols["main"]
    b.after = symbols["t_init"]
    b.stop = symbols["t_checksum"]
    if symbols.get("__heap_start", 0) > CHECKSUM_ADDRESS:
        raise BuildError("llvm-mos program data runs past $%04X" % CHECKSUM_ADDRESS)
    listing = run([os.path.join(toolchains.LLVM_MOS, "bin", "llvm-objdump"), "-d",
                   "--print-imm-hex", "k.bin.elf"], outdir, "llvm-objdump")
    b.listing = os.path.join(outdir, "k.lst")
    with open(b.listing, "w") as f:
        f.write(listing)
    return b


def build_oscar64(src, outdir):
    cmd = [os.path.join(toolchains.OSCAR64, "bin", "oscar64"), "-n", "-tm=c64", "-tf=prg"]
    cmd += SETTINGS["oscar64"] + ["-i=" + toolchains.INCLUDE, "-dRACE_OSCAR64", "-o=k.prg", src]
    out = run(cmd, outdir, "oscar64")
    if "error" in out.lower() and not os.path.exists(os.path.join(outdir, "k.prg")):
        raise BuildError("oscar64: " + out[-2000:])
    b = Build()
    with open(os.path.join(outdir, "k.prg"), "rb") as f:
        data = f.read()
    b.loads.append((data[0] | (data[1] << 8), data[2:]))
    with open(os.path.join(outdir, "k.map")) as f:
        b.objects, labels = toolchains._oscar_objects(f.read())
    if labels.get("startup") != 0x0801:
        raise BuildError("oscar64 startup is not at $0801")
    b.start = toolchains.OSCAR64_START
    b.entry = labels["main"]
    b.after = labels["t_init"]
    b.stop = labels["t_checksum"]
    b.listing = os.path.join(outdir, "k.asm")
    return b


def build_tisple(src, outdir):
    # the clock's ends are t.init's return and t.checksum's entry: neither may be inlined, as
    # the C's RACE_NOINLINE
    listing = run([toolchains.tisple_binary(), "build", src, "--origin", "0x%04X" % TISPLE_ORIGIN,
                   "--no-inline", "t.init", "--no-inline", "t.checksum",
                   "--asm", "-o", os.path.join(outdir, "k.bin")], outdir, "tisple build")
    b = Build()
    with open(os.path.join(outdir, "k.bin"), "rb") as f:
        b.loads.append((TISPLE_ORIGIN, f.read()))
    b.listing = os.path.join(outdir, "k.lst")
    with open(b.listing, "w") as f:
        f.write(listing)
    b.objects = toolchains._tisple_objects(listing)
    spans = {name: first for name, first, _last in b.objects}
    for name in ("t.main", "t.init", "t.checksum"):
        if name not in spans:
            raise BuildError("no %s in the tisple listing" % name)
    b.start = TISPLE_ORIGIN
    b.entry = spans["t.main"]
    b.after = spans["t.init"]
    b.stop = spans["t.checksum"]
    return b


BUILDERS = {
    "cc65": (build_cc65, "t.c"),
    "llvm-mos": (build_llvm_mos, "t.c"),
    "oscar64": (build_oscar64, "t.c"),
    "tisple": (build_tisple, "t.tsp"),
}


def build(compiler, program_dir):
    """builds program_dir/t.c or t.tsp into program_dir/<compiler>/"""
    program_dir = os.path.abspath(program_dir)
    builder, source = BUILDERS[compiler]
    outdir = os.path.join(program_dir, compiler)
    os.makedirs(outdir, exist_ok=True)
    result = builder(os.path.join(program_dir, source), outdir)
    for address, data in result.loads:
        if address <= CHECKSUM_ADDRESS + 1 and CHECKSUM_ADDRESS < address + len(data):
            raise BuildError("the binary's bytes at $%04X-$%04X cover the checksum at $%04X" %
                             (address, address + len(data) - 1, CHECKSUM_ADDRESS))
    return result


def host_check(program_dir, cc):
    """t.c built for this machine with sanitizers and run: its checksum, or an error text.
    The C writer's own check: a C compiler that disagrees with Tisple is only wrong when the
    host build agrees with the interpreter."""
    outdir = os.path.join(program_dir, "host")
    os.makedirs(outdir, exist_ok=True)
    exe = os.path.join(outdir, "t")
    # macOS's AddressSanitizer runtime takes minutes to start (its shadow setup walks the
    # dyld shared cache), so there the check uses UBSan alone
    sanitizers = "undefined" if sys.platform == "darwin" else "address,undefined"
    cmd = [cc, "-std=c99", "-O1", "-g", "-fsanitize=" + sanitizers,
           "-fno-sanitize-recover=all", "-DRACE_HOST", "-I", toolchains.INCLUDE, "-w",
           "-o", exe, os.path.join(program_dir, "t.c")]
    try:
        run(cmd, outdir, "host cc")
        out = run([exe], outdir, "host run", timeout=120)
    except BuildError as e:
        return None, str(e)[-1500:]
    for line in out.splitlines():
        if line.startswith("checksum "):
            return int(line.split()[1]), ""
    return None, "no checksum printed:\n" + out[-1500:]
