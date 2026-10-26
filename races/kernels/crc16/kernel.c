/* CRC-16-CCITT (polynomial $1021, start $FFFF), bit by bit, over 1 KB */
#include "race.h"

int main(void) {
    uint16_t crc = 0xFFFF;
    uint16_t i;
    uint8_t k;
    for (i = 0; i < 1024; i++) {
        crc ^= (uint16_t)(data[i]) << 8;
        for (k = 0; k < 8; k++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    result = crc;
    return 0;
}
