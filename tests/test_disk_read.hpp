#pragma once

// what the lib/disk tests share (test_disk_read.cpp): a program with tables :on disk built
// with the real lib/ onto a boot disk, and booted on the built-in IIe

#include "check.hpp"
#include "disk/boot_loader.hpp"
#include "machine/apple2.hpp"
#include "test_backend.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tisple_tests {

struct DiskProgram {
    tisple::Program program;
    tisple::Compiled code;
    std::unique_ptr<tisple::DiskSectors> disk;
    tisple::BootDiskInfo info;
    bool ok = false;
};

// as tisple build --disk lays it out, from origin
tisple::BuildOptions DiskReadLayout(uint16_t origin);

void BuildDiskProgram(TestResults& results, const std::string& text,
                      const tisple::BuildOptions& options, DiskProgram& out);

// a machine with the disk in drive 1 (our boot rom), booting
std::unique_ptr<tisple::Apple2> BootDiskProgram(const DiskProgram& p);

uint16_t DiskProgramVar(const DiskProgram& p, const char* name);
uint16_t DiskProgramFunction(const DiskProgram& p, const char* name);
// a function's value as a pointer: its entry, which a call through a pointer reaches first
uint16_t DiskProgramPointer(const DiskProgram& p, const char* name);
// a table :on disk's bytes, by its qualified name
const tisple::DiskTable* FindDiskTable(const DiskProgram& p, const char* name);

// "(make (array u8 n) b0 b1 ...)"
std::string MakeBytes(const std::vector<uint8_t>& bytes);
// bytes that differ from page to page
std::vector<uint8_t> DiskPattern(size_t size, uint32_t salt);

constexpr uint64_t kDiskReadBudget = 400ull * 1000 * 1000;

// runs until the program stops (returns) or budget cycles pass; the cycles it took
uint64_t RunDiskProgram(tisple::Apple2& a, uint64_t budget);

void RunDiskReadTests(TestResults& results);
void RunDiskReadFailTests(TestResults& results);
void RunDiskReadTimingTests(TestResults& results);
void RunDiskStreamTests(TestResults& results);

} // namespace tisple_tests
