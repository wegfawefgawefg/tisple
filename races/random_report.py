"""The random-program race's summary: win, loss and tie rates, Tisple's ratio against the
fastest other compiler, where the losing programs spend their work, the worst losses and
the bugs found. It reads the results one line at a time, so millions of programs fit.

Ratio is Tisple's cycles over the fastest correct C compiler's, per program; below 1 Tisple
wins, 1 is a tie. A program where Tisple or every C compiler is wrong is left out of the
ratios and counted as a bug.

A program's shape (crude, from tisple race-gen's "work": each feature of t.run as often as
the interpreter ran it, times a rough 6502 cost) is the share of its work in each feature
family: "divide 60%, multiply 20%, ...".
"""

import glob
import json
import math
import os
import shutil

import toolchains

DOC = os.path.join(toolchains.REPO, "docs", "races.md")
DOC_BEGIN = "<!-- random race: begin (races/random.sh --report --update-doc writes this part) -->"
DOC_END = "<!-- random race: end -->"
OTHERS = ["cc65", "llvm-mos", "oscar64"]
BUCKETS = [0.5, 0.75, 0.9, 1.0, 1.0001, 1.1, 1.25, 1.5, 2.0]
WIDTHS = {"u8": 8, "i8": 8, "bool": 8, "enum": 8, "u16": 16, "i16": 16}
# a rough 6502 cost per evaluation: a divide is a loop of hundreds of cycles, an add a few
COSTS = {"divide": 300, "multiply": 150, "shift by var": 40, "call": 30}


# --- shapes ------------------------------------------------------------------------------


def family(feature):
    """a feature name from tisple race-gen (src/race/race_shape.cpp) as one of a few families"""
    words = feature.split()
    op = words[0]
    width = "16-bit" if len(words) > 1 and words[1] in ("u16", "i16") else "8-bit"
    if op == "*":
        return "multiply"
    if op in ("/", "%"):
        return "divide"
    if op in ("<<", ">>"):
        return "shift by var" if feature.endswith("by var") else "shift by const"
    if op in ("+-", "&|^"):
        return "%s %s" % (width, op)
    if op == "~":
        return "~"
    if op == "cmp":
        return ("signed" if words[1].startswith("i") else "unsigned") + " compare"
    if op == "as":
        a, b = WIDTHS.get(words[1], 8), WIDTHS.get(words[2], 8)
        return "widen" if b > a else "narrow" if b < a else "as, same width"
    if feature.startswith(("struct", "at struct")):
        return "struct"
    if feature.startswith(("load/store", "pointer", "addr_of")):
        return "pointer"
    if op == "at":
        return "u16 index" if feature.endswith("by u16") else "u8 index"
    if op == "match":
        return "match"
    if op == "for":
        return "%s for" % width
    if feature in ("not", "and/or"):
        return "and/or/not"
    return feature


def shape(work):
    """family -> share of the weighed work, the largest eight, from race-gen's "work" """
    weights = {}
    for f, count in work.items():
        fam = family(f)
        cost = COSTS.get(fam, 8)
        if " u16" in f or " i16" in f:
            cost *= 2
        weights[fam] = weights.get(fam, 0) + count * cost
    total = sum(weights.values())
    if not total:
        return {}
    top = sorted(weights.items(), key=lambda kv: (-kv[1], kv[0]))[:8]
    return {f: round(w / total, 3) for f, w in top}


def ranked(shares):
    return sorted(shares.items(), key=lambda kv: (-kv[1], kv[0]))


def signature(shares, limit=3):
    return ", ".join("%s %d%%" % (f, round(100 * x)) for f, x in ranked(shares)[:limit])


# --- numbers -----------------------------------------------------------------------------


def geomean(values):
    values = [v for v in values if v > 0]
    if not values:
        return float("nan")
    return math.exp(sum(math.log(v) for v in values) / len(values))


def percentile(sorted_values, p):
    if not sorted_values:
        return float("nan")
    i = min(len(sorted_values) - 1, max(0, int(round(p / 100.0 * (len(sorted_values) - 1)))))
    return sorted_values[i]


def pct(n, total):
    return "%.1f%%" % (100.0 * n / total) if total else "-"


# --- reading -----------------------------------------------------------------------------


class Row:
    """one raced program: Tisple right and at least one C compiler right"""
    __slots__ = ("seed", "ratio", "tisple", "tisple_bytes", "best", "best_cycles", "others",
                 "shape")


def read_records(out_dir):
    """every results-*.jsonl line in out_dir, each seed once"""
    seen = set()
    for path in sorted(glob.glob(os.path.join(out_dir, "results-*.jsonl"))):
        with open(path) as f:
            for line in f:
                try:
                    r = json.loads(line)
                except ValueError:
                    continue  # a line cut short when a run was stopped
                if r["seed"] in seen:
                    continue
                seen.add(r["seed"])
                yield r


