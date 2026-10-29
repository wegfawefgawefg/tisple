/* Header of the random-program race's C programs (races/random.py), which tisple race-gen
 * writes from the fuzz generator's Tisple programs (src/race/race_c.hpp).
 *
 * RACE_NOINLINE keeps t.init and t.checksum functions of their own: the race's clock runs
 * from init's return to the checksum's entry, so their work is not counted. RACE_FIXED is
 * an object at a fixed address (chk, the checksum, at $BF00). RACE_HOST builds the program
 * for the host machine, to check the C itself (with sanitizers): the fixed object becomes a
 * variable, printed at exit. */
#ifndef RANDOM_H
#define RANDOM_H

#include <stdint.h>

/* functions are static, except for cc65, whose label file (the race's map of where each
 * function is, for counting bytes) only lists the others; cc65 does nothing with static */
#ifdef RACE_CC65
#define RACE_STATIC
#else
#define RACE_STATIC static
#endif

#if defined(RACE_LLVM_MOS) || defined(RACE_HOST)
#define RACE_NOINLINE __attribute__((noinline))
#elif defined(RACE_OSCAR64)
#define RACE_NOINLINE __noinline
#else
#define RACE_NOINLINE
#endif

#ifdef RACE_HOST
#include <stdio.h>
static volatile uint16_t race_host_fixed;
#define RACE_FIXED(type, address) (*(volatile type *)&race_host_fixed)
__attribute__((destructor)) static void race_host_print(void) {
    printf("checksum %u\n", (unsigned)race_host_fixed);
}
#else
#define RACE_FIXED(type, address) (*(volatile type *)(address))
#endif

#endif
