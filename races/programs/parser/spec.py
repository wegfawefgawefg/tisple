"""A text adventure's parser and verbs over a script of typed commands"""

from kernels import Kernel, RESULT
from layout import Buf

COMMANDS = 300
NONE, CARRIED, IN_BOX = 255, 254, 253
UNKNOWN, VERB, NOUN, DIR, PREP, NOISE = range(6)
V_GO, V_TAKE, V_DROP, V_OPEN, V_CLOSE, V_UNLOCK, V_PUT, V_LOOK, V_INVENTORY, V_EXAMINE = \
    range(1, 11)
NORTH, SOUTH, EAST, WEST, UP, DOWN = range(6)
LAMP, KEY, COIN, BOX, DOOR, SWORD, BOOK, ROPE = range(8)
P_IN, P_WITH = 1, 2
(M_EMPTY, M_UNKNOWN, M_NO_VERB, M_WHERE, M_NO_WAY, M_DOOR_SHUT, M_ROOM, M_CARRYING,
 M_NOTHING, M_WHAT, M_FIXED, M_HAVE_IT, M_OK, M_NOT_HERE, M_NOT_CARRIED) = range(1, 16)
M_OBJECT = 16
M_LOCKED, M_ALREADY, M_CANT, M_NEED_KEY, M_BOX_SHUT, M_DESCRIBE = range(40, 46)
M_END = 255

VOCAB = [
    ("GO", V_GO, VERB), ("WALK", V_GO, VERB), ("TAKE", V_TAKE, VERB), ("GET", V_TAKE, VERB),
    ("DROP", V_DROP, VERB), ("OPEN", V_OPEN, VERB), ("CLOSE", V_CLOSE, VERB),
    ("SHUT", V_CLOSE, VERB), ("UNLOCK", V_UNLOCK, VERB), ("PUT", V_PUT, VERB),
    ("LOOK", V_LOOK, VERB), ("L", V_LOOK, VERB), ("INVENT", V_INVENTORY, VERB),
    ("I", V_INVENTORY, VERB), ("EXAMIN", V_EXAMINE, VERB), ("X", V_EXAMINE, VERB),
    ("NORTH", NORTH, DIR), ("N", NORTH, DIR), ("SOUTH", SOUTH, DIR), ("S", SOUTH, DIR),
    ("EAST", EAST, DIR), ("E", EAST, DIR), ("WEST", WEST, DIR), ("W", WEST, DIR),
    ("UP", UP, DIR), ("U", UP, DIR), ("DOWN", DOWN, DIR), ("D", DOWN, DIR),
    ("LAMP", LAMP, NOUN), ("LANTER", LAMP, NOUN), ("KEY", KEY, NOUN), ("COIN", COIN, NOUN),
    ("BOX", BOX, NOUN), ("DOOR", DOOR, NOUN), ("SWORD", SWORD, NOUN), ("BOOK", BOOK, NOUN),
    ("ROPE", ROPE, NOUN), ("IN", P_IN, PREP), ("INTO", P_IN, PREP), ("WITH", P_WITH, PREP),
    ("THE", 0, NOISE), ("A", 0, NOISE), ("AN", 0, NOISE), ("BRASS", 0, NOISE),
    ("GOLD", 0, NOISE), ("OLD", 0, NOISE),
]
EXITS = [
    [1, NONE, 3, NONE, NONE, NONE],
    [NONE, 0, 2, NONE, 4, NONE],
    [NONE, 5, NONE, 1, NONE, NONE],
    [NONE, NONE, NONE, 0, NONE, 4],
    [NONE, NONE, NONE, NONE, 3, 1],
    [2, NONE, NONE, NONE, NONE, NONE],
]
START = [0, 3, 4, 2, 1, 5, 0, 4]

