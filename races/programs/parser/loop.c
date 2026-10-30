/* program.c with the word compare as a loop instead of memcmp.
 * A text adventure's parser and verbs, run over a script of 300 typed commands (the
 * race's script buffer: lines ending in a return, a 0 at the end). Each line is
 * upper-cased and split into words of at most 6 letters, each word looked up in the
 * vocabulary, the noise words dropped, and the verb dispatched to its handler, which
 * moves the player, takes and drops things, opens the door and the box. Every answer is
 * a byte appended to the race's log buffer; the checksum folds the log and the world. */
#include "race.h"

#define WORD_LEN 6
#define MAX_WORDS 8
#define LINE_MAX 64
#define NONE 255
#define CARRIED 254
#define IN_BOX 253
#define OBJECTS 8

enum { UNKNOWN, VERB, NOUN, DIR, PREP, NOISE };
enum { V_GO = 1, V_TAKE, V_DROP, V_OPEN, V_CLOSE, V_UNLOCK, V_PUT, V_LOOK, V_INVENTORY,
       V_EXAMINE };
enum { NORTH, SOUTH, EAST, WEST, UP, DOWN };
enum { LAMP, KEY, COIN, BOX, DOOR, SWORD, BOOK, ROPE };
enum { P_IN = 1, P_WITH };
enum { M_EMPTY = 1, M_UNKNOWN, M_NO_VERB, M_WHERE, M_NO_WAY, M_DOOR_SHUT, M_ROOM,
       M_CARRYING, M_NOTHING, M_WHAT, M_FIXED, M_HAVE_IT, M_OK, M_NOT_HERE,
       M_NOT_CARRIED, M_OBJECT = 16, M_LOCKED = 40, M_ALREADY, M_CANT, M_NEED_KEY,
       M_BOX_SHUT, M_DESCRIBE, M_END = 255 };

struct word {
    char text[WORD_LEN];
    uint8_t id;
    uint8_t kind;
};

static const struct word VOCAB[] = {
    {"GO    ", V_GO, VERB},         {"WALK  ", V_GO, VERB},       {"TAKE  ", V_TAKE, VERB},
    {"GET   ", V_TAKE, VERB},       {"DROP  ", V_DROP, VERB},     {"OPEN  ", V_OPEN, VERB},
    {"CLOSE ", V_CLOSE, VERB},      {"SHUT  ", V_CLOSE, VERB},    {"UNLOCK", V_UNLOCK, VERB},
    {"PUT   ", V_PUT, VERB},        {"LOOK  ", V_LOOK, VERB},     {"L     ", V_LOOK, VERB},
    {"INVENT", V_INVENTORY, VERB},  {"I     ", V_INVENTORY, VERB},
    {"EXAMIN", V_EXAMINE, VERB},    {"X     ", V_EXAMINE, VERB},
    {"NORTH ", NORTH, DIR},         {"N     ", NORTH, DIR},       {"SOUTH ", SOUTH, DIR},
    {"S     ", SOUTH, DIR},         {"EAST  ", EAST, DIR},        {"E     ", EAST, DIR},
    {"WEST  ", WEST, DIR},          {"W     ", WEST, DIR},        {"UP    ", UP, DIR},
    {"U     ", UP, DIR},            {"DOWN  ", DOWN, DIR},        {"D     ", DOWN, DIR},
    {"LAMP  ", LAMP, NOUN},         {"LANTER", LAMP, NOUN},       {"KEY   ", KEY, NOUN},
    {"COIN  ", COIN, NOUN},         {"BOX   ", BOX, NOUN},        {"DOOR  ", DOOR, NOUN},
    {"SWORD ", SWORD, NOUN},        {"BOOK  ", BOOK, NOUN},       {"ROPE  ", ROPE, NOUN},
    {"IN    ", P_IN, PREP},         {"INTO  ", P_IN, PREP},       {"WITH  ", P_WITH, PREP},
    {"THE   ", 0, NOISE},           {"A     ", 0, NOISE},         {"AN    ", 0, NOISE},
    {"BRASS ", 0, NOISE},           {"GOLD  ", 0, NOISE},         {"OLD   ", 0, NOISE},
};
#define VOCAB_COUNT (sizeof(VOCAB) / sizeof(VOCAB[0]))

/* exits by room and direction: north, south, east, west, up, down */
static const uint8_t EXITS[6 * 6] = {
    1, NONE, 3, NONE, NONE, NONE,
    NONE, 0, 2, NONE, 4, NONE,
    NONE, 5, NONE, 1, NONE, NONE,
    NONE, NONE, NONE, 0, NONE, 4,
    NONE, NONE, NONE, NONE, 3, 1,
    2, NONE, NONE, NONE, NONE, NONE,
};

