// Room for a program that does not fit: squeeze.hpp.

#include "backend/squeeze.hpp"

#include "backend/card_bank.hpp"

#include <algorithm>
#include <cstdio>

namespace tisple {

namespace {

std::uint32_t Bytes(const Assembly& as, const std::vector<AsmItem>& items) {
    std::uint32_t bytes = 0;
    for (const AsmItem& item : items) {
        bytes += static_cast<std::uint32_t>(ItemBytes(as, item));
    }
    return bytes;
}

std::string Count(std::size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// the part of a name after its last dot: the function's own name
std::string Own(const std::string& name) {
    std::size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(dot + 1);
}

} // namespace

Squeezed NoteSqueezed(const Backend& be, const std::vector<bool>& kept, const SizeChoices& choices,
                      const std::vector<AsmItem>& runtime) {
    Squeezed out;
    out.squeeze = be.budget.squeeze;
    out.kept = kept;
    for (bool loop : choices.keep_loops) {
        out.loops_kept += loop ? 1 : 0;
    }
    for (int r : {static_cast<int>(Runtime::Mul8Q), static_cast<int>(Runtime::Mul16Q),
                  static_cast<int>(Runtime::Mul8x8Q)}) {
        out.quarter = out.quarter || be.rt_used[r];
    }
    for (std::size_t f = 0; f < be.code.size(); f++) {
        out.bytes += kept[f] ? Bytes(be.as, be.code[f]) : 0;
    }
    out.bytes += Bytes(be.as, runtime) + (out.quarter ? 1024 : 0);
    constexpr std::uint64_t kMax = std::uint64_t(1) << 56;
    for (std::size_t f = 0; f < be.code.size(); f++) {
        std::uint64_t runs = be.fn_runs[f];
        std::uint64_t cost = be.fn_cost[f];
        if (kept[f] && runs > 0 && cost > 0) {
            out.cycles = cost > kMax / runs ? kMax : std::min(kMax, out.cycles + cost * runs);
        }
    }
    return out;
}

Diagnostic Bank1Note(const Program& program, const Squeezed& banked, const Squeezed* other,
                     bool chose_bank1) {
    std::uint64_t stubs = banked.crossings * kStubCycles;
    std::string names;
    for (std::size_t i = 0; i < banked.bank1.size() && i < 8; i++) {
        names += (i == 0 ? "" : ", ") +
                 program.functions[static_cast<std::size_t>(banked.bank1[i])].name;
    }
    if (banked.bank1.size() > 8) {
        names += ", and " + std::to_string(banked.bank1.size() - 8) + " more";
    }
    std::string text;
    std::string bank = Count(banked.bank1.size(), "function", "functions") + " (" +
                       std::to_string(banked.bank1_bytes) + " bytes: " + names +
                       ") from the language card's bank 1, called through stubs";
    std::string cost = "about " + std::to_string(stubs) + " cycles a run in bank switches (" +
                       std::to_string(banked.crossings) +
                       " crossings, loops weighed as for "
                       "code choices)";
    if (chose_bank1) {
        text = "the program does not fit main memory and the language card with the usual size "
               "budget, so it runs " +
               bank + ": " + cost + ".";
        if (other != nullptr) {
            text += " Without bank 1 it would fit with squeeze " + std::to_string(other->squeeze) +
                    " and run about " +
                    std::to_string(other->cycles - std::min(other->cycles, banked.cycles)) +
                    " cycles a run slower, the switches counted.";
        } else {
            text += " Without bank 1, a tighter size budget would not fit or would run slower.";
        }
    } else {
        text = "code in the language card's bank 1 would fit the program too: " + bank + ", " +
               cost + "; the tighter size budget costs less, so it is not used.";
    }
    Diagnostic note;
    note.message = text;
    return note;
}

Diagnostic SqueezeNote(const Program& program, const Squeezed& usual, const Squeezed& squeezed) {
    std::string text = "the program does not fit its memory with the usual size budget, so it is "
                       "built with a tighter one (squeeze " +
                       std::to_string(squeezed.squeeze) + " of " + std::to_string(kMaxSqueeze) +
                       "): the functions and runtime routines take " +
                       std::to_string(squeezed.bytes) + " bytes instead of " +
                       std::to_string(usual.bytes) + ".";
    // functions copied at every call before, calls now
    std::vector<std::string> calls;
    for (std::size_t f = 0; f < squeezed.kept.size() && f < usual.kept.size(); f++) {
        if (squeezed.kept[f] && !usual.kept[f]) {
            calls.push_back(program.functions[f].name);
        }
    }
    if (!calls.empty()) {
        text += " Called instead of copied: " + Count(calls.size(), "function", "functions") + " (";
        for (std::size_t i = 0; i < calls.size() && i < 8; i++) {
            text += (i == 0 ? "" : ", ") + calls[i];
        }
        text += calls.size() > 8 ? ", and " + std::to_string(calls.size() - 8) + " more)." : ").";
    }
    if (squeezed.loops_kept > usual.loops_kept) {
        text += " " +
                Count(static_cast<std::size_t>(squeezed.loops_kept - usual.loops_kept), "more loop",
                      "more loops") +
                " left rolled.";
    }
    if (usual.quarter && !squeezed.quarter) {
        text += " Multiplies by shift and add, without the quarter-square routines and their 1 KB "
                "of tables.";
    }
    if (squeezed.squeeze >= kSqueezeRolled) {
        text += " No unrolling or unswitching by the mid-level passes.";
    }
    text += " TISPLE_SIZE_NOTES=1 lists what the budget weighs.";
    Diagnostic note;
    note.message = text;
    return note;
}

Diagnostic OverlayNote(const Program& program, const Compiled& compiled) {
    const OverlayBuilt& built = compiled.overlays;
    std::uint32_t total = 0;
    std::string groups;
    for (std::size_t g = 0; g < built.groups.size(); g++) {
        const OverlayImage& image = built.groups[g];
        total += image.bytes;
        std::string roots;
        for (std::size_t i = 0; i < image.roots.size(); i++) {
            roots += (i == 0 ? "" : ", ") +
                     program.functions[static_cast<std::size_t>(image.roots[i])].name;
        }
        std::uint64_t frames10 = (image.load_cycles * 10 + 8515) / 17030;
        groups += (g == 0 ? "" : "; ") + roots + (image.on_disk ? " from the second disk" : "") +
                  " (" + Count(image.functions.size(), "function", "functions") + ", " +
                  std::to_string(image.bytes) + " bytes, a load about " +
                  std::to_string(image.load_cycles) + " cycles, " + std::to_string(frames10 / 10) +
                  "." + std::to_string(frames10 % 10) + " frames)";
    }
    std::uint32_t used = built.region_size + built.code_bytes;
    std::uint32_t freed = total > used ? total - used : 0;
    char where[16];
    std::snprintf(where, sizeof(where), "$%04X", built.region);
    std::string text =
        "the program does not fit main memory and the language card as it is, so " +
        Count(built.groups.size(), "group", "groups") + " of code take turns in a " +
        std::to_string(built.region_size) + "-byte overlay region at " + where +
        (built.disk_tables.empty()
             ? ", each copied in from aux memory when one of its roots is called and "
               "another is in: "
             : ", each copied in from aux memory (or read from the second disk) "
               "when one of its roots is called and another is in: ") +
        groups + ". The stubs and the copy take " + std::to_string(built.code_bytes) + " bytes; " +
        std::to_string(freed) + " bytes of main memory and the card freed.";
    Diagnostic note;
    note.message = text;
    return note;
}

std::vector<Diagnostic> ResidentMarkNotes(const Program& program, const OverlayPlan& plan) {
    std::vector<Diagnostic> notes;
    for (std::int32_t f : plan.resident_marks) {
        const Function& fn = program.functions[static_cast<std::size_t>(f)];
        Diagnostic note;
        note.loc = fn.loc;
        note.message = fn.name +
                       " is marked :overlay but stays resident: main, the interrupt handler, a "
                       "function pointer or code with an aux switch on runs it, or nothing does";
        notes.push_back(note);
    }
    return notes;
}

bool CheckNoInline(const Program& program, const std::vector<std::string>& names,
                   std::vector<Diagnostic>& errors) {
    bool ok = true;
    for (const std::string& name : names) {
        if (FindFunction(program, name) >= 0) {
            continue;
        }
        // the one meant: the same module and function after the last slash, or the same own name
        std::string meant;
        std::string example;
        for (const Function& fn : program.functions) {
            std::size_t slash = fn.name.rfind('/');
            bool tail = slash != std::string::npos && fn.name.substr(slash + 1) == name;
            if (meant.empty() && (tail || (Own(fn.name) == Own(name) && !fn.dispatcher))) {
                meant = fn.name;
            }
            if (example.empty() && slash != std::string::npos) {
                example = fn.name;
            }
        }
        if (example.empty() && !program.functions.empty()) {
            example = program.functions[0].name;
        }
        std::string text = "--no-inline " + name + ": no function has that name; ";
        if (!meant.empty()) {
            text += "did you mean " + meant + "?";
        } else {
            text +=
                "the names it takes are a module's path, a dot and the function, like " + example;
        }
        AddError(errors, Loc(), text);
        ok = false;
    }
    return ok;
}

} // namespace tisple
