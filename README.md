# Tisple ][

A small typed Lisp for the Apple II, with an optimizing 6502 compiler that
knows what your code costs in cycles before it runs.

```scheme
(fn read_key (returns u8)
    (let key (io_read u8 0xC000))
    (& key 0x7F))
```

Status: early. Design and milestones in `docs/design.md`.

```sh
cmake --preset dev && cmake --build --preset dev
./build-debug/tisple_tests
```

## License

GPL-3.0-or-later (`LICENSE`), with a runtime exception: the startup and runtime
code the compiler puts into your programs is also MIT, so programs built with
Tisple can use any license. See `COPYRIGHT`. Code under `third_party/` keeps
its own license.
