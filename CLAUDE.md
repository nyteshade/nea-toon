# Amiga TOON Port - Development Guide

## Project Overview

This is a native AmigaOS CLI implementation of the [TOON format](https://github.com/toon-format/spec) (Token-Oriented Object Notation), targeting SAS/C 6.58 on 68020+ Amigas.

The C implementation is a **clean-room port written from the spec**, not a transpilation of the TypeScript reference implementation. The TypeScript repo (`toon/`) provides spec version info, examples, and test fixtures.

## Repository Layout

```
build_port.py          Build script - regenerates toon-port/ from src/ + toon repo
src/                   Our C source files (the canonical editable sources)
toon-port/             Generated output (do not edit directly - rebuild via build_port.py)
toon/                  Upstream toon-format/toon repo (git clone)
SAS-C-GUIDE.md         SAS/C 6.58 reference for cross-compilation via vamos
vamos_build.cfg        vamos config for direct sc invocation during development
build.sh               Legacy build script (use build_port.py instead)
```

## Workflow

1. Edit C sources in `src/`
2. Run `python3 build_port.py --clean --with-tests` to regenerate `toon-port/`
3. Build via vamos: `cd toon-port && vamos -V sc:$SC -V src:$(pwd) --cwd src: -c .vamosrc sc:c/smake`
4. Test: `vamos -S -C 68020 -m 8192 -H emu -s 512 -V sc:$SC -V src:$(pwd) -- src:toon encode src:examples/users.json`

## SAS/C 6.58 Constraints (CRITICAL)

These constraints affect ALL code in src/. Violating them causes silent bugs or compiler errors.

### Language
- **C89 only**: No `//` comments, no mixed declarations/code, no VLAs, no designated initializers, no compound literals
- **No 64-bit integers**: `long long` = `long` = 32 bits. `int64_t` is 32-bit. Shifts >= 32 are UB.
- **No `<stdbool.h>`**: We typedef `toon_bool` as int with TRUE/FALSE macros
- **No `<stdint.h>`**: Use `long` for 32-bit, `short` for 16-bit
- **`inline` keyword**: Must be `__inline` for SAS/C; we `#define inline __inline` under `__SASC` in toon.h
- **Identifier limit**: Default 31 chars, we use `IDLEN=80`. Keep function names under 80 chars.
- **`INT32_MIN` trap**: Must be `(-2147483647L - 1L)`, NOT `(-2147483648L)` (the literal exceeds LONG_MAX)

### Libraries
- **No `%g/%e/%f` in printf**: `scnb.lib` (no-buffering) does not support float format specifiers. The `format_number()` function in toon_util.c handles this manually using FPU arithmetic.
- **No `<math.h>` dependency for encoding/decoding**: Only toon_util.c uses `floor()` from math.h
- **Link order matters**: `lib:c.o` first, then .o files, then `LIB lib:scnb.lib lib:scmnb.lib lib:amiga.lib`

### Pure/Resident
- The default build uses `lib:c.o` (not resident-capable)
- For pure binaries: change to `lib:cres.o` and remove `DATA=FARONLY` from SCoptions
- WARNING: `scnb.lib` has absolute data references that prevent true purity. A fully pure build requires replacing stdio with direct AmigaOS dos.library calls.

### vamos Limitations
- `icon.library` must be faked (`mode=fake` in .vamosrc) for smake to work
- `cres.o` (resident startup) crashes in vamos - use `c.o` for vamos testing
- No graphics/intuition/network support
- Console I/O behavior differs from real hardware
- Always test on real Amiga or Amiberry for production use

## Source File Map

| File | Purpose | Key functions |
|------|---------|---------------|
| `toon.h` | Header, types, API declarations | JsonValue, StrBuf, all public APIs |
| `main.c` | CLI interface | Path parser, get/set/del, arg parsing |
| `toon_decode.c` | TOON → JSON | `toon_decode()`, line scanner, header parser |
| `toon_encode.c` | JSON → TOON | `toon_encode()`, tabular detection, list emit |
| `json_parse.c` | JSON text → JsonValue | `json_parse()` |
| `json_emit.c` | JsonValue → JSON text | `json_emit()` |
| `toon_util.c` | Shared utilities | JsonValue constructors, StrBuf, `format_number()`, quoting |

## When Updating for New Spec Versions

1. Clone/pull the latest `toon-format/spec` repo
2. Read the SPEC.md changelog for breaking changes
3. Update the relevant decoder/encoder logic in `src/`
4. Run `python3 build_port.py --spec-repo /path/to/spec --with-tests --clean`
5. The generated `test_runner.c` will have updated test cases from the spec fixtures
6. Build and run tests to validate

## Common Tasks

### Adding a new CLI command
Edit `src/main.c`: add option parsing in the `while (i < argc)` loop, add a new `if (strcmp(command, "xxx") == 0)` block.

### Fixing a decode bug
The decoder in `src/toon_decode.c` works line-by-line. Key entry point is `toon_decode()` → `decode_object_at_depth()`. Array headers are parsed by `parse_array_header()`. Tabular rows use `decode_tabular_array()`, list items use `decode_list_array()`.

### Fixing an encode bug
The encoder in `src/toon_encode.c` dispatches on JsonValue type. Entry point is `toon_encode()` → `encode_value()`. Tabular detection is in `is_tabular_array()`. Object-as-list-item encoding is the most complex path.

### Adding float format support
`format_number()` in `src/toon_util.c` does manual double→string conversion because scnb.lib lacks `%g`. If you need more precision or handle edge cases, modify that function. Do NOT use sprintf with float format specifiers.
