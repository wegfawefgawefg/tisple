"""Building one kernel with one compiler and setting, and reading back where everything is.

A build gives the bytes to load, where to start, where the kernel (main) begins, the code
objects (function or library module, first and last address) for counting bytes, and
each buffer's address and layout.
"""

import os
import re
import shutil
import subprocess

from layout import BUFFER_HIGH, BUFFER_LOW, C_TYPES

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
COMPILERS = os.environ.get("TISPLE_COMPILERS",
                           os.path.expanduser("~/.local/share/tisple/compilers"))
CC65 = os.path.join(COMPILERS, "cc65")
LLVM_MOS = os.path.join(COMPILERS, "llvm-mos")
OSCAR64 = os.path.join(COMPILERS, "oscar64")
INCLUDE = os.path.join(HERE, "include")
TISPLE_ORIGIN = 0x6000


def tisple_binary():
    path = os.environ.get("TISPLE")
    if path:
        return path
    for d in ("build", "build-debug"):
        candidate = os.path.join(REPO, d, "tisple")
        if os.path.exists(candidate):
            return candidate
    raise BuildError("no tisple binary; build it or set TISPLE")


class BuildError(Exception):
    pass


class Variant:
    """one way to build a kernel: compiler, flags, buffer mode and source file.
    mode: 'const' buffers are constant pointers (the compiler knows the address),
    'extern' they are extern arrays the linker places, 'striped' as const but arrays of
    multibyte elements are Oscar64 __striped globals."""

    def __init__(self, compiler, flags, mode, source):
        self.compiler = compiler
        self.flags = flags
        self.mode = mode
        self.source = source

    def label(self):
        parts = [" ".join(self.flags)] if self.flags else []
        if self.compiler != "tisple":
            parts.append(self.mode)
        if self.source not in ("kernel.c", "kernel.tsp"):
            parts.append(self.source)
        return " ".join(parts)

    def key(self):
        text = "-".join([self.compiler] + self.flags + [self.mode, self.source])
        return re.sub(r"[^A-Za-z0-9.]+", "_", text)


class Build:
    def __init__(self):
        self.loads = []  # (address, bytes)
        self.start = 0
        self.entry = 0
        self.objects = []  # (name, first, last)
        self.addresses = {}  # buffer name -> address
        self.layouts = {}  # buffer name -> 'packed' or 'planar'
        self.listing = None


# flags each compiler is tried with; the fastest correct build counts
CC65_FLAGS = [["-Oirs", "-Cl"], ["-Oirs"]]
LLVM_MOS_FLAGS = [["-O2"], ["-O3"]]
OSCAR64_FLAGS = [["-O2"], ["-O3"], ["-O3", "-Ox", "-OM"]]


def kernel_dir(kernel):
    return os.path.join(HERE, "kernels", kernel.name)


def variants_for(kernel, compilers):
    # kernel.c, then other ways to write it in C: lib.c through the C library, ...
    sources = sorted((s for s in os.listdir(kernel_dir(kernel)) if s.endswith(".c")),
                     key=lambda s: (s != "kernel.c", s))
    has_multibyte = any(b.is_multibyte() for b in kernel.buffers)
    out = []
    for compiler in compilers:
        if compiler == "tisple":
            out.append(Variant("tisple", [], "const", "kernel.tsp"))
            continue
        for source in sources:
            if compiler == "cc65":
                for flags in CC65_FLAGS:
                    for mode in ("const", "extern"):
                        out.append(Variant(compiler, flags, mode, source))
            elif compiler == "llvm-mos":
                for flags in LLVM_MOS_FLAGS:
                    for mode in ("const", "extern"):
                        out.append(Variant(compiler, flags, mode, source))
            elif compiler == "oscar64":
                modes = ["const", "striped"] if has_multibyte else ["const"]
                for flags in OSCAR64_FLAGS:
                    for mode in modes:
                        out.append(Variant(compiler, flags, mode, source))
    return out


def run(cmd, cwd, what):
    proc = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, timeout=300)
    if proc.returncode != 0:
        raise BuildError("%s failed:\n%s\n%s" % (what, " ".join(cmd), proc.stdout[-3000:]))
    return proc.stdout