def make_row(r):
    if r.get("status") != "ok" or "results" not in r:
        return None
    want = r["checksum"]
    good = {c: x for c, x in r["results"].items() if x["ok"] and x["checksum"] == want}
    if "tisple" not in good:
        return None
    others = {c: (x["cycles"], x["bytes"]) for c, x in good.items() if c != "tisple"}
    if not others:
        return None
    row = Row()
    row.seed = r["seed"]
    row.tisple = good["tisple"]["cycles"]
    row.tisple_bytes = good["tisple"]["bytes"]
    row.best_cycles, row.best = min((cyc, c) for c, (cyc, _b) in others.items())
    row.ratio = row.tisple / row.best_cycles if row.best_cycles else float("inf")
    row.others = others
    row.shape = r.get("shape", {})
    return row


class Summary:
    def __init__(self):
        self.programs = 0
        self.seeds = []
        self.statuses = {}
        self.bugs = {}  # (compiler, kind) -> seeds
        self.rows = []

    def add(self, r):
        self.programs += 1
        self.seeds.append(r["seed"])
        self.statuses[r.get("status")] = self.statuses.get(r.get("status"), 0) + 1
        for b in r.get("bugs", []):
            self.bugs.setdefault((b["compiler"], b["kind"]), []).append(r["seed"])
        row = make_row(r)
        if row is not None:
            self.rows.append(row)

    def worst(self, count):
        return sorted(self.rows, key=lambda x: (-x.ratio, x.seed))[:count]


# --- tables ------------------------------------------------------------------------------


def ratio_lines(rows):
    ratios = sorted(x.ratio for x in rows)
    n = len(ratios)
    wins = sum(1 for x in ratios if x < 1)
    ties = sum(1 for x in ratios if x == 1)
    losses = n - wins - ties
    lines = ["Tisple against the fastest other compiler, per program: wins %d (%s), ties %d (%s), "
             "losses %d (%s). Geometric mean of the ratios %.3f." % (
                 wins, pct(wins, n), ties, pct(ties, n), losses, pct(losses, n), geomean(ratios)),
             "",
             "| Percentile | 1 | 5 | 10 | 25 | 50 | 75 | 90 | 95 | 99 | max |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
             "| Tisple / best other | " + " | ".join(
                 "%.2f" % percentile(ratios, p) for p in (1, 5, 10, 25, 50, 75, 90, 95, 99, 100)) +
             " |", ""]
    head = ["< %.2f" % BUCKETS[0]]
    counts = [sum(1 for x in ratios if x < BUCKETS[0])]
    for lo, hi in zip(BUCKETS, BUCKETS[1:]):
        head.append("1.00" if lo == 1.0 else "%.2f-%.2f" % (lo, hi))
        counts.append(sum(1 for x in ratios if lo <= x < hi))
    head.append(">= %.2f" % BUCKETS[-1])
    counts.append(sum(1 for x in ratios if x >= BUCKETS[-1]))
    lines += ["| Ratio | " + " | ".join(head) + " |", "| --- |" + " ---: |" * len(head),
              "| Programs | " + " | ".join(str(c) for c in counts) + " |"]
    return lines


def compiler_lines(rows):
    lines = ["| Against | Programs | Tisple faster | Cycles, geomean of Tisple / it | "
             "Bytes, geomean of Tisple / it | Fastest of all |",
             "| --- | ---: | ---: | ---: | ---: | ---: |"]
    for c in OTHERS:
        cyc = []
        byt = []
        faster = 0
        fastest = 0
        for x in rows:
            if c not in x.others:
                continue
            cycles, size = x.others[c]
            cyc.append(x.tisple / cycles if cycles else float("inf"))
            if size:
                byt.append(x.tisple_bytes / size)
            faster += x.tisple < cycles
            fastest += x.best == c and cycles < x.tisple
        if cyc:
            lines.append("| %s | %d | %s | %.3f | %.3f | %s |" % (
                c, len(cyc), pct(faster, len(cyc)), geomean(cyc), geomean(byt),
                pct(fastest, len(rows))))
    return lines