static const uint8_t START[OBJECTS] = {0, 3, 4, 2, 1, 5, 0, 4};

static char line[LINE_MAX];
static uint8_t line_len;
static char words[MAX_WORDS][WORD_LEN];
static uint8_t word_ids[MAX_WORDS];
static uint8_t word_kinds[MAX_WORDS];
static uint8_t word_count;

static uint8_t room;
static uint8_t loc[OBJECTS];
static uint8_t door_open, door_locked, box_open;
static uint16_t moves;
static uint16_t log_len;
static uint16_t script_at;

static uint8_t noun1, noun2, prep, dir;

static void say(uint8_t code) {
    log_buf[log_len] = code;
    log_len++;
}

/* the next line of the script, upper-cased; 0 at the end of the script */
static uint8_t read_line(void) {
    char c;
    line_len = 0;
    if (script[script_at] == 0) return 0;
    for (;;) {
        c = script[script_at];
        if (c == 0) break;
        script_at++;
        if (c == '\r') break;
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (line_len < LINE_MAX) {
            line[line_len] = c;
            line_len++;
        }
    }
    return 1;
}

/* words of letters, each cut to 6 and padded with spaces; at most 8 */
static void split_words(void) {
    uint8_t i = 0, n;
    word_count = 0;
    while (i < line_len && word_count < MAX_WORDS) {
        while (i < line_len && (line[i] < 'A' || line[i] > 'Z')) i++;
        if (i == line_len) break;
        n = 0;
        while (i < line_len && line[i] >= 'A' && line[i] <= 'Z') {
            if (n < WORD_LEN) {
                words[word_count][n] = line[i];
                n++;
            }
            i++;
        }
        while (n < WORD_LEN) {
            words[word_count][n] = ' ';
            n++;
        }
        word_count++;
    }
}

static uint8_t same_text(uint8_t v, uint8_t w) {
    uint8_t k;
    for (k = 0; k < WORD_LEN; k++) {
        if (VOCAB[v].text[k] != words[w][k]) return 0;
    }
    return 1;
}

static void look_up(uint8_t w) {
    uint8_t v;
    for (v = 0; v < VOCAB_COUNT; v++) {
        if (same_text(v, w)) {
            word_ids[w] = VOCAB[v].id;
            word_kinds[w] = VOCAB[v].kind;
            return;
        }
    }
    word_ids[w] = NONE;
    word_kinds[w] = UNKNOWN;
}

/* look every word up and drop the noise words */
static void look_up_words(void) {
    uint8_t w, kept = 0;
    for (w = 0; w < word_count; w++) {
        look_up(w);
        if (word_kinds[w] != NOISE) {
            word_ids[kept] = word_ids[w];
            word_kinds[kept] = word_kinds[w];
            kept++;
        }
    }
    word_count = kept;
}

static void list_objects_at(uint8_t where) {
    uint8_t o;
    for (o = 0; o < OBJECTS; o++) {
        if (loc[o] == where) say(M_OBJECT + o);
    }
}

static void describe(void) {
    say(M_ROOM);
    say(room);
    list_objects_at(room);
}

static uint8_t here(uint8_t o) {
    return loc[o] == room || loc[o] == CARRIED;
}

static uint8_t at_door(void) {
    return room == 1 || room == 2;
}

/* the handlers give their answer, or 0 when they have said it themselves */
static uint8_t do_go(void) {
    uint8_t to;
    if (dir == NONE) return M_WHERE;
    to = EXITS[room * 6 + dir];
    if (to == NONE) return M_NO_WAY;
    if (((room == 1 && dir == EAST) || (room == 2 && dir == WEST)) && !door_open)
        return M_DOOR_SHUT;
    room = to;
    describe();
    return 0;
}

static uint8_t do_inventory(void) {
    uint8_t o, any = 0;
    say(M_CARRYING);
    for (o = 0; o < OBJECTS; o++) {
        if (loc[o] == CARRIED) {
            say(M_OBJECT + o);
            any = 1;
        }
    }
    return any ? 0 : M_NOTHING;
}

static uint8_t do_take(void) {
    if (noun1 == NONE) return M_WHAT;
    if (noun1 == DOOR) return M_FIXED;
    if (loc[noun1] == CARRIED) return M_HAVE_IT;
    if (loc[noun1] != room) return M_NOT_HERE;
    loc[noun1] = CARRIED;
    return M_OK;
}