# --- C sources --------------------------------------------------------------------------


def _struct_definitions(kernel):
    seen = set()
    text = ""
    for b in kernel.buffers:
        if b.struct is None or b.struct in seen:
            continue
        seen.add(b.struct)
        fields = "".join("    %s %s;\n" % (C_TYPES[t], f) for f, t in b.kind)
        text += "struct %s {\n%s};\n" % (b.struct, fields)
    return text


def _buffer_line(b, mode):
    if mode == "extern":
        if b.count is None:
            return "extern %s %s;\n" % (b.c_type(), b.name)
        return "extern %s %s[%d];\n" % (b.c_type(), b.name, b.count)
    if mode == "striped" and b.is_multibyte():
        return "__striped %s %s[%d];\n" % (b.c_type(), b.name, b.count)
    if b.count is None:
        return "#define %s (*(%s *)0x%04X)\n" % (b.name, b.c_type(), b.address)
    return "#define %s ((%s *)0x%04X)\n" % (b.name, b.c_type(), b.address)


def write_c_sources(kernel, variant, outdir):
    """the kernel source, buffers.h and any generated headers into outdir"""
    text = "/* written by races/race.py: %s buffers, %s mode */\n" % (kernel.name, variant.mode)
    text += _struct_definitions(kernel)
    for b in kernel.buffers:
        text += _buffer_line(b, variant.mode)
    with open(os.path.join(outdir, "buffers.h"), "w") as f:
        f.write(text)
    for name, make in kernel.c_headers.items():
        with open(os.path.join(outdir, name), "w") as f:
            f.write(make())
    shutil.copy(os.path.join(kernel_dir(kernel), variant.source), os.path.join(outdir, "k.c"))


def c_layouts(kernel, variant, build):
    for b in kernel.buffers:
        striped = variant.mode == "striped" and b.is_multibyte()
        build.layouts[b.name] = "planar" if striped else "packed"
        if b.name not in build.addresses:
            build.addresses[b.name] = b.address


# --- cc65 -------------------------------------------------------------------------------


def _cc65_objects(map_text):
    starts = {}
    for m in re.finditer(r"^(\w+)\s+([0-9A-F]{6})\s+([0-9A-F]{6})\s+([0-9A-F]{6})", map_text,
                         re.M):
        starts[m.group(1)] = int(m.group(2), 16)
    objects = []
    module = None
    modules = map_text.split("Segment list:")[0]
    for line in modules.splitlines():
        if line and not line.startswith(" ") and line.endswith(":"):
            module = line[:-1]
            module = re.sub(r"^.*/lib/", "", module)
            continue
        m = re.match(r"\s+(\w+)\s+Offs=([0-9A-F]+)\s+Size=([0-9A-F]+)", line)
        if not m or module is None:
            continue
        seg, offs, size = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
        if seg not in ("CODE", "LOWCODE", "STARTUP", "ONCE") or size == 0:
            continue
        first = starts[seg] + offs
        objects.append(("%s %s" % (module, seg), first, first + size - 1))
    return objects


def _cc65_labels(path):
    labels = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) == 3 and parts[0] == "al":
                labels[parts[2].lstrip(".")] = int(parts[1], 16)
    return labels


def build_cc65(kernel, variant, outdir):
    write_c_sources(kernel, variant, outdir)
    cmd = [os.path.join(CC65, "bin", "cl65"), "-t", "sim6502",
           "-C", os.path.join(HERE, "toolchains", "cc65-race.cfg")] + variant.flags
    cmd += ["-I", INCLUDE, "-I", ".", "-DRACE_CC65"]
    if variant.mode == "extern":
        defs = ",".join("-D,_%s=$%04X" % (b.name, b.address) for b in kernel.buffers)
        cmd += ["-Wl", defs]
    cmd += ["-m", "k.map", "-Ln", "k.lbl", "-l", "k.lst", "-o", "k.bin", "k.c"]
    run(cmd, outdir, "cl65")
    build = Build()
    with open(os.path.join(outdir, "k.bin"), "rb") as f:
        build.loads.append((0x6000, f.read()))
    labels = _cc65_labels(os.path.join(outdir, "k.lbl"))
    build.start = labels["startup"]
    build.entry = labels["_main"]
    with open(os.path.join(outdir, "k.map")) as f:
        build.objects = _cc65_objects(f.read())
    build.listing = os.path.join(outdir, "k.lst")
    c_layouts(kernel, variant, build)
    return build


