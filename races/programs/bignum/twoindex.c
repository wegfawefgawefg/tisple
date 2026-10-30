/* program.c with the digits turned round by two indexes meeting in the middle (Oscar64
 * v1.32.273 miscompiles program.c's dec[i] and dec[dec_len - 1 - i], docs/races.md).
 * Decimal arithmetic as games and calculators do it. A score is 8 decimal digits, most
 * significant first; each award (a u16) is split into digits by subtracting powers of
 * ten and added digit by digit with a carry. 8 games of 50 awards; after each game the
 * score goes into the high-score table (8 scores, best first) if it beats one. Then a
 * bignum: 100! built in little-endian bytes by multiplying by 2..100, and printed in
 * decimal into dec by dividing it by 10 again and again. The checksum folds the table,
 * the last score and the digits. */
#include "race.h"

#define DIGITS 8
#define GAMES 8
#define AWARDS 50
#define HIGH 8
#define BIG 80

static const uint16_t POWERS[4] = {10000, 1000, 100, 10};

static uint8_t score[DIGITS];
static uint8_t big[BIG];
static uint8_t big_len;
static uint8_t dec_len;

static void add_points(uint16_t award) {
    uint8_t add[5];
    uint8_t k, i, at, v, carry;
    for (k = 0; k < 4; k++) {
        add[k] = 0;
        while (award >= POWERS[k]) {
            award -= POWERS[k];
            add[k]++;
        }
    }
    add[4] = (uint8_t)award;
    carry = 0;
    for (i = 0; i < DIGITS; i++) {
        at = DIGITS - 1 - i;
        v = score[at] + carry;
        if (i < 5) v += add[4 - i];
        carry = 0;
        if (v >= 10) {
            v -= 10;
            carry = 1;
        }
        score[at] = v;
    }
}

/* is the score above high-score entry e */
static uint8_t beats(uint8_t e) {
    uint8_t i;
    const uint8_t *h = high + e * DIGITS;
    for (i = 0; i < DIGITS; i++) {
        if (score[i] != h[i]) return score[i] > h[i];
    }
    return 0;
}

static void enter_high_score(void) {
    uint8_t e, i;
    for (e = 0; e < HIGH; e++) {
        if (beats(e)) break;
    }
    if (e == HIGH) return;
    for (i = HIGH * DIGITS - 1; i >= (e + 1) * DIGITS; i--) high[i] = high[i - DIGITS];
    for (i = 0; i < DIGITS; i++) high[e * DIGITS + i] = score[i];
}

static void factorial(uint8_t n) {
    uint8_t m, i;
    uint16_t v, carry;
    big[0] = 1;
    big_len = 1;
    for (m = 2; m <= n; m++) {
        carry = 0;
        for (i = 0; i < big_len; i++) {
            v = big[i] * m + carry;
            big[i] = (uint8_t)v;
            carry = v >> 8;
        }
        while (carry != 0) {
            big[big_len++] = (uint8_t)carry;
            carry >>= 8;
        }
    }
}

/* the digits come out last first, so they are turned round at the end */
static void print_big(void) {
    uint8_t i, j, t;
    uint16_t v, rem;
    dec_len = 0;
    while (big_len > 0) {
        rem = 0;
        i = big_len;
        while (i != 0) {
            i--;
            v = (rem << 8) | big[i];
            big[i] = (uint8_t)(v / 10);
            rem = v % 10;
        }
        dec[dec_len++] = '0' + (uint8_t)rem;
        while (big_len > 0 && big[big_len - 1] == 0) big_len--;
    }
    for (i = 0, j = dec_len - 1; i < j; i++, j--) {
        t = dec[i];
        dec[i] = dec[j];
        dec[j] = t;
    }
}

int main(void) {
    uint8_t g, k, i;
    uint16_t sum_a = 0, sum_b = 0;
    for (i = 0; i < HIGH * DIGITS; i++) high[i] = 0;
    for (g = 0; g < GAMES; g++) {
        for (i = 0; i < DIGITS; i++) score[i] = 0;
        for (k = 0; k < AWARDS; k++) add_points(points[g * AWARDS + k]);
        enter_high_score();
    }
    factorial(100);
    print_big();
    for (i = 0; i < HIGH * DIGITS; i++) {
        sum_a += high[i];
        sum_b += sum_a;
    }
    for (i = 0; i < DIGITS; i++) {
        sum_a += score[i];
        sum_b += sum_a;
    }
    for (i = 0; i < dec_len; i++) {
        sum_a += dec[i];
        sum_b += sum_a;
    }
    sum_a += dec_len;
    sum_b += sum_a;
    checksum = sum_a ^ sum_b;
    return 0;
}
