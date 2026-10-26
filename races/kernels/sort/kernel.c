/* insertion sort of 256 bytes in place */
#include "race.h"

int main(void) {
    uint8_t i = 1;
    do {
        uint8_t v = arr[i];
        uint8_t j = i;
        while (j != 0 && arr[j - 1] > v) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = v;
    } while (++i != 0);
    return 0;
}
