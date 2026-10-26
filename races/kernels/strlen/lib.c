/* length of a zero-terminated string (3000 bytes), through the C library */
#include <string.h>
#include "race.h"

int main(void) {
    result = strlen((const char *)str);
    return 0;
}
