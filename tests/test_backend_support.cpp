#include "test_backend.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

Compiled Finish(Built built) {
    Compiled c;
    c.built = std::move(built);
    if (!c.built.ok) {
        c.errors = ErrorText(c.built);
        return c;
    }
    std::vector<tisple::Diagnostic> errors;
    c.ok = tisple::CompileProgram(c.built.program, tisple::BuildOptions(), c.code, errors);
    if (!c.ok) {
        c.errors = tisple::FormatDiagnostics(c.built.program.sources, errors);
    }
    return c;
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "$%04X", value);
    return text;
}

} // namespace

Compiled CompileText(const std::string& text, const std::vector<tisple::VirtualFile>& extra) {
    return Finish(BuildText(text, extra));
}

Compiled CompileFile(const std::string& path) {
    return Finish(BuildFile(path));
}

bool StartDiff(Diff& d, const Compiled& c) {
    d.c = &c;
    d.message.clear();
    if (!c.ok) {
        d.message = "did not compile: " + c.errors;
        return false;
    }
    tisple::InitInterp(d.in, c.built.program, c.code.layout);
    d.interp_io.clear();
    d.in.io.read = [&d](std::uint16_t address) {
        std::uint8_t value = tisple::IoReadValue(address, d.interp_io.size());
        d.interp_io.push_back(tisple::IoAccess{address, value, false});
        return value;
    };
    d.in.io.write = [&d](std::uint16_t address, std::uint8_t value) {
        d.interp_io.push_back(tisple::IoAccess{address, value, true});
    };
    std::string error;
    if (!tisple::StartCompiled(d.sim, c.code, error)) {
        d.message = error;
        return false;
    }
    return true;
}

bool SameGlobals(const Diff& d, std::string& message) {
    const tisple::Program& program = d.c->built.program;
    const tisple::Layout& layout = d.c->code.layout;
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        if (program.vars[v].function >= 0) {
            continue;
        }
        const tisple::VarPlace& place = layout.vars[v];
        for (std::uint32_t i = 0; i < place.size; i++) {
            std::uint16_t at = static_cast<std::uint16_t>(place.address + i);
            std::uint8_t want = d.in.memory[at];
            std::uint8_t got = d.sim.machine->mem[at];
            if (want != got) {
                message = program.vars[v].name + " byte " + std::to_string(i) + " at " + Hex(at) +
                          ": interpreter " + std::to_string(want) + ", 6502 " + std::to_string(got);
                return false;
            }
        }
    }
    return true;
}

bool DiffCall(Diff& d, const std::string& function, const std::vector<std::uint16_t>& args) {
    const tisple::Program& program = d.c->built.program;
    std::int32_t id = tisple::FindFunction(program, function);
    if (id < 0) {
        d.message = "no function " + function;
        return false;
    }
    tisple::InterpResult want = tisple::CallFunction(d.in, id, args);
    if (want.kind != tisple::StopKind::Finished) {
        d.message = "interpreter stopped: " + want.message;
        return false;
    }
    tisple::SimCall got = tisple::CallCompiled(d.sim, program, d.c->code, id, args, d.max_cycles);
    d.last_cycles = got.cycles;
    if (!got.returned) {
        d.message = std::string("6502 stopped: ") + tisple::StopReasonName(got.reason);
        return false;
    }
    const tisple::Function& fn = program.functions[static_cast<std::size_t>(id)];
    std::uint32_t size = tisple::TypeOf(program, fn.ret).size;
    if (fn.ret != tisple::kVoidType && size > 0) {
        std::uint16_t value = got.a;
        if (size == 2) {
            value = static_cast<std::uint16_t>(value | (got.x << 8));
        }
        if (value != want.value) {
            d.message = function + " returned " + std::to_string(value) + " on the 6502, " +
                        std::to_string(want.value) + " in the interpreter";
            return false;
        }
    }
    if (!SameGlobals(d, d.message)) {
        return false;
    }
    if (d.interp_io.size() != d.sim.io.size()) {
        d.message = "I/O: " + std::to_string(d.interp_io.size()) +
                    " accesses in the interpreter, " + std::to_string(d.sim.io.size()) +
                    " on the 6502";
        return false;
    }
    for (std::size_t i = 0; i < d.interp_io.size(); i++) {
        const tisple::IoAccess& a = d.interp_io[i];
        const tisple::IoAccess& b = d.sim.io[i];
        if (a.address != b.address || a.write != b.write || a.value != b.value) {
            d.message = "I/O access " + std::to_string(i) + ": interpreter " + Hex(a.address) +
                        (a.write ? " write " : " read ") + std::to_string(a.value) + ", 6502 " +
                        Hex(b.address) + (b.write ? " write " : " read ") + std::to_string(b.value);
            return false;
        }
    }
    return true;
}

void CheckDiff(TestResults& results, Diff& d, const std::string& function,
               const std::vector<std::uint16_t>& args) {
    bool ok = DiffCall(d, function, args);
    if (!ok) {
        std::string arg_text;
        for (std::uint16_t a : args) {
            arg_text += " " + std::to_string(a);
        }
        std::fprintf(stderr, "differs: (%s%s): %s\n", function.c_str(), arg_text.c_str(),
                     d.message.c_str());
    }
    CHECK(results, ok);
}

int CountMn(const Compiled& c, const std::string& function, tisple::Mn mn) {
    int count = 0;
    for (const tisple::FunctionSpan& span : c.code.spans) {
        if (span.name != function) {
            continue;
        }
        for (std::size_t i = span.first; i < span.end; i++) {
            const tisple::AsmItem& item = c.code.as.items[i];
            count += item.kind == tisple::ItemKind::Ins && item.mn == mn ? 1 : 0;
        }
    }
    return count;
}

void CheckProgram(TestResults& results, const std::string& text, const std::string& function,
                  const std::vector<std::uint16_t>& args) {
    Compiled c = CompileText(text);
    Diff d;
    bool ok = StartDiff(d, c) && DiffCall(d, function, args);
    if (!ok) {
        std::fprintf(stderr, "program:\n%s\n  %s\n", text.c_str(), d.message.c_str());
    }
    CHECK(results, ok);
}

} // namespace tisple_tests
