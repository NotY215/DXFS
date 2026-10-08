# DXFS

A digit-container file format and terminal editor, in C++17.

**Honest math first:** 100,000 arbitrary decimal digits = 40.5 KiB of entropy,
so they cannot fit in 1 KiB. DXFS instead stores *generators* — the smallest
recipe that reproduces the digits — so 100k digits of π really do take 33 bytes.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Binaries:

* `build/dxtool` — pack / unpack / info / gen / check
* `build/dxedit` — full-screen digit editor (POSIX)

## Quick tour

```sh
# honest path: 100k random digits -> 42 KB, close to the 40.5 KB entropy floor
./build/dxtool pack rand.txt rand.dx

# generative path: 100k digits of pi -> 33 bytes total
./build/dxtool gen pi 100000 pi.dx

# repeating pattern -> ~37 bytes
./build/dxtool gen pattern 100000 rep.dx 314159

# open either in the editor
./build/dxedit pi.dx
```

## Codecs

| codec    | representation                    | 100k random | 100k of π |
|----------|-----------------------------------|-------------|-----------|
| packed   | 19 digits per uint64              | 42.1 KB     | 42.1 KB   |
| rle      | (digit, run) pairs                | varies      | small     |
| pattern  | one period + repeat count         | no gain     | 2 B       |
| constant | 1-byte id, digits recomputed      | n/a         | **33 B**  |
| lcg      | 8-byte seed                       | 40 B        | n/a       |

See `src/format.cpp` for the details.

# File tree recap
```
dxfs/
├── CMakeLists.txt
├── README.md                       (optional)
├── .gitignore                      (optional)
├── cmake/
│   ├── roundtrip.cmake
│   └── pi_test.cmake
├── include/
│   └── dx/
│       ├── bigint.hpp
│       └── format.hpp
└── src/
├── format.cpp                  → libdxfmt.a
├── dxtool.cpp                  → dxtool  (CLI, has its own main)
└── dxedit.cpp                  → dxedit  (editor, has its own main)
```

## Project policies

- [Contributing](CONTRIBUTING.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)
- [Security](SECURITY.md)
- [Support](SUPPORT.md)
- [Citation](CITATION.cff)
- [Governance](GOVERNANCE.md)
- [License](LICENSE)
