/* Sorting as a program does it. keys (1024 words) by quicksort with an explicit stack of
 * ranges, leaving runs under CUTOFF for one insertion sort pass at the end; table (64
 * records of score and id) by a stable insertion sort, high scores first; then the best
 * 16 as a high-score table that 64 new scores are entered into. A checksum of all three
 * goes to checksum. struct entry is in buffers.h: uint16_t score, uint8_t id. */
#include "race.h"

#define KEYS 1024
#define RECORDS 64
#define TOP 16
#define CUTOFF 8
#define STACK 24

static uint16_t lo_stack[STACK];
static uint16_t hi_stack[STACK];
static uint8_t sp;

static void swap_keys(uint16_t i, uint16_t j) {
    uint16_t t = keys[i];
    keys[i] = keys[j];
    keys[j] = t;
}

static void push(uint16_t lo, uint16_t hi) {
    lo_stack[sp] = lo;
    hi_stack[sp] = hi;
    sp++;
}

/* Hoare's partition around the median of the first, middle and last keys; every key in
 * lo..split is at most every key in split+1..hi */
static uint16_t partition(uint16_t lo, uint16_t hi) {
    uint16_t mid = lo + ((hi - lo) >> 1);
    uint16_t pivot, i, j;
    if (keys[mid] < keys[lo]) swap_keys(mid, lo);
    if (keys[hi] < keys[lo]) swap_keys(hi, lo);
    if (keys[hi] < keys[mid]) swap_keys(hi, mid);
    pivot = keys[mid];
    i = lo;
    j = hi;
    for (;;) {
        while (keys[i] < pivot) i++;
        while (keys[j] > pivot) j--;
        if (i >= j) return j;
        swap_keys(i, j);
        i++;
        j--;
    }
}

static void insertion_sort_keys(uint16_t n) {
    uint16_t i, j, v;
    for (i = 1; i < n; i++) {
        v = keys[i];
        j = i;
        while (j > 0 && keys[j - 1] > v) {
            keys[j] = keys[j - 1];
            j--;
        }
        keys[j] = v;
    }
}

static void quicksort_keys(uint16_t n) {
    uint16_t lo, hi, split;
    sp = 0;
    push(0, n - 1);
    while (sp != 0) {
        sp--;
        lo = lo_stack[sp];
        hi = hi_stack[sp];
        while (hi - lo >= CUTOFF) {
            split = partition(lo, hi);
            /* push the bigger side and go on with the smaller: the stack stays short */
            if (split - lo < hi - split) {
                push(split + 1, hi);
                hi = split;
            } else {
                push(lo, split);
                lo = split + 1;
            }
        }
    }
    insertion_sort_keys(n);
}

/* high scores first; equal scores keep their order */
static void sort_table(void) {
    uint8_t i, j, id;
    uint16_t score;
    for (i = 1; i < RECORDS; i++) {
        score = table[i].score;
        id = table[i].id;
        j = i;
        while (j > 0 && table[j - 1].score < score) {
            table[j].score = table[j - 1].score;
            table[j].id = table[j - 1].id;
            j--;
        }
        table[j].score = score;
        table[j].id = id;
    }
}

/* a new score goes in below every score at least as high, the last one drops off */
static void enter_score(uint16_t score, uint8_t id) {
    uint8_t pos, i;
    if (score <= top[TOP - 1].score) return;
    pos = 0;
    while (top[pos].score >= score) pos++;
    for (i = TOP - 1; i > pos; i--) {
        top[i].score = top[i - 1].score;
        top[i].id = top[i - 1].id;
    }
    top[pos].score = score;
    top[pos].id = id;
}

static uint16_t sum_a, sum_b;

static void fold(uint16_t v) {
    sum_a += v;
    sum_b += sum_a;
}

int main(void) {
    uint16_t i;
    uint8_t k;
    quicksort_keys(KEYS);
    sort_table();
    for (k = 0; k < TOP; k++) {
        top[k].score = table[k].score;
        top[k].id = table[k].id;
    }
    for (k = 0; k < RECORDS; k++) {
        enter_score(news[k], 100 + k);
    }
    sum_a = 0;
    sum_b = 0;
    for (i = 0; i < KEYS; i++) fold(keys[i]);
    for (k = 0; k < RECORDS; k++) {
        fold(table[k].score);
        fold(table[k].id);
    }
    for (k = 0; k < TOP; k++) {
        fold(top[k].score);
        fold(top[k].id);
    }
    checksum = sum_a ^ sum_b;
    return 0;
}
