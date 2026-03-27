# toon - TOON Format CLI for AmigaOS

A native AmigaOS command-line tool for working with [TOON](https://github.com/toon-format/spec) (Token-Oriented Object Notation) files. Implements the TOON Specification v3.0.

TOON is a line-oriented, indentation-based text format that encodes the JSON data model with explicit structure and minimal quoting. It is particularly compact for arrays of uniform objects.

```
users[3]{id,name,role,active}:
  1,Alice,admin,true
  2,Bob,developer,true
  3,Charlie,designer,false
```

## Requirements

- AmigaOS 2.x or later
- SAS/C 6.58 with `SC:` assign configured
- 68020 or better CPU

## Building on Amiga

Extract the archive, enter the directory, and type:

```
smake
```

This produces the `toon` executable. The `SCoptions` file provides compiler flags and `INCLUDE:` is expected to point to SAS/C's include directory (standard SAS/C setup).

To clean build artifacts:

```
smake clean
```

### Cross-Compiling with vamos (macOS/Linux)

You can also build on a modern system using [vamos](https://github.com/cnvogelg/amitools) (a lightweight AmigaOS API emulator). A `.vamosrc` is included that sets up the necessary assigns and enables `icon.library` in fake mode (required for smake under vamos).

```sh
pip3 install amitools
export SC=/path/to/sasc658

vamos -V sc:$SC -V src:$(pwd) --cwd src: -c .vamosrc sc:c/smake
```

To run the built binary:

```sh
vamos -S -C 68020 -m 8192 -H emu -s 512 \
  -V sc:$SC -V src:$(pwd) -- src:toon [command] [args...]
```

**vamos limitations:** vamos emulates AmigaOS at the API level, not hardware. It supports exec, dos, utility, and math libraries. It does not support graphics, intuition (beyond alerts), icon.library (faked for smake), or network libraries. Console I/O and FPU edge cases should be tested on real hardware or a full emulator like Amiberry.

## Usage

### Convert JSON to TOON

```
toon encode config.json
toon encode config.json -o config.toon
```

### Convert TOON to JSON

```
toon decode config.toon
toon decode config.toon -o config.json
```

### Read a value

```
toon get config.toon server.host
> localhost

toon get config.toon server.port
> 8080

toon get config.toon database.connection
> host: db.example.com
> port: 5432
> username: admin
> database: myapp_prod
```

### Write a value

```
toon set config.toon server.port 9090
toon set config.toon cache.redis.host redis.local
```

Setting a value that doesn't exist creates the full path automatically.

### Delete a value

```
toon del config.toon database.pool
toon del users.toon users[1]
```

Deleting an array element removes it and shifts remaining elements.

### Pipe from stdin

```
echo '{"name":"Alice","age":30}' | toon encode
type data.toon | toon decode
```

## Path Syntax

Paths use JavaScript-style property access:

| Syntax | Meaning |
|--------|---------|
| `person.name` | Object property |
| `users[0].name` | Array index + property |
| `users.0.name` | Same, without brackets (shell-friendly) |
| `["my-key"].val` | Quoted key for special characters |
| `data["x.y"]` | Literal dotted key (no path splitting) |

Numeric segments like `.0` are smart: they index into arrays, but fall back to key lookup on objects.

## Options

| Flag | Description |
|------|-------------|
| `-i <n>` | Indent size (default 2) |
| `-d <delim>` | Delimiter: `comma`, `tab`, or `pipe` |
| `-s` | Strict mode (default for decode) |
| `-l` | Lenient mode |
| `-o <file>` | Output to file instead of stdout |
| `-j` | Output as JSON (get command) |
| `-t` | Output as TOON (get command, default for complex values) |

## TOON Format Quick Reference

### Objects

Indentation-based, like YAML:

```
server:
  host: localhost
  port: 8080
```

### Primitive Arrays

Inline with declared length:

```
tags[3]: admin,ops,dev
empty[0]:
```

### Tabular Arrays

Uniform objects with field headers:

```
users[2]{id,name,active}:
  1,Alice,true
  2,Bob,false
```

### List Arrays

Mixed or non-uniform elements:

```
items[3]:
  - hello
  - 42
  - true
```

### Delimiters

Comma (default), tab, or pipe:

```
data[2|]{name|value}:
  Widget|9.99
  Gadget|14.5
```

### String Quoting

Strings are quoted only when necessary (contains `:`, delimiter, looks numeric, etc.):

```
name: Alice
zip: "10001"
note: "contains: colon"
```

## Examples

The `examples/` directory contains sample TOON and JSON files:

- `users.toon` / `users.json` - Tabular array of user records
- `config.toon` / `config.json` - Nested configuration object
- `mixed.toon` - Mixed types: objects, arrays, primitives
- `products.toon` - Tabular product inventory

## Files

```
SMakefile       Build rules for smake
SCoptions       SAS/C compiler options
.vamosrc        vamos config for cross-compilation (not needed on Amiga)
toon.h          Header - data structures and API
main.c          CLI interface, path parser, get/set/del
toon_decode.c   TOON to JSON decoder
toon_encode.c   JSON to TOON encoder
json_parse.c    JSON text parser
json_emit.c     JSON text emitter
toon_util.c     Shared utilities, string buffer, number formatting
examples/       Sample TOON and JSON files
```

## Spec Compliance

Implements TOON Specification v3.0 with the following coverage:

- Objects with indentation-based nesting
- Primitive arrays (inline)
- Tabular arrays (uniform objects with field headers)
- List arrays (mixed/non-uniform elements)
- Objects as list items
- Comma, tab, and pipe delimiters
- String quoting rules per spec section 7.2
- Canonical number formatting (no exponent, no trailing zeros)
- Strict and lenient decoding modes
- Root form discovery (object, array, or primitive)

Not implemented (optional per spec):
- Key folding (encode) and path expansion (decode)
- Streaming decode

## License

MIT

## Links

- [TOON Specification](https://github.com/toon-format/spec)
- [TOON Reference Implementation (TypeScript)](https://github.com/toon-format/toon)
