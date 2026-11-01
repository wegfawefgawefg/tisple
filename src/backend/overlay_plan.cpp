// Which code takes turns in the overlay region: overlay_plan.hpp.

#include "backend/overlay_plan.hpp"

#include "backend/analysis.hpp"
#include "backend/overlay_graph.hpp"

#include <algorithm>
#include <cstdio>

namespace tisple {

namespace {

constexpr std::uint32_t kShareFraction = 8;   // share more than an eighth of the smaller: one group
constexpr std::uint32_t kMinGroupWeight = 64; // expressions: less is not worth a load

struct Cluster {
    std::vector<std::int32_t> roots;
    std::vector<bool> group;
    std::uint32_t weight = 0;
};

struct Planner {
    OverlayGraph g;
    // by function: the facts that say whether two groups may take turns (Guards::screens, or
    // Guards::functions for roots the program marks :overlay)
    const std::vector<GuardFacts>* facts = nullptr;
    std::vector<bool> candidate;          // by function: may be a root
    std::vector<std::vector<bool>> alone; // by function: GroupOf it alone (candidates only)
    std::vector<Cluster> clusters;
    std::vector<std::vector<bool>> runs; // by cluster: RunsFrom its group
};

// every function a group's code may run: its members' callees, theirs, ...
std::vector<bool> RunsFrom(const OverlayGraph& g, const std::vector<bool>& group) {
    const Program& program = *g.program;
    std::vector<bool> seen(group.size(), false);
    std::vector<std::int32_t> work;
    for (std::size_t f = 0; f < group.size(); f++) {
        if (group[f]) {
            work.push_back(static_cast<std::int32_t>(f));
        }
    }
    while (!work.empty()) {
        std::size_t f = static_cast<std::size_t>(work.back());
        work.pop_back();
        for (std::int32_t callee : program.functions[f].callees) {
            if (!seen[static_cast<std::size_t>(callee)]) {
                seen[static_cast<std::size_t>(callee)] = true;
                work.push_back(callee);
            }
        }
    }
    return seen;
}

std::int32_t AddCluster(Planner& p, std::vector<std::int32_t> roots) {
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    Cluster c;
    c.roots = roots;
    c.group = roots.size() == 1 && !p.alone[static_cast<std::size_t>(roots[0])].empty()
                  ? p.alone[static_cast<std::size_t>(roots[0])]
                  : GroupOf(p.g, roots);
    c.weight = Weight(p.g, c.group);
    p.clusters.push_back(c);
    p.runs.push_back(RunsFrom(p.g, c.group));
    return static_cast<std::int32_t>(p.clusters.size() - 1);
}

// how much more two clusters' roots reach together than apart: the code they share
std::uint32_t Shared(const Planner& p, const Cluster& x, const Cluster& y) {
    std::vector<std::int32_t> roots = x.roots;
    roots.insert(roots.end(), y.roots.begin(), y.roots.end());
    std::uint32_t both = Weight(p.g, GroupOf(p.g, roots));
    std::uint32_t apart = x.weight + y.weight;
    return both > apart ? both - apart : 0;
}

bool ShareCode(const Planner& p, const Cluster& x, const Cluster& y) {
    return Shared(p, x, y) * kShareFraction > std::min(x.weight, y.weight);
}

bool Overlap(const std::vector<bool>& a, const std::vector<bool>& b) {
    for (std::size_t f = 0; f < a.size(); f++) {
        if (a[f] && b[f]) {
            return true;
        }
    }
    return false;
}

// both may be in the program as groups at once: no function in both, and neither's code
// reaching the other's roots (a load would overwrite the code that asked for it)
bool Apart(const Planner& p, std::int32_t a, std::int32_t b) {
    const Cluster& x = p.clusters[static_cast<std::size_t>(a)];
    const Cluster& y = p.clusters[static_cast<std::size_t>(b)];
    if (Overlap(x.group, y.group)) {
        return false;
    }
    for (std::int32_t r : x.roots) {
        if (p.runs[static_cast<std::size_t>(b)][static_cast<std::size_t>(r)]) {
            return false;
        }
    }
    for (std::int32_t s : y.roots) {
        if (p.runs[static_cast<std::size_t>(a)][static_cast<std::size_t>(s)]) {
            return false;
        }
    }
    return true;
}

// two clusters may take turns in the region: apart, every pair of their roots with facts that
// contradict (they never run in one pass), and no code shared between them (parts of one
// screen, which change often)
bool TakeTurns(const Planner& p, std::int32_t a, std::int32_t b) {
    if (!Apart(p, a, b)) {
        return false;
    }
    const Cluster& x = p.clusters[static_cast<std::size_t>(a)];
    const Cluster& y = p.clusters[static_cast<std::size_t>(b)];
    for (std::int32_t r : x.roots) {
        for (std::int32_t s : y.roots) {
            if (!Contradict((*p.facts)[static_cast<std::size_t>(r)],
                            (*p.facts)[static_cast<std::size_t>(s)])) {
                return false;
            }
        }
    }
    return !ShareCode(p, x, y);
}

// the clusters inside one: its group's candidates no other of them reaches alone, each opened
// in turn; and the sets of them that share code, as one cluster each
void Open(Planner& p, std::int32_t at, std::vector<std::int32_t>& out) {
    const Cluster outer = p.clusters[static_cast<std::size_t>(at)];
    std::size_t count = outer.group.size();
    std::vector<std::int32_t> inside;
    for (std::size_t f = 0; f < count; f++) {
        bool root = std::find(outer.roots.begin(), outer.roots.end(), f) != outer.roots.end();
        if (outer.group[f] && p.candidate[f] && !root) {
            inside.push_back(static_cast<std::int32_t>(f));
        }
    }
    std::vector<std::int32_t> found;
    for (std::int32_t f : inside) {
        bool covered = false;
        for (std::int32_t other : inside) {
            covered =
                covered || (other != f &&
                            p.alone[static_cast<std::size_t>(other)][static_cast<std::size_t>(f)]);
        }
        if (!covered) {
            found.push_back(AddCluster(p, {f}));
        }
    }
    // the ones worth a load that share code, joined (one set per connected run of them)
    std::vector<std::size_t> set(found.size());
    for (std::size_t i = 0; i < found.size(); i++) {
        set[i] = i;
    }
    auto top = [&](std::size_t i) {
        while (set[i] != i) {
            i = set[i];
        }
        return i;
    };
    for (std::size_t i = 0; i < found.size(); i++) {
        const Cluster x = p.clusters[static_cast<std::size_t>(found[i])];
        for (std::size_t j = i + 1; j < found.size() && x.weight >= kMinGroupWeight; j++) {
            const Cluster& y = p.clusters[static_cast<std::size_t>(found[j])];
            if (y.weight >= kMinGroupWeight && top(i) != top(j) && ShareCode(p, x, y)) {
                set[top(j)] = top(i);
            }
        }
    }
    out.insert(out.end(), found.begin(), found.end());
    for (std::size_t i = 0; i < found.size(); i++) {
        std::vector<std::int32_t> roots;
        std::size_t parts = 0;
        for (std::size_t j = 0; j < found.size() && top(i) == i; j++) {
            if (top(j) == i) {
                const Cluster& part = p.clusters[static_cast<std::size_t>(found[j])];
                roots.insert(roots.end(), part.roots.begin(), part.roots.end());
                parts++;
            }
        }
        if (parts >= 2) {
            out.push_back(AddCluster(p, roots));
        }
    }
    for (std::int32_t c : found) {
        if (p.clusters[static_cast<std::size_t>(c)].weight >= 2 * kMinGroupWeight) {
            Open(p, c, out);
        }
    }
}

// of the clusters, the set that frees the most (their sum less the biggest) with every two
// taking turns and their sum within capacity: for each cluster as the biggest, the biggest of
// the rest that fit, in turn
std::vector<std::int32_t> Choose(const Planner& p, std::vector<std::int32_t> clusters,
                                 std::uint32_t capacity) {
    std::stable_sort(clusters.begin(), clusters.end(), [&](std::int32_t a, std::int32_t b) {
        return p.clusters[static_cast<std::size_t>(a)].weight >
               p.clusters[static_cast<std::size_t>(b)].weight;
    });
    std::vector<std::int32_t> best;
    std::uint64_t best_freed = 0;
    for (std::size_t first = 0; first < clusters.size(); first++) {
        std::vector<std::int32_t> taken;
        std::uint64_t sum = 0;
        for (std::size_t i = first; i < clusters.size(); i++) {
            std::int32_t c = clusters[i];
            std::uint32_t w = p.clusters[static_cast<std::size_t>(c)].weight;
            bool fits = w >= kMinGroupWeight && sum + w <= capacity;
            for (std::size_t t = 0; t < taken.size() && fits; t++) {
                fits = TakeTurns(p, c, taken[t]);
            }
            if (fits) {
                taken.push_back(c);
                sum += w;
            }
        }
        std::uint64_t largest =
            taken.empty() ? 0 : p.clusters[static_cast<std::size_t>(taken[0])].weight;
        if (taken.size() >= 2 && sum - largest > best_freed) {
            best = taken;
            best_freed = sum - largest;
        }
    }
    return best;
}

OverlayPlan PlanOf(const Planner& p, const std::vector<std::int32_t>& chosen) {
    OverlayPlan plan;
    for (std::int32_t c : chosen) {
        const Cluster& cluster = p.clusters[static_cast<std::size_t>(c)];
        OverlayGroupPlan group;
        group.roots = cluster.roots;
        for (std::size_t f = 0; f < cluster.group.size(); f++) {
            if (cluster.group[f]) {
                group.members.push_back(static_cast<std::int32_t>(f));
            }
        }
        group.weight = cluster.weight;
        plan.groups.push_back(group);
    }
    return plan;
}

Planner MakePlanner(const Program& program) {
    Planner p;
    p.g = MakeOverlayGraph(program);
    std::size_t count = program.functions.size();
    p.candidate.assign(count, false);
    p.alone.assign(count, {});
    return p;
}

// marked roots: each with what only it reaches; those that cannot take turns made one, until
// every two can
std::vector<std::int32_t> MarkedClusters(Planner& p, const std::vector<std::int32_t>& marked) {
    std::vector<std::int32_t> clusters;
    for (std::int32_t f : marked) {
        clusters.push_back(AddCluster(p, {f}));
    }
    for (bool merged = true; merged;) {
        merged = false;
        for (std::size_t i = 0; i < clusters.size() && !merged; i++) {
            for (std::size_t j = i + 1; j < clusters.size() && !merged; j++) {
                if (TakeTurns(p, clusters[i], clusters[j])) {
                    continue;
                }
                std::vector<std::int32_t> roots =
                    p.clusters[static_cast<std::size_t>(clusters[i])].roots;
                const std::vector<std::int32_t>& more =
                    p.clusters[static_cast<std::size_t>(clusters[j])].roots;
                roots.insert(roots.end(), more.begin(), more.end());
                clusters[i] = AddCluster(p, roots);
                clusters.erase(clusters.begin() + static_cast<std::ptrdiff_t>(j));
                merged = true;
            }
        }
    }
    return clusters;
}

} // namespace

OverlayPlan PlanOverlays(const Program& program, std::uint32_t capacity) {
    Planner p = MakePlanner(program);
    const OverlayGraph& g = p.g;
    std::size_t count = program.functions.size();
    std::vector<bool> reached = ReachedWithout(g, std::vector<bool>(count, false));
    std::vector<std::int32_t> marked;
    std::vector<std::int32_t> barred_marks;
    for (std::size_t f = 0; f < count; f++) {
        if (!program.functions[f].overlay) {
            continue;
        }
        bool usable = reached[f] && !g.barred[f];
        (usable ? marked : barred_marks).push_back(static_cast<std::int32_t>(f));
    }
    OverlayPlan plan;
    if (!marked.empty() || !barred_marks.empty()) {
        p.facts = &g.guards.functions;
        std::vector<std::int32_t> clusters = MarkedClusters(p, marked);
        plan = PlanOf(p, Choose(p, clusters, capacity));
        plan.marked = true;
        plan.resident_marks = barred_marks;
        return plan;
    }
    // no marks: the candidates are what runs only under a loop's own dispatch
    p.facts = &g.guards.screens;
    for (std::size_t f = 0; f < count; f++) {
        p.candidate[f] = reached[f] && !g.barred[f] && !g.guards.screens[f].empty() &&
                         g.weight[f] > kMaxInlineExprs;
        if (p.candidate[f]) {
            p.alone[f] = GroupOf(g, {static_cast<std::int32_t>(f)});
        }
    }
    Cluster top;
    top.group = reached;
    p.clusters.push_back(top);
    p.runs.push_back(reached);
    std::vector<std::int32_t> clusters;
    Open(p, 0, clusters);
    return PlanOf(p, Choose(p, clusters, capacity));
}

OverlayPlan PlanOverlaysTest(const Program& program, int pick) {
    Planner p = MakePlanner(program);
    const OverlayGraph& g = p.g;
    std::size_t count = program.functions.size();
    std::vector<bool> reached = ReachedWithout(g, std::vector<bool>(count, false));
    for (std::size_t f = 0; f < count; f++) {
        p.candidate[f] = reached[f] && !g.barred[f];
        if (p.candidate[f]) {
            p.alone[f] = GroupOf(g, {static_cast<std::int32_t>(f)});
        }
    }
    Cluster top;
    top.group = reached;
    p.clusters.push_back(top);
    p.runs.push_back(reached);
    std::vector<std::int32_t> clusters;
    Open(p, 0, clusters);
    // a seeded order; each that can be in the program with those taken, all for 1, else about
    // half, at most 6
    std::uint64_t s = static_cast<std::uint64_t>(pick) * 0x9E3779B97F4A7C15ull + 7;
    auto next = [&]() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s >> 11;
    };
    for (std::size_t i = clusters.size(); i > 1; i--) {
        std::swap(clusters[i - 1], clusters[next() % i]);
    }
    std::vector<std::int32_t> taken;
    for (std::int32_t c : clusters) {
        bool fits = taken.size() < 6 && (pick == 1 || next() % 2 == 0);
        for (std::size_t t = 0; t < taken.size() && fits; t++) {
            fits = Apart(p, c, taken[t]);
        }
        if (fits) {
            taken.push_back(c);
        }
    }
    return PlanOf(p, taken);
}

void PrintOverlayPlan(const Program& program, const OverlayPlan& plan) {
    for (std::size_t g = 0; g < plan.groups.size(); g++) {
        const OverlayGroupPlan& group = plan.groups[g];
        std::string roots;
        std::string members;
        for (std::int32_t r : group.roots) {
            roots += " " + program.functions[static_cast<std::size_t>(r)].name;
        }
        for (std::int32_t m : group.members) {
            members += " " + program.functions[static_cast<std::size_t>(m)].name;
        }
        std::fprintf(stderr, "overlay group %zu (%u expressions), roots:%s\n  functions:%s\n", g,
                     group.weight, roots.c_str(), members.c_str());
    }
    for (std::int32_t f : plan.resident_marks) {
        std::fprintf(stderr, "marked :overlay but resident: %s\n",
                     program.functions[static_cast<std::size_t>(f)].name.c_str());
    }
}

std::vector<std::string> OverlayRootNames(const Program& program, const OverlayPlan& plan) {
    std::vector<std::string> names;
    for (const OverlayGroupPlan& group : plan.groups) {
        for (std::int32_t r : group.roots) {
            names.push_back(program.functions[static_cast<std::size_t>(r)].name);
        }
    }
    return names;
}

} // namespace tisple
