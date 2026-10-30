#include "machine/taps.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

// The config is the phone player's JSON. Only its "taps" array is read, and only its shape:
// objects of "x", "y", "w", "h" (numbers) and "key" (a string, or a number for a key code).
// A small scanner for that shape is plainer here than a JSON library.

namespace tisple {

namespace {

// a key name as the phone player takes it: RETURN, ESC, ... or one character
bool KeyCode(const std::string& name, uint8_t& code) {
    struct Named {
        const char* name;
        uint8_t code;
    };
    const Named named[] = {{"RETURN", 13}, {"ESC", 27},   {"SPACE", 32},
                           {"LEFT", 8},    {"RIGHT", 21}, {"UP", 11},
                           {"DOWN", 10},   {"TAB", 9},    {"DELETE", 127}};
    for (const Named& n : named) {
        if (name == n.name) {
            code = n.code;
            return true;
        }
    }
    if (name.size() == 1) {
        code = static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(name[0])));
        return true;
    }
    return false;
}

// the value after "field": in one object's text, as a number or a string
bool FieldValue(const std::string& object, const char* field, std::string& value) {
    std::string quoted = std::string("\"") + field + "\"";
    size_t at = object.find(quoted);
    if (at == std::string::npos) {
        return false;
    }
    at = object.find(':', at + quoted.size());
    if (at == std::string::npos) {
        return false;
    }
    at++;
    while (at < object.size() && std::isspace(static_cast<unsigned char>(object[at]))) {
        at++;
    }
    if (at < object.size() && object[at] == '"') {
        size_t end = object.find('"', at + 1);
        if (end == std::string::npos) {
            return false;
        }
        value = object.substr(at + 1, end - at - 1);
        return true;
    }
    size_t end = at;
    while (end < object.size() &&
           (std::isdigit(static_cast<unsigned char>(object[end])) || object[end] == '-')) {
        end++;
    }
    value = object.substr(at, end - at);
    return !value.empty();
}

bool ReadTap(const std::string& object, Tap& tap) {
    std::string x, y, w, h, key;
    if (!FieldValue(object, "x", x) || !FieldValue(object, "y", y) || !FieldValue(object, "w", w) ||
        !FieldValue(object, "h", h) || !FieldValue(object, "key", key)) {
        return false;
    }
    tap.x = std::atoi(x.c_str());
    tap.y = std::atoi(y.c_str());
    tap.w = std::atoi(w.c_str());
    tap.h = std::atoi(h.c_str());
    if (!key.empty() && std::isdigit(static_cast<unsigned char>(key[0])) && key.size() > 1) {
        tap.code = static_cast<uint8_t>(std::atoi(key.c_str()));
        return true;
    }
    return KeyCode(key, tap.code);
}

} // namespace

bool LoadTaps(const std::string& path, std::vector<Tap>& taps, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot read '" + path + "'";
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    std::string json = text.str();
    size_t at = json.find("\"taps\"");
    if (at == std::string::npos) {
        error = "'" + path + "' has no \"taps\"";
        return false;
    }
    size_t open = json.find('[', at);
    size_t close = json.find(']', at);
    if (open == std::string::npos || close == std::string::npos || close < open) {
        error = "'" + path + "': \"taps\" is not an array";
        return false;
    }
    size_t pos = open;
    while (true) {
        size_t start = json.find('{', pos);
        if (start == std::string::npos || start > close) {
            break;
        }
        size_t end = json.find('}', start);
        if (end == std::string::npos || end > close) {
            break;
        }
        Tap tap;
        if (!ReadTap(json.substr(start, end - start + 1), tap)) {
            error = "'" + path + "': a tap needs x, y, w, h and key";
            return false;
        }
        taps.push_back(tap);
        pos = end + 1;
    }
    return true;
}

const Tap* TapAt(const std::vector<Tap>& taps, int x, int y) {
    for (const Tap& t : taps) {
        if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h) {
            return &t;
        }
    }
    return nullptr;
}

} // namespace tisple