# --- llvm-mos ---------------------------------------------------------------------------


def _sim_blocks(data):
    """llvm-mos sim images: (address, length, bytes) blocks; the last one is the vectors"""
    blocks = []
    at = 0
    while at + 4 <= len(data):
        address = data[at] | (data[at + 1] << 8)
        length = data[at + 2] | (data[at + 3] << 8)
        blocks.append((address, data[at + 4:at + 4 + length]))
        at += 4 + length
    return blocks


def _nm_objects(nm_text):
    """code objects from llvm-nm -n -S: sized symbols as they are, others up to the next"""
    symbols = []
    for line in nm_text.splitlines():
        parts = line.split()
        if len(parts) == 4:
            symbols.append((int(parts[0], 16), int(parts[1], 16), parts[2], parts[3]))
        elif len(parts) == 3:
            symbols.append((int(parts[0], 16), None, parts[1], parts[2]))
    text = [s for s in symbols if s[2] in "tT"]
    objects = []
    for i, (address, size, _kind, name) in enumerate(text):
        if size is None or size == 0:
            later = [s[0] for s in text[i + 1:] if s[0] > address]
            if not later:
                continue
            size = later[0] - address
        objects.append((name, address, address + size - 1))
    return objects, {s[3]: s[0] for s in symbols}


def build_llvm_mos(kernel, variant, outdir):
    write_c_sources(kernel, variant, outdir)
    cmd = [os.path.join(LLVM_MOS, "bin", "mos-sim-clang"), "-mcpu=mos6502"] + variant.flags
    cmd += ["-I", INCLUDE, "-I", ".", "-DRACE_LLVM_MOS"]
    if variant.mode == "extern":
        cmd += ["-Wl,--defsym=%s=0x%04X" % (b.name, b.address) for b in kernel.buffers]
    cmd += ["-o", "k.bin", "k.c"]
    run(cmd, outdir, "mos-sim-clang")
    build = Build()
    with open(os.path.join(outdir, "k.bin"), "rb") as f:
        blocks = _sim_blocks(f.read())
    build.loads = [b for b in blocks if b[0] < 0xFFFA]
    nm = run([os.path.join(LLVM_MOS, "bin", "llvm-nm"), "-n", "-S", "--defined-only",
              "k.bin.elf"], outdir, "llvm-nm")
    build.objects, symbols = _nm_objects(nm)
    build.start = symbols["_start"]
    build.entry = symbols["main"]
    end = symbols.get("__heap_start", 0)
    if end > BUFFER_LOW:
        raise BuildError("llvm-mos program data runs to $%04X, into the buffers" % end)
    listing = run([os.path.join(LLVM_MOS, "bin", "llvm-objdump"), "-d", "--print-imm-hex",
                   "k.bin.elf"], outdir, "llvm-objdump")
    build.listing = os.path.join(outdir, "k.lst")
    with open(build.listing, "w") as f:
        f.write(listing)
    c_layouts(kernel, variant, build)
    return build


# --- oscar64 ----------------------------------------------------------------------------

OSCAR64_START = 0x080D  # the BASIC stub's SYS 2061


def _oscar_objects(map_text):
    objects = []
    labels = {}
    section = None
    for line in map_text.splitlines():
        if line in ("sections", "regions", "objects", "objects by size"):
            section = line
            continue
        m = re.match(r"([0-9a-f]{4}) - ([0-9a-f]{4}) : ([^,]+), (\w+):", line)
        if section != "objects" or not m:
            continue
        first, end, name, kind = int(m.group(1), 16), int(m.group(2), 16), m.group(3), m.group(4)
        labels[name] = first
        if kind == "NATIVE_CODE" and end > first:
            objects.append((name, first, end - 1))
    return objects, labels


