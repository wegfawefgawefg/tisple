#!/usr/bin/env python3
"""The compiler race: every kernel with every compiler, timed in our simulator.

    races/run.sh [--kernels a,b] [--compilers cc65,tisple] [--jobs N] [--variants]
                 [--update-doc]
    races/run.sh --gaps [--kernels a,b]
    races/run.sh --programs [--kernels a,b] [--update-doc]   the program race

--programs races the programs in races/programs/ (program_specs.py) instead of the
kernels: the same builds, runs and checks, into races/out/programs/ and the program
tables in docs/races.md.

Each build is run by `tisple race`: the binary's own startup runs, the inputs are written
when main is entered, and the cycles from main's first instruction to its RTS are the
score. A result that differs from the Python reference does not count. Bytes are the code
the kernel ran: every function or library module it executed, whole.
"""

import argparse
import concurrent.futures
import json
import math
import os
import sys

import kernels as kernel_specs
import program_specs
import toolchains
from layout import decode, encode

OUT = os.path.join(toolchains.HERE, "out")
DOC = os.path.join(toolchains.REPO, "docs", "races.md")
ALL_COMPILERS = ["cc65", "llvm-mos", "oscar64", "tisple"]
MAX_CYCLES = 400000000
DOC_BEGIN = "<!-- race results: begin (races/run.sh --update-doc writes this part) -->"
DOC_END = "<!-- race results: end -->"
PROGRAM_DOC_BEGIN = ("<!-- program race results: begin (races/run.sh --programs --update-doc "
                     "writes this part) -->")
PROGRAM_DOC_END = "<!-- program race results: end -->"


def out_dir(kernel):
    """races/out/<kernel>, races/out/programs/<program>"""
    if kernel.folder == "kernels":
        return os.path.join(OUT, kernel.name)
    return os.path.join(OUT, kernel.folder, kernel.name)


class Result:
    def __init__(self, kernel, variant):
        self.kernel = kernel.name
        self.compiler = variant.compiler
        self.variant = variant.label()
        self.key = variant.key()
        self.ok = False
        self.error = ""
        self.cycles = 0
        self.bytes = 0
        self.objects = []
        self.listing = None

    def as_json(self):
        return dict(self.__dict__)


def write_file(path, data):
    with open(path, "wb") as f:
        f.write(data)


def parse_ranges(path):
    executed = set()
    with open(path) as f:
        for line in f:
            first, last = (int(x, 16) for x in line.split())
            executed.update(range(first, last + 1))
    return executed


def code_bytes(build, executed):
    """bytes of every code object the kernel ran, plus any ran code outside one"""
    total = 0
    names = []
    covered = set()
    for name, first, last in build.objects:
        span = range(first, last + 1)
        if any(a in executed for a in span):
            total += len(span)
            names.append(name)
        covered.update(span)
    loose = len(executed - covered)
    if loose:
        names.append("(%d bytes outside any object)" % loose)
    return total + loose, names


def check_outputs(kernel, build, inputs, memory):
    expected = kernel.reference(inputs)
    for name, want in expected.items():
        buf = kernel.buffer(name)
        address = build.addresses[name]
        got = decode(buf, memory[address:address + buf.size()], build.layouts[name])
        if got != want:
            if isinstance(want, list):
                bad = [i for i in range(len(want)) if got[i] != want[i]]
                return "%s wrong at %d of %d elements, first [%d] got %r want %r" % (
                    name, len(bad), len(want), bad[0], got[bad[0]], want[bad[0]])
            return "%s is %r, want %r" % (name, got, want)
    return ""


def measure(kernel, variant):
    result = Result(kernel, variant)
    outdir = os.path.join(out_dir(kernel), variant.key())
    try:
        build = toolchains.build(kernel, variant, outdir)
    except (toolchains.BuildError, KeyError, OSError) as e:
        result.error = "build: %s" % e
        return result
    result.listing = os.path.relpath(build.listing, toolchains.REPO)
    inputs = kernel_specs.make_inputs(kernel)
    cmd = [toolchains.tisple_binary(), "race"]
    for i, (address, data) in enumerate(build.loads):
        path = os.path.join(outdir, "load%d.bin" % i)
        write_file(path, data)
        cmd += ["--load", "%s@0x%04X" % (path, address)]
    for buf in kernel.buffers:
        path = os.path.join(outdir, "in_%s.bin" % buf.name)
        write_file(path, encode(buf, inputs[buf.name], build.layouts[buf.name]))
        cmd += ["--patch", "%s@0x%04X" % (path, build.addresses[buf.name])]
    dump = os.path.join(outdir, "memory.bin")
    ranges = os.path.join(outdir, "executed.txt")
    cmd += ["--start", "0x%04X" % build.start, "--entry", "0x%04X" % build.entry,
            "--dump", dump, "--executed", ranges, "--max-cycles", str(MAX_CYCLES)]
    try:
        out = toolchains.run(cmd, outdir, "tisple race")
    except toolchains.BuildError as e:
        result.error = "run: %s" % str(e).splitlines()[-1]
        return result
    for line in out.splitlines():
        if line.startswith("kernel_cycles "):
            result.cycles = int(line.split()[1])
    with open(dump, "rb") as f:
        memory = f.read()
    result.error = check_outputs(kernel, build, inputs, memory)
    result.ok = result.error == ""
    result.bytes, result.objects = code_bytes(build, parse_ranges(ranges))
    return result


