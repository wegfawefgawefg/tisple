#include "check.hpp"
#include "machine/taps.hpp"
#include "test_machine.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

// --taps: a game's phone player config read for the window's clicks

namespace tisple_tests {

void RunMachineTapsTests(TestResults& results) {
    // a name of its own: fuzz runs start several test processes at once
    std::string path = "/tmp/tisple_test_taps_" + std::to_string(getpid()) + ".json";
    {
        std::ofstream out(path);
        out << "{\n \"title\": \"T\",\n \"buttons\": [{\"label\": \"1\", \"key\": \"1\"}],\n"
               " \"taps\": [\n  {\"x\": 20, \"y\": 148, \"w\": 36, \"h\": 44, \"key\": \"1\"},\n"
               "  {\"x\": 124, \"y\": 0, \"w\": 66, \"h\": 14, \"key\": \"b\"},\n"
               "  {\"x\": 200, \"y\": 0, \"w\": 10, \"h\": 10, \"key\": \"RETURN\"},\n"
               "  {\"x\": 0, \"y\": 100, \"w\": 10, \"h\": 10, \"key\": 27}\n ]\n}\n";
    }
    std::vector<tisple::Tap> taps;
    std::string error;
    CHECK(results, tisple::LoadTaps(path, taps, error));
    CHECK(results, taps.size() == 4);
    if (taps.size() == 4) {
        CHECK(results, taps[0].x == 20 && taps[0].y == 148 && taps[0].w == 36 && taps[0].h == 44);
        CHECK(results, taps[0].code == '1');
        CHECK(results, taps[1].code == 'B'); // letters as the keyboard sends them
        CHECK(results, taps[2].code == 13);
        CHECK(results, taps[3].code == 27);
    }
    const tisple::Tap* hit = tisple::TapAt(taps, 30, 160);
    CHECK(results, hit != nullptr && hit->code == '1');
    CHECK(results, tisple::TapAt(taps, 60, 160) == nullptr); // just past the box
    std::remove(path.c_str());

    std::vector<tisple::Tap> none;
    CHECK(results, !tisple::LoadTaps("/tmp/tisple_test_no_such_taps.json", none, error));
}

} // namespace tisple_tests