def build_oscar64(kernel, variant, outdir):
    write_c_sources(kernel, variant, outdir)
    cmd = [os.path.join(OSCAR64, "bin", "oscar64"), "-n", "-tm=c64", "-tf=prg"] + variant.flags
    cmd += ["-i=" + INCLUDE, "-i=" + outdir, "-dRACE_OSCAR64", "-o=k.prg", "k.c"]
    run(cmd, outdir, "oscar64")
    build = Build()
    with open(os.path.join(outdir, "k.prg"), "rb") as f:
        data = f.read()
    build.loads.append((data[0] | (data[1] << 8), data[2:]))
    with open(os.path.join(outdir, "k.map")) as f:
        build.objects, labels = _oscar_objects(f.read())
    if labels.get("startup") != 0x0801:
        raise BuildError("oscar64 startup is not at $0801")
    build.start = OSCAR64_START
    build.entry = labels["main"]
    for b in kernel.buffers:
        if variant.mode == "striped" and b.is_multibyte():
            build.addresses[b.name] = labels[b.name]
    build.listing = os.path.join(outdir, "k.asm")
    c_layouts(kernel, variant, build)
    return build


# --- tisple -----------------------------------------------------------------------------


def _tisple_objects(listing):
    """spans "name  $ADDR, N bytes"; the runtime span split at its rt. labels"""
    spans = []  # name, first, last, [(routine label, its first address)]
    pending = None
    for line in listing.splitlines():
        m = re.match(r"(\S+)  \$([0-9A-F]{4}), (\d+) bytes$", line)
        if m:
            first = int(m.group(2), 16)
            spans.append((m.group(1), first, first + int(m.group(3)) - 1, []))
            pending = None
            continue
        m = re.match(r"  (rt\.\w+):$", line)
        if m and spans and spans[-1][0] == "runtime":
            pending = m.group(1)
            continue
        m = re.match(r"  ([0-9A-F]{4})  ", line)
        if m and pending is not None:
            spans[-1][3].append((pending, int(m.group(1), 16)))
            pending = None
    objects = []
    for name, first, last, routines in spans:
        if not routines:
            objects.append((name, first, last))
            continue
        if routines[0][1] > first:
            objects.append((name, first, routines[0][1] - 1))
        for i, (label, address) in enumerate(routines):
            end = routines[i + 1][1] - 1 if i + 1 < len(routines) else last
            objects.append((label, address, end))
    return objects


def build_tisple(kernel, variant, outdir):
    src = os.path.join(outdir, "kernel.tsp")
    shutil.copy(os.path.join(kernel_dir(kernel), variant.source), src)
    listing = run([tisple_binary(), "build", src, "--origin", "0x%04X" % TISPLE_ORIGIN,
                   "--asm", "-o", os.path.join(outdir, "k.bin")], outdir, "tisple build")
    build = Build()
    with open(os.path.join(outdir, "k.bin"), "rb") as f:
        build.loads.append((TISPLE_ORIGIN, f.read()))
    build.listing = os.path.join(outdir, "k.lst")
    with open(build.listing, "w") as f:
        f.write(listing)
    build.objects = _tisple_objects(listing)
    entry = [o for o in build.objects if o[0] == "kernel.main"]
    if not entry:
        raise BuildError("no kernel.main in the tisple listing")
    build.start = TISPLE_ORIGIN
    build.entry = entry[0][1]
    for b in kernel.buffers:
        build.addresses[b.name] = b.address
        build.layouts[b.name] = "planar"
    return build


BUILDERS = {
    "cc65": build_cc65,
    "llvm-mos": build_llvm_mos,
    "oscar64": build_oscar64,
    "tisple": build_tisple,
}


def build(kernel, variant, outdir):
    os.makedirs(outdir, exist_ok=True)
    result = BUILDERS[variant.compiler](kernel, variant, outdir)
    for address, data in result.loads:
        for b in kernel.buffers:
            first = result.addresses[b.name]
            if first < address + len(data) and address < first + b.size() and \
                    BUFFER_LOW <= first < BUFFER_HIGH:
                raise BuildError("the binary's bytes at $%04X-$%04X cover buffer %s" %
                                 (address, address + len(data) - 1, b.name))
    return result