def best_by_compiler(results, compilers):
    best = {}
    for c in compilers:
        good = [r for r in results if r.compiler == c and r.ok]
        if good:
            best[c] = min(good, key=lambda r: (r.cycles, r.bytes))
    return best


def ratio_text(best):
    tisple = best.get("tisple")
    rivals = [r for c, r in best.items() if c != "tisple"]
    if tisple is None or not rivals:
        return "-"
    fastest = min(rivals, key=lambda r: r.cycles)
    return "%.2f" % (tisple.cycles / fastest.cycles)


def one_line(text, width):
    lines = [l for l in text.splitlines() if l.strip()]
    return (lines[-1] if lines else "")[:width].replace("|", "/")


def kernel_table(kernel, results, compilers):
    best = best_by_compiler(results, compilers)
    lines = ["### %s: %s" % (kernel.name, kernel.title), "",
             "| Compiler | Cycles | Bytes | Best setting |", "| --- | ---: | ---: | --- |"]
    for c in compilers:
        r = best.get(c)
        if r is None:
            errors = [x.error for x in results if x.compiler == c]
            lines.append("| %s | failed | | %s |" % (c, one_line((errors or ["not run"])[0], 80)))
            continue
        lines.append("| %s | %d | %d | %s |" % (c, r.cycles, r.bytes, r.variant or "-"))
    if best:
        winner = min(best.values(), key=lambda r: (r.cycles, r.bytes))
        lines.append("")
        lines.append("Winner: %s. Tisple / best other: %s." % (winner.compiler, ratio_text(best)))
    return "\n".join(lines)


def geomean(values):
    return math.exp(sum(math.log(v) for v in values) / len(values)) if values else 0.0


def geomean_row(by_kernel, compilers):
    """each compiler's cycles over the fastest compiler's, and Tisple over the fastest other,
    as geometric means over the kernels where they built and ran correctly"""
    slower = {c: [] for c in compilers}
    tisple_ratios = []
    for _kernel, results in by_kernel:
        best = best_by_compiler(results, compilers)
        if not best:
            continue
        fastest = min(r.cycles for r in best.values())
        for c, r in best.items():
            slower[c].append(r.cycles / fastest)
        rivals = [r.cycles for c, r in best.items() if c != "tisple"]
        if "tisple" in best and rivals:
            tisple_ratios.append(best["tisple"].cycles / min(rivals))
    cells = []
    for c in compilers:
        n = len(slower[c])
        note = "" if n == len(by_kernel) else " (%d of %d)" % (n, len(by_kernel))
        cells.append("%.2fx%s" % (geomean(slower[c]), note) if n else "-")
    tisple = "%.3f" % geomean(tisple_ratios) if tisple_ratios else "-"
    return "| geomean, x the fastest | %s | | %s |" % (" | ".join(cells), tisple)


def summary_table(by_kernel, compilers, label="Kernel"):
    head = "| %s | " % label + " | ".join(compilers) + " | Winner | Tisple / best other |"
    lines = [head, "| --- |" + " ---: |" * len(compilers) + " --- | ---: |"]
    for kernel, results in by_kernel:
        best = best_by_compiler(results, compilers)
        cells = []
        for c in compilers:
            r = best.get(c)
            cells.append("%d / %d" % (r.cycles, r.bytes) if r else "failed")
        winner = min(best.values(), key=lambda r: (r.cycles, r.bytes)).compiler if best else "-"
        lines.append("| %s | %s | %s | %s |" % (kernel.name, " | ".join(cells), winner,
                                                ratio_text(best)))
    if len(by_kernel) > 1:
        lines.append(geomean_row(by_kernel, compilers))
    return "\n".join(lines)