# what players type: mostly sensible, some noise, typos and shouting
TEMPLATES = [
    "{dir}", "go {dir}", "walk {dirw}", "{dirw}", "take {obj}", "get the {obj}",
    "take brass lamp", "drop {obj}", "drop the {obj}", "open door", "open the box",
    "close box", "shut the door", "unlock door with key", "unlock the old door with the key",
    "put {obj} in box", "put the gold coin into the box", "look", "l", "inventory", "i",
    "examine {obj}", "x box", "xyzzy", "take", "go", "dance with {obj}", "the", "",
    "LOOK AT THE LAMP", "Get Lantern", "  go   north  ", "plugh {obj}", "in box", "examine",
]
OBJECT_WORDS = ["lamp", "key", "coin", "box", "door", "sword", "book", "rope", "lantern"]
DIRS = ["n", "s", "e", "w", "u", "d"]
DIR_WORDS = ["north", "south", "east", "west", "up", "down"]
# a walk through the whole house first, so every verb meets every state
OPENING = ["look", "take lamp", "take book", "e", "d", "take key", "u", "w", "n",
           "open door", "unlock door with key", "open the door", "e", "examine box",
           "open box", "put book in box", "x box", "take box", "s", "take sword", "n", "w",
           "u", "take coin", "take rope", "d", "i", "drop rope", "close door", "e"]


def make_script(rng):
    lines = list(OPENING)
    while len(lines) < COMMANDS:
        t = TEMPLATES[rng.below(len(TEMPLATES))]
        lines.append(t.format(dir=DIRS[rng.below(6)], dirw=DIR_WORDS[rng.below(6)],
                              obj=OBJECT_WORDS[rng.below(len(OBJECT_WORDS))]))
    text = "".join(line + "\r" for line in lines)
    data = [ord(c) for c in text] + [0]
    assert len(data) <= 4096
    return data + [0] * (4096 - len(data))


def inputs(rng):
    return {"script": make_script(rng), "log_buf": [0x5A] * 4096, "checksum": 0x5A5A}


