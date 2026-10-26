/* fill 8 KB with a byte, through the C library */
#include <string.h>
#include "race.h"

int main(void) {
    memset(buf, value, 8192);
    return 0;
}
