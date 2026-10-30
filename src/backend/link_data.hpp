#pragma once

// What data the binary holds (link_data.cpp), for LinkProgram: the consts in it, and the
// startup's stores of :address lets outside it.
//
// A const (a string's table
// among them) is in the binary when kept code names it, it has an :address, or an address in
// data the binary holds points into it. One the program as written may still take the address
// of (backend/analysis.hpp, FindAddressedConsts) but the binary does not need is the
// interpreter's only: it gets an address inside the code, which the interpreter never reads or
// writes, and no bytes of the binary. The rest are left out: the interpreter reads them from
// their initial bytes.

#include "backend/link.hpp"

namespace tisple {

struct TableUse {
    std::vector<bool> dropped;     // by var: not in the binary (the interpreter's only too)
    std::vector<bool> interp_only; // by var: not in the binary, but the interpreter's memory
};

TableUse FindTableUse(const Backend& be, const LinkInput& in);

// addresses inside runs (the code's stretches) for every interp_only var, as shaped in
// be.shapes; false when they do not all fit, and then none is placed
bool PlaceInterpOnly(const Backend& be, const std::vector<bool>& interp_only,
                     const std::vector<AddressRange>& runs, std::vector<VarPlace>& places);

// explicit-address globals with initial values outside [start, end): the startup writes them.
// Aux ones are in the aux segment instead
bool StartupStores(const Backend& be, std::uint32_t start, std::uint32_t end,
                   std::vector<std::pair<std::uint16_t, std::uint8_t>>& stores,
                   std::vector<Diagnostic>& errors);

} // namespace tisple