def dominant_lines(rows, top):
    """programs by the family most of their work is in: how much Tisple loses there"""
    groups = {}
    for x in rows:
        r = ranked(x.shape)
        groups.setdefault(r[0][0] if r else "-", []).append(x)
    entries = [(geomean([x.ratio for x in v]), f, v) for f, v in groups.items()
               if len(v) >= max(3, len(rows) // 500)]
    entries.sort(key=lambda e: -e[0])
    lines = ["| Most of the work is | Programs | Tisple loses | Geomean ratio | Median ratio | "
             "Fastest most often |", "| --- | ---: | ---: | ---: | ---: | --- |"]
    for g, f, v in entries[:top]:
        ratios = sorted(x.ratio for x in v)
        winners = {}
        for x in v:
            w = x.best if x.ratio > 1 else "tisple"
            winners[w] = winners.get(w, 0) + 1
        best = max(winners.items(), key=lambda kv: (kv[1], kv[0]))
        lines.append("| %s | %d | %s | %.2f | %.2f | %s (%s) |" % (
            f, len(v), pct(sum(1 for x in ratios if x > 1), len(v)), g, percentile(ratios, 50),
            best[0], pct(best[1], len(v))))
    return lines


def pair_lines(rows, top):
    """the worst 5% of programs by their two heaviest families"""
    worst = sorted(rows, key=lambda x: -x.ratio)[:max(1, len(rows) // 20)]
    groups = {}
    for x in worst:
        key = " + ".join(sorted(f for f, _s in ranked(x.shape)[:2]))
        groups.setdefault(key, []).append(x.ratio)
    entries = sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0]))[:top]
    lines = ["| The worst %d losses, by their two heaviest families | Programs | Geomean ratio |" %
             len(worst), "| --- | ---: | ---: |"]
    for key, v in entries:
        lines.append("| %s | %d | %.2f |" % (key, len(v), geomean(v)))
    return lines


def worst_lines(rows):
    lines = ["| Seed | Ratio | Fastest | Tisple cycles | Its cycles | Where the work is |",
             "| ---: | ---: | --- | ---: | ---: | --- |"]
    for x in rows:
        lines.append("| %d | %.2f | %s | %d | %d | %s |" % (x.seed, x.ratio, x.best, x.tisple,
                                                         x.best_cycles, signature(x.shape)))
    return lines


def bug_lines(bugs):
    if not bugs:
        return ["No compiler disagreed with the interpreter."]
    lines = ["| Who is wrong | What | Programs | Seeds |", "| --- | --- | ---: | --- |"]
    for (c, kind), seeds in sorted(bugs.items()):
        seeds = sorted(seeds)
        shown = ", ".join(str(s) for s in seeds[:12]) + (" ..." if len(seeds) > 12 else "")
        lines.append("| %s | %s | %d | %s |" % (c, kind, len(seeds), shown))
    return lines


def summary_text(s, worst_count=15):
    rows = s.rows
    statuses = ", ".join("%d %s" % (n, st) for st, n in
                         sorted(s.statuses.items(), key=lambda kv: (-kv[1], str(kv[0]))))
    head = "%d programs (seeds %d..%d): %s; %d raced (Tisple and a C compiler right)." % (
        s.programs, min(s.seeds) if s.seeds else 0, max(s.seeds) if s.seeds else 0, statuses,
        len(rows))
    if not rows:
        return head
    parts = [head, ""] + ratio_lines(rows) + ["", "Against each compiler:", ""]
    parts += compiler_lines(rows)
    parts += ["", "By where the work is (the feature family with the largest share of t.run's "
              "weighed work), worst first:", ""] + dominant_lines(rows, 16)
    parts += ["", ""] + pair_lines(rows, 10)
    parts += ["", "Worst losses:", ""] + worst_lines(s.worst(worst_count))
    parts += ["", "Bugs (a compiler whose checksum is not the interpreter's, or that failed to "
              "build or run the program):", ""] + bug_lines(s.bugs)
    return "\n".join(parts)


def summarize(out_dir):
    s = Summary()
    for r in read_records(out_dir):
        s.add(r)
    return s


def save_worst(s, out_dir, count, rerun):
    """rebuilds the worst losses into out_dir/worst/<rank>-<seed>/: the program in both
    languages, Tisple's listing and the fastest compiler's"""
    rows = s.worst(count)
    dest_root = os.path.join(out_dir, "worst")
    shutil.rmtree(dest_root, ignore_errors=True)
    os.makedirs(dest_root)
    for rank, x in enumerate(rows, 1):
        rerun(x.seed)
        src = os.path.join(out_dir, "one", str(x.seed))
        dest = os.path.join(dest_root, "%02d-%d" % (rank, x.seed))
        os.makedirs(dest)
        for name in ("t.tsp", "t.c", "info.json"):
            shutil.copy(os.path.join(src, name), dest)
        shutil.copy(os.path.join(src, "tisple", "k.lst"), os.path.join(dest, "tisple.lst"))
        listing = "k.asm" if x.best == "oscar64" else "k.lst"
        shutil.copy(os.path.join(src, x.best, listing), os.path.join(dest, x.best + ".lst"))
        shutil.rmtree(src, ignore_errors=True)
    return "The %d worst losses, each with Tisple's listing and the fastest compiler's, are in %s." % (
        len(rows), dest_root)


def update_doc(text):
    with open(DOC) as f:
        doc = f.read()
    if DOC_BEGIN not in doc:
        doc += "\n## Random programs\n\n%s\n%s\n" % (DOC_BEGIN, DOC_END)
    begin = doc.index(DOC_BEGIN) + len(DOC_BEGIN)
    end = doc.index(DOC_END)
    with open(DOC, "w") as f:
        f.write(doc[:begin] + "\n\n" + text + "\n\n" + doc[end:])
