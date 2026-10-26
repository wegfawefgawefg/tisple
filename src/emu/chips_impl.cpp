// the vendored chips headers are single-file libraries; this is the one place
// that compiles their implementation (warnings are relaxed for this file in CMake)

#define CHIPS_IMPL
#define CHIPS_UTIL_IMPL

#include "m6502.h"
#include "m6502dasm.h"