def variants_table(kernel, results):
    lines = ["| Compiler | Setting | Cycles | Bytes | Result |", "| --- | --- | ---: | ---: | --- |"]
    for r in results:
        status = "ok" if r.ok else one_line(r.error, 70)
        lines.append("| %s | %s | %d | %d | %s |" % (r.compiler, r.variant or "-", r.cycles,
                                                     r.bytes, status))
    return "\n".join(lines)


def run_gaps(chosen, top):
    """tisple gaps on each Tisple kernel, one at a time, on the race's inputs, into
    races/out/gaps/<kernel>.txt"""
    os.makedirs(os.path.join(OUT, "gaps"), exist_ok=True)
    for k in chosen:
        main = "kernel.tsp" if k.folder == "kernels" else "program.tsp"
        src = os.path.join(toolchains.kernel_dir(k), main)
        cmd = [toolchains.tisple_binary(), "gaps", src, "--sim", "--top", str(top)]
        inputs = kernel_specs.make_inputs(k)
        for buf in k.buffers:
            path = os.path.join(OUT, "gaps", "%s_%s.bin" % (k.name, buf.name))
            write_file(path, encode(buf, inputs[buf.name], "planar"))
            cmd += ["--patch", "%s@0x%04X" % (path, buf.address)]
        try:
            out = toolchains.run(cmd, toolchains.REPO, "tisple gaps")
        except toolchains.BuildError as e:
            out = str(e)
        path = os.path.join(OUT, "gaps", k.name + ".txt")
        with open(path, "w") as f:
            f.write(out)
        summary = [l for l in out.splitlines() if l.startswith("could save")]
        print("gaps %s: %s (%s)" % (k.name, summary[0] if summary else "no summary",
                                    os.path.relpath(path, toolchains.REPO)))


def update_doc(text, begin_mark, end_mark):
    with open(DOC) as f:
        doc = f.read()
    begin = doc.index(begin_mark) + len(begin_mark)
    end = doc.index(end_mark)
    with open(DOC, "w") as f:
        f.write(doc[:begin] + "\n\n" + text + "\n\n" + doc[end:])


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--kernels", default="")
    parser.add_argument("--compilers", default=",".join(ALL_COMPILERS))
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--variants", action="store_true", help="every setting, not just the best")
    parser.add_argument("--update-doc", action="store_true", help="rewrite the tables in docs/races.md")
    parser.add_argument("--programs", action="store_true",
                        help="the program race (races/programs/) instead of the kernels")
    parser.add_argument("--gaps", action="store_true",
                        help="only run tisple gaps on the Tisple kernels (races/out/gaps/)")
    args = parser.parse_args()
    compilers = [c for c in args.compilers.split(",") if c]
    everything = program_specs.load() if args.programs else kernel_specs.KERNELS
    chosen = everything
    if args.kernels:
        by_name = {k.name: k for k in everything}
        chosen = [by_name[n] for n in args.kernels.split(",")]
    if args.gaps:
        run_gaps(chosen, 12)
        return 0

    jobs = [(k, v) for k in chosen for v in toolchains.variants_for(k, compilers)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(lambda kv: measure(*kv), jobs))

    by_kernel = []
    for k in chosen:
        by_kernel.append((k, [r for r in results if r.kernel == k.name]))
    results_dir = os.path.join(OUT, "programs") if args.programs else OUT
    os.makedirs(results_dir, exist_ok=True)
    with open(os.path.join(results_dir, "results.json"), "w") as f:
        json.dump([r.as_json() for r in results], f, indent=1)

    sections = ["Cycles / bytes, best setting of each compiler:", "",
                summary_table(by_kernel, compilers, "Program" if args.programs else "Kernel")]
    for k, rs in by_kernel:
        sections += ["", kernel_table(k, rs, compilers)]
        if args.variants:
            sections += ["", variants_table(k, rs)]
    text = "\n".join(sections)
    print(text)
    failed = [r for r in results if not r.ok]
    for r in failed:
        print("note: %s %s %s: %s" % (r.kernel, r.compiler, r.variant, one_line(r.error, 200)),
              file=sys.stderr)
    if args.update_doc:
        marks = (PROGRAM_DOC_BEGIN, PROGRAM_DOC_END) if args.programs else (DOC_BEGIN, DOC_END)
        update_doc("\n".join(sections[:3] + [s for k, rs in by_kernel
                                             for s in ["", kernel_table(k, rs, compilers)]]),
                   *marks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
