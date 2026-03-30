# nea-toon

A native AmigaOS implementation of the [TOON](https://github.com/toon-format/spec) (Token-Oriented Object Notation) format. Written in C89 for SAS/C 6.58, targeting 68020+ Amigas.

TOON is a line-oriented, indentation-based text format that encodes the JSON data model with explicit structure and minimal quoting. It is particularly compact for tabular data:

```
users[3]{id,name,role,active}:
  1,Alice,admin,true
  2,Bob,developer,true
  3,Charlie,designer,false
```

This is equivalent to a JSON array of three objects with `id`, `name`, `role`, and `active` fields — but without repeating keys on every row.

## Features

- **Encode** JSON to TOON
- **Decode** TOON to JSON
- **Get** values by path (`server.host`, `users[0].name`, `users.0.name`)
- **Set** values by path with automatic deep-creation of intermediate objects
- **Delete** keys or array elements by path
- Tabular, inline, and list array formats
- Comma, tab, and pipe delimiters
- Strict and lenient decoding modes
- Canonical number formatting without FPU printf dependency

## Quick Start

### On Amiga

Copy the `toon-port/` directory to your Amiga. With SAS/C 6.58 installed and `SC:` assigned:

```
cd toon-port
smake
```

### Cross-Compiling with vamos

```sh
git clone --recursive https://github.com/nyteshade/nea-toon.git
cd nea-toon
export SC=/path/to/sasc658

# Generate toon-port/ + compile all CPU variants
python3 build_port.py --build --with-tests

# Run a specific variant
vamos -S -C 68020 -m 8192 -H emu -s 512 \
  -V sc:$SC -V src:toon-port -- src:toon.020 encode src:examples/users.json
```

Requires [amitools/vamos](https://github.com/cnvogelg/amitools) (`pip3 install amitools`) and a licensed copy of SAS/C 6.58.

### CPU-Specific Binaries

Each build is optimized for its target CPU:

| Binary | Target | Optimization |
|--------|--------|-------------|
| `toon.000` | 68000 | Size-optimized (runs on any Amiga) |
| `toon.020` | 68020+ | Speed-optimized with inlining |
| `toon.040` | 68040+ | Speed + instruction scheduling |
| `toon.060` | 68060 | Speed + 060 instruction scheduling |

Copy the appropriate binary to `C:` and rename:
```
copy toon.020 C:toon
```

## Usage

```
toon encode [opts] [file.json]         JSON to TOON
toon decode [opts] [file.toon]         TOON to JSON
toon get [opts] <file.toon> <path>     Read a value
toon set [opts] <file.toon> <path> <value>   Write a value
toon del [opts] <file.toon> <path>     Delete a value
```

### Examples

```sh
# Convert JSON to TOON
toon encode data.json -o data.toon

# Convert TOON to JSON
toon decode data.toon

# Read nested values
toon get config.toon server.host           # → localhost
toon get config.toon database.pool         # → TOON output (default)
toon get -j config.toon database.pool      # → JSON output

# Write values (creates intermediate objects automatically)
toon set config.toon cache.redis.host redis.local

# Delete values
toon del config.toon logging.format        # Remove a key
toon del users.toon users[1]               # Remove array element

# Pipe from stdin
echo '{"name":"Alice","age":30}' | toon encode
```

### Path Syntax

Paths use JavaScript-style dot/bracket notation, with a shell-friendly alternative for array indices:

| Syntax | Meaning |
|--------|---------|
| `person.name` | Object property |
| `users[0].name` | Array index + property |
| `users.0.name` | Same, without brackets |
| `["my-key"].val` | Quoted key for special characters |
| `data["x.y"]` | Literal dotted key (no path splitting) |

### Options

| Flag | Description |
|------|-------------|
| `-i <n>` | Indent size (default 2) |
| `-d <delim>` | Delimiter: `comma`, `tab`, or `pipe` |
| `-s` | Strict mode (default for decode) |
| `-l` | Lenient mode |
| `-o <file>` | Output to file |
| `-j` | Output as JSON (get command) |
| `-t` | Output as TOON (get command, default for complex values) |

## Project Structure

```
src/                   C source files (edit these)
  toon.h               Header, data structures, API
  main.c               CLI: encode, decode, get, set, del
  toon_decode.c        TOON → JSON decoder
  toon_encode.c        JSON → TOON encoder
  json_parse.c         JSON parser
  json_emit.c          JSON emitter
  toon_util.c          Utilities, string buffer, number formatting

build_port.py          Generates toon-port/ from src/ + toon repos
toon-port/             Ready-to-build Amiga directory (generated)
  SMakefile            SAS/C build rules
  SCoptions            Compiler flags
  .vamosrc             vamos cross-compilation config
  examples/            Sample .toon and .json files
  test_runner.c        Auto-generated spec conformance tests (with --with-tests)

toon/                  Upstream toon-format/toon repo (git submodule)
CLAUDE.md              Development guide for AI-assisted maintenance
SAS-C-GUIDE.md         SAS/C 6.58 cross-compilation reference
```

## Build System

The `build_port.py` script handles everything from source preparation to release packaging:

```sh
# Generate toon-port/ directory only (for manual Amiga build)
python3 build_port.py --clean --with-tests

# Generate + cross-compile all four CPU variants via vamos
python3 build_port.py --build --with-tests

# Full release: clean build, compile, package .tar.gz for GitHub
python3 build_port.py --release --with-tests

# With explicit spec repo and verbose C89 checks
python3 build_port.py --release --with-tests --spec-repo /path/to/spec -v
```

The `--release` flag produces a `releases/toon-X.Y-amiga.tar.gz` archive containing CLI binaries, toon.library, SDK, examples, and a README. Upload directly to GitHub Releases:

```sh
gh release create v1.5 releases/toon-1.5-amiga.tar.gz \
  --title 'toon 1.5 for AmigaOS'
```

On a real Amiga, `smake all` builds all four CPU variants from source:

```
smake              Build default (68020)
smake all          Build toon.000, toon.020, toon.040, toon.060
smake toon.000     Build 68000 variant only
smake clean        Remove all build artifacts
smake test         Build and run conformance tests
```

## TOON Format Quick Reference

### Objects

```
server:
  host: localhost
  port: 8080
```

### Primitive Arrays

```
tags[3]: admin,ops,dev
empty[0]:
```

### Tabular Arrays (uniform objects)

```
users[2]{id,name,active}:
  1,Alice,true
  2,Bob,false
```

### List Arrays (mixed types)

```
items[3]:
  - hello
  - 42
  - true
```

### Delimiters

```
data[2|]{name|value}:
  Widget|9.99
  Gadget|14.5
```

### Quoting

Strings are unquoted unless they contain special characters, look numeric, or match reserved words:

```
name: Alice
zip: "10001"
note: "value with: colon"
```

For the full specification, see the [TOON Spec](https://github.com/toon-format/spec/blob/main/SPEC.md).

## Spec Compliance

Implements TOON Specification v3.0:

- [x] Objects with indentation-based nesting
- [x] Primitive arrays (inline)
- [x] Tabular arrays (uniform objects with field headers)
- [x] List arrays (mixed/non-uniform elements)
- [x] Objects as list items (first field on hyphen line)
- [x] Comma, tab, and pipe delimiters with scoped quoting
- [x] String quoting rules (section 7.2)
- [x] Canonical number formatting (no exponent, no trailing zeros)
- [x] Strict and lenient decoding modes
- [x] Root form discovery (object, array, or primitive)
- [ ] Key folding (encode-side, optional per spec)
- [ ] Path expansion (decode-side, optional per spec)
- [ ] Streaming decode

## SAS/C Notes

This codebase is written for SAS/C 6.58, a C89 compiler for AmigaOS. Key constraints:

- No 64-bit integer types (`long long` is 32-bit on SAS/C)
- No `%g`/`%e`/`%f` in `scnb.lib` printf — float formatting is done manually via FPU arithmetic
- `inline` is mapped to `__inline` via preprocessor guard
- Identifiers up to 80 characters (`IDLEN=80`)

See [SAS-C-GUIDE.md](SAS-C-GUIDE.md) for the full cross-compilation reference.

## License

MIT License. Copyright (c) 2026 Brielle Harrison (nyteshade).

See [LICENSE](LICENSE) for details.

## Links

- [This project](https://github.com/nyteshade/nea-toon)
- [TOON Specification](https://github.com/toon-format/spec)
- [TOON Reference Implementation (TypeScript)](https://github.com/toon-format/toon)
- [amitools/vamos](https://github.com/cnvogelg/amitools) — AmigaOS emulator for cross-compilation