static uint8_t do_drop(void) {
    if (noun1 == NONE) return M_WHAT;
    if (loc[noun1] != CARRIED) return M_NOT_CARRIED;
    loc[noun1] = room;
    return M_OK;
}

/* open is 1 for open, 0 for close */
static uint8_t do_open(uint8_t open) {
    if (noun1 == NONE) return M_WHAT;
    if (noun1 == DOOR) {
        if (!at_door()) return M_NOT_HERE;
        if (open && door_locked) return M_LOCKED;
        if (door_open == open) return M_ALREADY;
        door_open = open;
        return M_OK;
    }
    if (noun1 == BOX) {
        if (!here(BOX)) return M_NOT_HERE;
        if (box_open == open) return M_ALREADY;
        box_open = open;
        return M_OK;
    }
    return M_CANT;
}

static uint8_t do_unlock(void) {
    if (noun1 != DOOR) return M_CANT;
    if (!at_door()) return M_NOT_HERE;
    if (noun2 != KEY || loc[KEY] != CARRIED) return M_NEED_KEY;
    if (!door_locked) return M_ALREADY;
    door_locked = 0;
    return M_OK;
}

static uint8_t do_put(void) {
    if (noun1 == NONE) return M_WHAT;
    if (prep != P_IN || noun2 != BOX) return M_CANT;
    if (loc[noun1] != CARRIED) return M_NOT_CARRIED;
    if (!here(BOX)) return M_NOT_HERE;
    if (!box_open) return M_BOX_SHUT;
    loc[noun1] = IN_BOX;
    return M_OK;
}

static uint8_t do_examine(void) {
    if (noun1 == NONE) return M_WHAT;
    if (!here(noun1)) return M_NOT_HERE;
    say(M_DESCRIBE);
    say(noun1);
    if (noun1 == BOX && box_open) list_objects_at(IN_BOX);
    return 0;
}

/* the verb's noun, a preposition and its noun, and a direction, from the other words */
static void find_objects(void) {
    uint8_t w;
    noun1 = noun2 = prep = dir = NONE;
    for (w = 1; w < word_count; w++) {
        if (word_kinds[w] == PREP && prep == NONE) prep = word_ids[w];
        else if (word_kinds[w] == NOUN) {
            if (prep == NONE && noun1 == NONE) noun1 = word_ids[w];
            else if (prep != NONE && noun2 == NONE) noun2 = word_ids[w];
        } else if (word_kinds[w] == DIR && dir == NONE) dir = word_ids[w];
    }
}

static uint8_t run_command(void) {
    uint8_t w;
    if (word_count == 0) return M_EMPTY;
    for (w = 0; w < word_count; w++) {
        if (word_kinds[w] == UNKNOWN) {
            say(M_UNKNOWN);
            say(w);
            return 0;
        }
    }
    find_objects();
    if (word_kinds[0] == DIR) {
        dir = word_ids[0];
        return do_go();
    }
    if (word_kinds[0] != VERB) return M_NO_VERB;
    switch (word_ids[0]) {
    case V_GO: return do_go();
    case V_TAKE: return do_take();
    case V_DROP: return do_drop();
    case V_OPEN: return do_open(1);
    case V_CLOSE: return do_open(0);
    case V_UNLOCK: return do_unlock();
    case V_PUT: return do_put();
    case V_LOOK:
        describe();
        return 0;
    case V_INVENTORY: return do_inventory();
    case V_EXAMINE: return do_examine();
    }
    return 0;
}

int main(void) {
    uint16_t i, sum_a, sum_b;
    uint8_t o, answer;
    room = 0;
    for (o = 0; o < OBJECTS; o++) loc[o] = START[o];
    door_open = 0;
    door_locked = 1;
    box_open = 0;
    moves = 0;
    log_len = 0;
    script_at = 0;
    while (read_line()) {
        split_words();
        look_up_words();
        answer = run_command();
        if (answer != 0) say(answer);
        say(M_END);
        moves++;
    }
    sum_a = 0;
    sum_b = 0;
    for (i = 0; i < log_len; i++) {
        sum_a += log_buf[i];
        sum_b += sum_a;
    }
    sum_a += room;
    sum_b += sum_a;
    for (o = 0; o < OBJECTS; o++) {
        sum_a += loc[o];
        sum_b += sum_a;
    }
    sum_a += door_open + door_locked * 2 + box_open * 4;
    sum_b += sum_a;
    sum_a += moves;
    sum_b += sum_a;
    sum_a += log_len;
    sum_b += sum_a;
    checksum = sum_a ^ sum_b;
    return 0;
}
