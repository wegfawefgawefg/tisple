# Tisple ][

A small typed Lisp for the Apple II, with an optimizing 6502 compiler that
knows what your code costs in cycles before it runs.

```scheme
(fn read_key
    (let key (io_read u8 0xC000))
    (bit_and key 0x7F))
```

Status: early. Design and milestones in `docs/design.md`.

```sh
cmake --preset dev && cmake --build --preset dev
./build-debug/tisple_tests
```
