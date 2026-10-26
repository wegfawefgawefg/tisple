/* copy 4 KB, through the C library */
#include <string.h>
#include "race.h"

int main(void) {
    memcpy(dst, src, 4096);
    return 0;
}
