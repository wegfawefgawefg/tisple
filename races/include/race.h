/* Common header for the race's C kernels (races/README.md).
 * buffers.h is written by races/race.py for each build: the kernel's buffers at their
 * fixed addresses, either as constant pointers or as extern arrays the linker places. */
#ifndef RACE_H
#define RACE_H

#include <stdint.h>

#ifdef RACE_OSCAR64
/* keep code and data below the race's buffers ($2000-$5FFF), bss and stacks above */
#pragma region(main, 0x0880, 0x2000, , , {code, data})
#pragma region(himem, 0x6000, 0xa000, , , {bss, heap, stack})
#endif

#include "buffers.h"

#endif
