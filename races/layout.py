"""Race buffers: typed arrays at fixed addresses, and their bytes in each layout.

A buffer is a scalar, an array of a scalar type, or an array of structs. Every kernel's
inputs and outputs are buffers in $2000-$5FFF, which no contestant's code or data uses.

Layouts:
  packed  C's: elements one after another, each field little-endian
  planar  Tisple's (and Oscar64's __striped): one byte plane per field byte, low byte
          first, each plane `count` bytes long. A u8 array is the same in both.
"""

SIZES = {"u8": 1, "i8": 1, "u16": 2, "i16": 2}
C_TYPES = {"u8": "uint8_t", "i8": "int8_t", "u16": "uint16_t", "i16": "int16_t"}
BUFFER_LOW = 0x2000
BUFFER_HIGH = 0x6000  # one past the end


class Buf:
    """name: the C and Tisple name. kind: a scalar type, or a list of (field, type) for a
    struct named `struct`. count: None for a scalar. tisple_declared: False when the
    Tisple version reaches it through integer-made pointers (the hi-res screen)."""

    def __init__(self, name, kind, count, address, struct=None, tisple_declared=True):
        self.name = name
        self.kind = kind
        self.count = count
        self.address = address
        self.struct = struct
        self.tisple_declared = tisple_declared

    def fields(self):
        if isinstance(self.kind, list):
            return self.kind
        return [(None, self.kind)]

    def element_size(self):
        return sum(SIZES[t] for _, t in self.fields())

    def size(self):
        return self.element_size() * (self.count or 1)

    def is_multibyte(self):
        return self.element_size() > 1 and self.count is not None

    def c_type(self):
        if self.struct is not None:
            return "struct " + self.struct
        return C_TYPES[self.kind]


def _int_bytes(value, kind):
    size = SIZES[kind]
    value &= (1 << (8 * size)) - 1
    return [(value >> (8 * i)) & 0xFF for i in range(size)]


def _from_bytes(data, kind):
    value = 0
    for i, b in enumerate(data):
        value |= b << (8 * i)
    if kind.startswith("i") and value >= 1 << (8 * SIZES[kind] - 1):
        value -= 1 << (8 * SIZES[kind])
    return value


def _elements(buf, value):
    """the value as a list of per-element field-value lists"""
    items = [value] if buf.count is None else value
    out = []
    for item in items:
        if isinstance(buf.kind, list):
            out.append([item[f] for f, _ in buf.kind])
        else:
            out.append([item])
    return out


def encode(buf, value, layout):
    elements = _elements(buf, value)
    fields = buf.fields()
    if layout == "packed" or buf.count is None:
        data = []
        for element in elements:
            for (_, kind), v in zip(fields, element):
                data += _int_bytes(v, kind)
        return bytes(data)
    data = []
    for index, (_, kind) in enumerate(fields):
        for byte in range(SIZES[kind]):
            data += [_int_bytes(e[index], kind)[byte] for e in elements]
    return bytes(data)


def decode(buf, data, layout):
    fields = buf.fields()
    count = buf.count or 1
    elements = [[0] * len(fields) for _ in range(count)]
    if layout == "packed" or buf.count is None:
        at = 0
        for element in elements:
            for index, (_, kind) in enumerate(fields):
                element[index] = _from_bytes(data[at:at + SIZES[kind]], kind)
                at += SIZES[kind]
    else:
        at = 0
        for index, (_, kind) in enumerate(fields):
            planes = []
            for _byte in range(SIZES[kind]):
                planes.append(data[at:at + count])
                at += count
            for i in range(count):
                elements[i][index] = _from_bytes([p[i] for p in planes], kind)
    values = []
    for element in elements:
        if isinstance(buf.kind, list):
            values.append({f: v for (f, _), v in zip(buf.kind, element)})
        else:
            values.append(element[0])
    return values[0] if buf.count is None else values


class Rng:
    """xorshift32, so the inputs never depend on the Python version"""

    def __init__(self, seed):
        self.state = seed or 1

    def next(self):
        x = self.state
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.state = x
        return x

    def below(self, n):
        return self.next() % n

    def bytes(self, n, low=0, high=256):
        return [low + self.below(high - low) for _ in range(n)]
