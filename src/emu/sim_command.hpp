#pragma once

namespace tisple {

// `tisple sim <file.bin> [--load ADDR] [--start ADDR] [--stop ADDR] [--max-cycles N] [--trace]`
// args are the words after "sim"; returns the process exit code
int RunSimCommand(int argc, char** argv);

} // namespace tisple