class World:
    def __init__(self):
        self.room = 0
        self.loc = list(START)
        self.door_open, self.door_locked, self.box_open = 0, 1, 0
        self.log = []

    def say(self, code):
        self.log.append(code)

    def here(self, o):
        return self.loc[o] in (self.room, CARRIED)

    def at_door(self):
        return self.room in (1, 2)

    def describe(self):
        self.say(M_ROOM)
        self.say(self.room)
        self.list_at(self.room)

    def list_at(self, where):
        for o in range(8):
            if self.loc[o] == where:
                self.say(M_OBJECT + o)

    def go(self, d):
        if d == NONE:
            return self.say(M_WHERE)
        to = EXITS[self.room][d]
        if to == NONE:
            return self.say(M_NO_WAY)
        if ((self.room == 1 and d == EAST) or (self.room == 2 and d == WEST)) and \
                not self.door_open:
            return self.say(M_DOOR_SHUT)
        self.room = to
        self.describe()

    def open_close(self, n, opening):
        if n == NONE:
            return self.say(M_WHAT)
        if n == DOOR:
            if not self.at_door():
                return self.say(M_NOT_HERE)
            if opening and self.door_locked:
                return self.say(M_LOCKED)
            if self.door_open == opening:
                return self.say(M_ALREADY)
            self.door_open = opening
            return self.say(M_OK)
        if n == BOX:
            if not self.here(BOX):
                return self.say(M_NOT_HERE)
            if self.box_open == opening:
                return self.say(M_ALREADY)
            self.box_open = opening
            return self.say(M_OK)
        self.say(M_CANT)

    def command(self, kinds, ids):
        if not kinds:
            return self.say(M_EMPTY)
        for w, k in enumerate(kinds):
            if k == UNKNOWN:
                self.say(M_UNKNOWN)
                return self.say(w)
        noun1 = noun2 = prep = d = NONE
        for k, i in list(zip(kinds, ids))[1:]:
            if k == PREP and prep == NONE:
                prep = i
            elif k == NOUN:
                if prep == NONE and noun1 == NONE:
                    noun1 = i
                elif prep != NONE and noun2 == NONE:
                    noun2 = i
            elif k == DIR and d == NONE:
                d = i
        if kinds[0] == DIR:
            return self.go(ids[0])
        if kinds[0] != VERB:
            return self.say(M_NO_VERB)
        verb = ids[0]
        loc = self.loc
        if verb == V_GO:
            self.go(d)
        elif verb == V_TAKE:
            if noun1 == NONE:
                self.say(M_WHAT)
            elif noun1 == DOOR:
                self.say(M_FIXED)
            elif loc[noun1] == CARRIED:
                self.say(M_HAVE_IT)
            elif loc[noun1] == self.room:
                loc[noun1] = CARRIED
                self.say(M_OK)
            else:
                self.say(M_NOT_HERE)
        elif verb == V_DROP:
            if noun1 == NONE:
                self.say(M_WHAT)
            elif loc[noun1] != CARRIED:
                self.say(M_NOT_CARRIED)
            else:
                loc[noun1] = self.room
                self.say(M_OK)
        elif verb == V_OPEN:
            self.open_close(noun1, 1)
        elif verb == V_CLOSE:
            self.open_close(noun1, 0)
        elif verb == V_UNLOCK:
            if noun1 != DOOR:
                self.say(M_CANT)
            elif not self.at_door():
                self.say(M_NOT_HERE)
            elif noun2 != KEY or loc[KEY] != CARRIED:
                self.say(M_NEED_KEY)
            elif not self.door_locked:
                self.say(M_ALREADY)
            else:
                self.door_locked = 0
                self.say(M_OK)
        elif verb == V_PUT:
            if noun1 == NONE:
                self.say(M_WHAT)
            elif prep != P_IN or noun2 != BOX:
                self.say(M_CANT)
            elif loc[noun1] != CARRIED:
                self.say(M_NOT_CARRIED)
            elif not self.here(BOX):
                self.say(M_NOT_HERE)
            elif not self.box_open:
                self.say(M_BOX_SHUT)
            else:
                loc[noun1] = IN_BOX
                self.say(M_OK)
        elif verb == V_LOOK:
            self.describe()
        elif verb == V_INVENTORY:
            self.say(M_CARRYING)
            carried = [o for o in range(8) if loc[o] == CARRIED]
            for o in carried:
                self.say(M_OBJECT + o)
            if not carried:
                self.say(M_NOTHING)
        elif verb == V_EXAMINE:
            if noun1 == NONE:
                self.say(M_WHAT)
            elif not self.here(noun1):
                self.say(M_NOT_HERE)
            else:
                self.say(M_DESCRIBE)
                self.say(noun1)
                if noun1 == BOX and self.box_open:
                    self.list_at(IN_BOX)


def split_words(line):
    words = []
    for word in "".join(c if "A" <= c <= "Z" else " " for c in line.upper()).split():
        if len(words) < 8:
            words.append(word[:6])
    return words


def reference(ins):
    script = ins["script"]
    text = "".join(chr(c) for c in script[:script.index(0)])
    world = World()
    moves = 0
    for line in text.split("\r")[:-1]:
        kinds, ids = [], []
        for word in split_words(line[:64]):
            match = [(i, k) for t, i, k in VOCAB if t == word]
            i, k = match[0] if match else (NONE, UNKNOWN)
            if k != NOISE:
                kinds.append(k)
                ids.append(i)
        world.command(kinds, ids)
        world.say(M_END)
        moves += 1
    log = world.log
    a = b = 0
    tail = [world.room] + world.loc
    tail += [world.door_open + world.door_locked * 2 + world.box_open * 4, moves, len(log)]
    for v in log + tail:
        a = (a + v) & 0xFFFF
        b = (b + a) & 0xFFFF
    out = list(ins["log_buf"])
    out[:len(log)] = log
    return {"log_buf": out, "checksum": a ^ b}


PROGRAM = Kernel("parser", "Text adventure: %d typed commands tokenized, looked up, "
                 "dispatched" % COMMANDS,
                 [Buf("script", "u8", 4096, 0x2000), Buf("log_buf", "u8", 4096, 0x3000),
                  Buf("checksum", "u16", None, RESULT)],
                 inputs, reference)
