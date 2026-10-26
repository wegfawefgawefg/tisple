// the scripted input of `tisple machine` and `tisple run`: --keys and --mouse

#include "machine/scripted_input.hpp"

#include "machine/mouse.hpp"

#include <cstdio>
#include <cstdlib>

namespace tisple {

bool ParseNumber(const char* text, uint64_t& out) {
    char* end = nullptr;
    if (text[0] == '$') {
        out = std::strtoull(text + 1, &end, 16);
        return text[1] != '\0' && *end == '\0';
    }
    out = std::strtoull(text, &end, 0);
    return text[0] != '\0' && *end == '\0';
}

bool ParseCycle(const std::string& text, uint64_t& out) {
    if (!text.empty() && text.back() == 'f') {
        uint64_t frames = 0;
        if (!ParseNumber(text.substr(0, text.size() - 1).c_str(), frames)) {
            return false;
        }
        out = frames * kCyclesPerFrame;
        return true;
    }
    return ParseNumber(text.c_str(), out);
}

namespace {

bool ParseKeyName(const std::string& name, uint8_t& code) {
    struct Named {
        const char* name;
        uint8_t code;
    };
    static const Named kNames[] = {
        {"RETURN", 0x0D}, {"RET", 0x0D},    {"ESC", 0x1B},  {"LEFT", 0x08},
        {"RIGHT", 0x15},  {"UP", 0x0B},     {"DOWN", 0x0A}, {"SPACE", 0x20},
        {"TAB", 0x09},    {"DELETE", 0x7F}, {"DEL", 0x7F},  {"COMMA", 0x2C},
    };
    for (const Named& n : kNames) {
        if (name == n.name) {
            code = n.code;
            return true;
        }
    }
    if (name.size() == 1) {
        char c = name[0];
        code = static_cast<uint8_t>(c >= 'a' && c <= 'z' ? c - 32 : c);
        return true;
    }
    uint64_t value = 0;
    if ((name[0] == '$' || name.rfind("0x", 0) == 0) && ParseNumber(name.c_str(), value) &&
        value < 0x80) {
        code = static_cast<uint8_t>(value);
        return true;
    }
    return false;
}

} // namespace

bool ScheduleKeys(Apple2& a, const char* spec, std::string& error) {
    std::string text = spec;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t comma = text.find(',', pos);
        std::string entry =
            text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? text.size() : comma + 1;
        size_t colon = entry.find(':');
        uint64_t cycle = 0;
        uint8_t code = 0;
        if (colon == std::string::npos || !ParseCycle(entry.substr(0, colon), cycle) ||
            !ParseKeyName(entry.substr(colon + 1), code)) {
            error = "bad key '" + entry + "', want cycle:key";
            return false;
        }
        if (!ScheduleKey(a, cycle, code)) {
            error = "too many scripted keys";
            return false;
        }
    }
    return true;
}

bool ScheduleMouseInput(Apple2& a, const char* spec, std::string& error) {
    std::string text = spec;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t semi = text.find(';', pos);
        std::string entry =
            text.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = semi == std::string::npos ? text.size() : semi + 1;
        uint64_t cycle = 0;
        long x = 0;
        long y = 0;
        int button = 0;
        int used = 0;
        size_t colon = entry.find(':');
        bool ok = colon != std::string::npos && ParseCycle(entry.substr(0, colon), cycle);
        std::string rest = ok ? entry.substr(colon + 1) : "";
        ok = ok && std::sscanf(rest.c_str(), "%ld,%ld,%d%n", &x, &y, &button, &used) == 3 &&
             static_cast<size_t>(used) == rest.size() && (button == 0 || button == 1) &&
             x >= -32768 && x <= 32767 && y >= -32768 && y <= 32767;
        if (!ok) {
            error = "bad mouse input '" + entry + "', want cycle:x,y,button";
            return false;
        }
        if (!ScheduleMouse(a, cycle, static_cast<int32_t>(x), static_cast<int32_t>(y),
                           button == 1)) {
            error = "too much scripted mouse input";
            return false;
        }
    }
    return true;
}

} // namespace tisple
