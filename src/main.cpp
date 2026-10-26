#include "backend/build_command.hpp"
#include "check/commands.hpp"
#include "emu/sim_command.hpp"
#include "fuzz_gen.hpp"
#include "gaps/gaps.hpp"
#include "ide/index.hpp"
#include "interp/interp_command.hpp"
#include "machine/machine_command.hpp"
#include "reader/commands.hpp"

#include <cstdio>
#include <cstring>

namespace {

#ifdef TISPLE_HAS_SDL
const tisple::MachineWindowFn kMachineWindow = tisple::RunMachineWindow;
#else
const tisple::MachineWindowFn kMachineWindow = nullptr;
#endif

void PrintUsage() {
    std::printf("Tisple ][ 0.1\n"
                "usage: tisple <command> [args]\n"
                "  help    show this\n"
                "  sim     run a 6502 binary in the simulator\n"
                "  fmt     format .tsp files (-w rewrites, --check fails if unformatted)\n"
                "  show    print a .tsp file with line numbers and colors\n"
                "  gen-editors <dir>  regenerate the editor grammars\n");
    std::printf("  check   check a program and its imports\n"
                "  expand  print a program after macro expansion\n"
                "  interp  run main in the reference interpreter (--max-steps N)\n"
                "  index   names, types, definitions and errors as JSON, for editors (--stdin)\n");
    std::printf("  build   compile to a 6502 binary (-o out.bin, --origin A, --asm)\n"
                "  run     build and run on the apple iie (window; --frames N etc. headless;\n"
                "          --sim for the bare simulator)\n");
    std::printf("  machine run a raw binary in the built-in Apple IIe (--window, --screenshot)\n");
    std::printf("  gaps    rank where the compiled code is worse than it could be (--superopt,\n"
                "          --fuzz N for the fuzz generator's programs)\n");
}

} // namespace

int main(int argc, char** argv) {
    // pick the command
    if (argc < 2 || std::strcmp(argv[1], "help") == 0) {
        PrintUsage();
        return argc < 2 ? 1 : 0;
    }

    if (std::strcmp(argv[1], "sim") == 0) {
        return tisple::RunSimCommand(argc - 2, argv + 2);
    }
    if (std::strcmp(argv[1], "fmt") == 0) {
        return tisple::RunFmtCommand(argc, argv);
    }
    if (std::strcmp(argv[1], "show") == 0) {
        return tisple::RunShowCommand(argc, argv);
    }
    if (std::strcmp(argv[1], "gen-editors") == 0) {
        return tisple::RunGenEditorsCommand(argc, argv);
    }

    if (std::strcmp(argv[1], "check") == 0) {
        return tisple::RunCheckCommand(argc, argv);
    }
    if (std::strcmp(argv[1], "expand") == 0) {
        return tisple::RunExpandCommand(argc, argv);
    }
    if (std::strcmp(argv[1], "index") == 0) {
        return tisple::RunIndexCommand(argc, argv);
    }
    if (std::strcmp(argv[1], "interp") == 0) {
        return tisple::RunInterpCommand(argc, argv);
    }
    if (std::strcmp(argv[1], "machine") == 0) {
        return tisple::RunMachineCommand(argc - 2, argv + 2, kMachineWindow);
    }

    if (std::strcmp(argv[1], "build") == 0) {
        return tisple::RunBuildCommand(argc, argv);
    }
    if (std::strcmp(argv[1], "run") == 0) {
        return tisple::RunRunCommand(argc, argv, kMachineWindow);
    }
    if (std::strcmp(argv[1], "gaps") == 0) {
        return tisple::RunGapsCommand(argc, argv, tisple_tests::GenerateProgram);
    }

    std::fprintf(stderr, "tisple: unknown command '%s'\n", argv[1]);
    PrintUsage();
    return 1;
}
