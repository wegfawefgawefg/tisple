#!/usr/bin/env python3
"""The random-program race: the fuzz generator's programs in Tisple and in C, built by every
compiler at its best kernel-race setting and timed in our simulator (docs/races.md,
"Random programs"; races/README.md has the commands).

    races/random.sh --seed S --count N [--jobs J] [--out DIR] [--compilers a,b] [--host]
    races/random.sh --report [--out DIR] [--worst K] [--update-doc]
    races/random.sh --one SEED [--out DIR]

For each seed `tisple race-gen` writes t.tsp, t.c and the interpreter's checksum. Each
build runs under `tisple race`, timed from t.init's return (init sets every global's start
value, so no compiler can fold them) to t.checksum's entry, then on to main's return; the
checksum it left at $BF00 must be the interpreter's. A wrong checksum is a bug in that compiler; when a C compiler is wrong, the C
itself is checked on the host with sanitizers (and with --host, every program is), so a
fault in our C writer is not blamed on a C compiler.

A run is deterministic and resumable: DIR/results-S-E.jsonl gets one line per seed, and a
rerun skips the seeds already there. Shards are seed ranges; --report reads every
results-*.jsonl in DIR. Programs with bugs are kept in DIR/bugs/<seed>/.
"""

import argparse
import concurrent.futures
import json
import os
import shutil
import sys

import random_build
import random_report
import toolchains
from toolchains import BuildError

ALL_COMPILERS = ["cc65", "llvm-mos", "oscar64", "tisple"]
DEFAULT_OUT = os.path.join(toolchains.HERE, "out", "random")
MAX_CYCLES = 400000000


def parse_ranges(path):
    executed = set()
    with open(path) as f:
        for line in f:
            first, last = (int(x, 16) for x in line.split())
            executed.update(range(first, last + 1))
    return executed


def code_bytes(objects, executed):
    total = 0
    covered = set()
    for _name, first, last in objects:
        span = range(first, last + 1)
        if any(a in executed for a in span):
            total += len(span)
        covered.update(span)
    return total + len(executed - covered)


def measure(compiler, program_dir):
    """one compiler on one program: cycles, bytes and the checksum it left, or an error"""
    out = {"ok": False, "cycles": 0, "bytes": 0, "checksum": None, "error": ""}
    try:
        build = random_build.build(compiler, program_dir)
    except (BuildError, KeyError, OSError, IndexError) as e:
        out["error"] = "build: %s" % str(e)[-2000:]
        return out
    outdir = os.path.join(os.path.abspath(program_dir), compiler)
    cmd = [toolchains.tisple_binary(), "race"]
    for i, (address, data) in enumerate(build.loads):
        path = os.path.join(outdir, "load%d.bin" % i)
        with open(path, "wb") as f:
            f.write(data)
        cmd += ["--load", "%s@0x%04X" % (path, address)]
    # opaque, the u16 t.init adds to every start value, is 0 (tests/fuzz_gen_race.cpp)
    zero = os.path.join(outdir, "opaque.bin")
    with open(zero, "wb") as f:
        f.write(b"\0\0")
    cmd += ["--patch", "%s@0x%04X" % (zero, random_build.OPAQUE_ADDRESS)]
    ranges = os.path.join(outdir, "executed.txt")
    cmd += ["--start", "0x%04X" % build.start, "--entry", "0x%04X" % build.entry,
            "--after", "0x%04X" % build.after, "--stop", "0x%04X" % build.stop,
            "--executed", ranges,
            "--word", "0x%04X" % random_build.CHECKSUM_ADDRESS, "--max-cycles", str(MAX_CYCLES)]
    try:
        text = random_build.run(cmd, outdir, "tisple race", timeout=300)
    except BuildError as e:
        out["error"] = "run: %s" % str(e).strip().splitlines()[-1]
        return out
    for line in text.splitlines():
        parts = line.split()
        if parts[:1] == ["kernel_cycles"]:
            out["cycles"] = int(parts[1])
        elif parts[:1] == ["word"]:
            out["checksum"] = int(parts[2])
    out["bytes"] = code_bytes(build.objects, parse_ranges(ranges))
    out["ok"] = True
    return out


def find_bugs(record, program_dir, host_cc, always_host):
    """a bug per compiler whose build failed or whose checksum is not the interpreter's"""
    want = record["checksum"]
    bugs = []
    host = None
    if always_host or any(not r["ok"] or r["checksum"] != want
                          for c, r in record["results"].items() if c != "tisple"):
        if host_cc:
            got, error = random_build.host_check(program_dir, host_cc)
            host = {"checksum": got, "error": error}
            record["host"] = host
    writer_wrong = host is not None and host["checksum"] != want
    if writer_wrong:
        bugs.append({"compiler": "c-writer", "kind": "host build disagrees with the interpreter",
                     "got": host["checksum"], "want": want, "error": host["error"][-500:]})
    for compiler, r in record["results"].items():
        if not r["ok"]:
            kind = "build or run failed"
        elif r["checksum"] != want:
            kind = "wrong checksum"
        else:
            continue
        if compiler != "tisple" and writer_wrong:
            continue  # the C was wrong, not the compiler
        bugs.append({"compiler": compiler, "kind": kind, "got": r["checksum"], "want": want,
                     "error": r["error"][-500:]})
    return bugs


