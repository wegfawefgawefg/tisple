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

MIT OR Apache-2.0, at your option. See `COPYRIGHT`, `LICENSE-MIT` and
`LICENSE-APACHE`. `third_party/chips` keeps its zlib license.
