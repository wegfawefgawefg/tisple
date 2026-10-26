#pragma once

// the $C000-$FFFF side of the bus: soft switches, language card, rom and slot space

#include "machine/apple2.hpp"

namespace tisple {

// aims the cpu's io hook at a; call again after the struct is copied
void InstallBus(Apple2& a);

// the byte the video scanner fetched at this cycle
uint8_t FloatingBus(const Apple2& a, uint64_t cycle);

// whether code can run at addr: false for i/o and for rom space with nothing behind it
bool CanExecute(const Apple2& a, uint16_t addr);

// language card and soft-switch reset states
void ResetSoftSwitches(Apple2& a);

} // namespace tisple
