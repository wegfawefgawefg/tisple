#pragma once

#include "check.hpp"
#include "source_file.hpp"

#include <string>
#include <vector>

namespace tisple_tests {

void RunReaderTests(TestResults& results);
void RunFormatTests(TestResults& results);
void RunShowTests(TestResults& results);

// every .tsp under examples/, programs/ and lib/, sorted
std::vector<std::string> ExampleFiles();
std::string SourcePath(const std::string& relative);
tisple::SourceFile Text(const std::string& text);

} // namespace tisple_tests
