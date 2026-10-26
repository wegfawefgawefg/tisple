#include <cstdio>
#include <cstring>

namespace {

void PrintUsage() {
    std::printf("Tisple ][ 0.1\n"
                "usage: tisple <command> [args]\n"
                "  help    show this\n");
}

} // namespace

int main(int argc, char** argv) {
    // pick the command
    if (argc < 2 || std::strcmp(argv[1], "help") == 0) {
        PrintUsage();
        return argc < 2 ? 1 : 0;
    }

    std::fprintf(stderr, "tisple: unknown command '%s'\n", argv[1]);
    PrintUsage();
    return 1;
}
