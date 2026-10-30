// (pack bytes) in macro code: a list of byte values, ZX0-packed (pack/zx0.hpp). lib/tables'
// packed and packed_bytes use it; lib/unpack puts the bytes back on the Apple II.

#include "macro/eval_internal.hpp"
#include "pack/zx0.hpp"

#include <cstdint>
#include <string>

namespace tisple {

Value PackLibrary(MacroRun& run, Frame& frame, std::vector<Value>& args, const Node& at) {
    if (args.size() != 1 || args[0].kind != ValueKind::List) {
        RunFail(run, frame, at,
                args.size() == 1 ? "pack needs a list of bytes, got " + DescribeValue(args[0])
                                 : "write (pack bytes)");
        return Value();
    }
    const std::vector<Value>& items = args[0].items;
    if (items.empty() || items.size() > 65535) {
        RunFail(run, frame, at, "pack takes 1 to 65535 bytes, got " + std::to_string(items.size()));
        return Value();
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); i++) {
        const Value& v = items[i];
        if (v.kind != ValueKind::Int || v.integer < 0 || v.integer > 255) {
            RunFail(run, frame, at,
                    "pack takes bytes, 0 to 255; item " + std::to_string(i) + " is " +
                        DescribeValue(v));
            return Value();
        }
        bytes.push_back(static_cast<std::uint8_t>(v.integer));
    }
    // the parse looks at each byte a few hundred times; count it as one step a byte
    if (!Step(run, bytes.size())) {
        return Value();
    }
    std::vector<std::uint8_t> packed = PackZx0(bytes);
    std::vector<Value> out;
    out.reserve(packed.size());
    for (std::uint8_t b : packed) {
        out.push_back(IntValue(b, frame.module));
    }
    return ListValue(std::move(out), frame.module);
}

} // namespace tisple