def race_seed(seed, out_dir, compilers, host_cc, always_host, keep, work_name="work-one"):
    # each shard works in a folder of its own, so shards can share out_dir
    work = os.path.join(out_dir, work_name)
    program_dir = os.path.join(work, str(seed))
    record = {"seed": seed}
    os.makedirs(work, exist_ok=True)
    try:
        random_build.run([toolchains.tisple_binary(), "race-gen", "--seed", str(seed),
                          "--out", work], work, "tisple race-gen")
        with open(os.path.join(program_dir, "info.json")) as f:
            info = json.load(f)
    except (BuildError, OSError, ValueError) as e:
        record.update(status="race-gen failed", message=str(e)[-1000:])
        shutil.rmtree(program_dir, ignore_errors=True)
        return record
    record.update(status=info["status"], checksum=info["checksum"], steps=info["steps"],
                  shape=random_report.shape(info["work"]))
    if info["status"] != "ok":
        record["message"] = info.get("message", "")[-1000:]
        shutil.rmtree(program_dir, ignore_errors=True)
        return record
    record["results"] = {c: measure(c, program_dir) for c in compilers}
    record["bugs"] = find_bugs(record, program_dir, host_cc, always_host)
    if record["bugs"]:
        dest = os.path.join(out_dir, "bugs", str(seed))
        shutil.rmtree(dest, ignore_errors=True)
        shutil.copytree(program_dir, dest)
    if keep:
        dest = os.path.join(out_dir, "one", str(seed))
        shutil.rmtree(dest, ignore_errors=True)
        shutil.copytree(program_dir, dest)
    shutil.rmtree(program_dir, ignore_errors=True)
    return record


def done_seeds(path):
    seeds = set()
    if not os.path.exists(path):
        return seeds
    with open(path) as f:
        for line in f:
            try:
                seeds.add(json.loads(line)["seed"])
            except (ValueError, KeyError):
                pass  # a line cut short when a run was stopped
    return seeds


def find_host_cc():
    for cc in (os.environ.get("RACE_HOST_CC"), "cc", "clang", "gcc"):
        if cc and shutil.which(cc):
            return cc
    return None


def compact(record):
    """the record as one results line: no empty error texts"""
    out = dict(record)
    if "results" in out:
        out["results"] = {c: {k: v for k, v in r.items() if k != "error" or v}
                          for c, r in out["results"].items()}
    return json.dumps(out, sort_keys=True) + "\n"


def report_bugs(record):
    for bug in record.get("bugs", []):
        print("seed %d: %s %s (got %s, want %s)" % (record["seed"], bug["compiler"], bug["kind"],
                                                    bug["got"], bug["want"]), file=sys.stderr)


def run_shard(args, compilers):
    os.makedirs(args.out, exist_ok=True)
    last = args.seed + args.count - 1
    path = os.path.join(args.out, "results-%d-%d.jsonl" % (args.seed, last))
    work_name = "work-%d-%d" % (args.seed, last)
    done = done_seeds(path)
    todo = [s for s in range(args.seed, last + 1) if s not in done]
    host_cc = None if args.no_host else find_host_cc()
    print("random race: seeds %d..%d, %d to run (%d done), %d jobs, into %s" %
          (args.seed, last, len(todo), len(done), args.jobs, path), file=sys.stderr)
    finished = 0
    pending = set()
    seeds = iter(todo)
    with open(path, "a") as results, \
            concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        while True:
            # a few seeds queued per job, not millions of futures at once
            for seed in seeds:
                pending.add(pool.submit(race_seed, seed, args.out, compilers, host_cc,
                                        args.host, False, work_name))
                if len(pending) >= args.jobs * 4:
                    break
            if not pending:
                break
            done, pending = concurrent.futures.wait(
                pending, return_when=concurrent.futures.FIRST_COMPLETED)
            for future in done:
                record = future.result()
                results.write(compact(record))
                results.flush()
                report_bugs(record)
                finished += 1
                if finished % 500 == 0:
                    print("  %d of %d" % (finished, len(todo)), file=sys.stderr)
    shutil.rmtree(os.path.join(args.out, work_name), ignore_errors=True)
    # the same lines whatever order the jobs finished in
    with open(path) as f:
        lines = [l for l in f if l.strip()]
    lines.sort(key=lambda l: json.loads(l)["seed"])
    with open(path, "w") as f:
        f.writelines(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--count", type=int, default=0)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--compilers", default=",".join(ALL_COMPILERS))
    parser.add_argument("--host", action="store_true",
                        help="check every program's C on the host, not only on a mismatch")
    parser.add_argument("--no-host", action="store_true", help="never build on the host")
    parser.add_argument("--report", action="store_true", help="summarize DIR's results")
    parser.add_argument("--worst", type=int, default=20,
                        help="--report: rebuild the K worst losses into DIR/worst/")
    parser.add_argument("--update-doc", action="store_true",
                        help="--report: write the summary into docs/races.md")
    parser.add_argument("--one", type=int, default=0, help="race one seed, kept in DIR/one/")
    args = parser.parse_args()
    compilers = [c for c in args.compilers.split(",") if c]
    if args.one:
        record = race_seed(args.one, args.out, compilers, find_host_cc(), args.host, True)
        print(json.dumps(record, indent=1, sort_keys=True))
        report_bugs(record)
        return 0
    if args.report:
        summary = random_report.summarize(args.out)
        text = random_report.summary_text(summary)
        if args.worst:
            saved = random_report.save_worst(summary, args.out, args.worst,
                                             lambda s: race_seed(s, args.out, compilers,
                                                                 find_host_cc(), False, True))
            text += "\n\n" + saved
        print(text)
        if args.update_doc:
            random_report.update_doc(text)
        return 0
    if args.count <= 0:
        parser.error("give --count N (and --seed S), --report or --one SEED")
    run_shard(args, compilers)
    return 0


if __name__ == "__main__":
    sys.exit(main())
