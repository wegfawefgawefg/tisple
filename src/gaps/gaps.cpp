#include "gaps/gaps.hpp"

#include "check/check.hpp"

#include <memory>
#include <thread>
#include <vector>

namespace tisple {

namespace {

std::string MachineName(const Compiled& compiled, const ProfileRunOptions& run) {
    if (compiled.main < 0) {
        return "the bare simulator, every function in turn";
    }
    return run.bare ? "the bare simulator" : "the Apple IIe (no rom, no keys)";
}

void FuzzOne(ProgramTextFn generate, std::uint64_t seed, const GapsOptions& options,
             WindowCache& cache, BulkGaps& bulk) {
    bulk.programs++;
    LoadOptions load;
    load.lib_dir = "lib0"; // nothing to import: fuzz programs stand alone
    load.files.push_back(VirtualFile{"t.tsp", generate(seed)});
    Program program;
    Compiled compiled;
    std::vector<Diagnostic> errors;
    if (!BuildProgram("t.tsp", load, program, errors) ||
        !CompileProgram(program, BuildOptions(), compiled, errors)) {
        bulk.failed++;
        return;
    }
    GapsOptions o = options;
    o.run.bare = true;
    ProgramGaps gaps;
    std::string error;
    if (!FindProgramGaps(program, compiled, o, cache, gaps, error)) {
        bulk.failed++;
        return;
    }
    AddProgram(bulk, gaps, seed);
}

} // namespace

bool FindProgramGaps(const Program& program, const Compiled& compiled, const GapsOptions& options,
                     WindowCache& cache, ProgramGaps& out, std::string& error) {
    std::unique_ptr<Profiler> profiler = std::make_unique<Profiler>();
    if (!ProfileCompiled(program, compiled, options.run, *profiler, out.run, error)) {
        return false;
    }
    std::unique_ptr<SiteMap> map = std::make_unique<SiteMap>();
    BuildSiteMap(compiled, *map);
    out.machine = MachineName(compiled, options.run);
    out.instructions = profiler->instructions;
    out.cycles = profiler->cycles;
    out.find = options.find;
    out.only = options.only;
    for (const Finding& f : FindGaps(compiled, *map, *profiler, options.find)) {
        if (f.function.find(options.only) != std::string::npos) {
            out.findings.push_back(f);
        }
    }
    out.superopt = options.superopt;
    if (options.superopt) {
        HotWindowOptions windows = options.windows;
        windows.only = options.only;
        out.windows =
            SuperoptHotWindows(compiled, *map, *profiler, windows, cache, out.window_stats);
    }
    return true;
}

BulkGaps FuzzGaps(ProgramTextFn generate, std::uint64_t first, std::uint64_t count, int threads,
                  const GapsOptions& options) {
    threads = threads < 1 ? 1 : threads;
    std::vector<BulkGaps> parts(static_cast<std::size_t>(threads));
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; t++) {
        workers.emplace_back([&, t]() {
            WindowCache cache;
            for (std::uint64_t i = static_cast<std::uint64_t>(t); i < count;
                 i += static_cast<std::uint64_t>(threads)) {
                FuzzOne(generate, first + i, options, cache, parts[static_cast<std::size_t>(t)]);
            }
        });
    }
    for (std::thread& w : workers) {
        w.join();
    }
    BulkGaps total;
    for (const BulkGaps& part : parts) {
        MergeBulk(total, part);
    }
    return total;
}

} // namespace tisple
