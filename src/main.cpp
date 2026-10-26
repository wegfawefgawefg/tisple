#include "emu/sim_command.hpp"
#include "reader/commands.hpp"

#include <cstdio>
#include <cstring>

namespace {

void PrintUsage() {
    std::printf("Tisple ][ 0.1\n"
                "usage: tisple <command> [args]\n"
                "  help    show this\n"
                "  sim     run a 6502 binary in the simulator\n"
                "  fmt     format .tsp files (-w rewrites, --check fails if unformatted)\n"
                "  show    print a .tsp file with line numbers and colors\n"
                "  gen-editors <dir>  regenerate the editor grammars\n");
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

    std::fprintf(stderr, "tisple: unknown command '%s'\n", argv[1]);
    PrintUsage();
    return 1;
}
