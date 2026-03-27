# Cross-Compiling with SAS/C 6.58 on macOS via vamos

A practical guide for using the SAS/C 6.58 Amiga C compiler on modern
macOS, targeting AmigaOS 2.x+ (68020+).  Based on hard-won experience
porting QuickJS-ng to AmigaOS.

## Overview

SAS/C 6.58 is a native AmigaOS compiler.  It runs on 68k hardware (or
emulation).  On macOS, we use **vamos** — a lightweight AmigaOS
emulator from the amitools package — to execute SAS/C's `sc` compiler
and `slink` linker.  The compiled output is native 68k Amiga binaries.

```
macOS host                    vamos (AmigaOS emulation)
┌──────────┐                  ┌────────────────────┐
│ source   │ ──── vamos ────> │ sc (SAS/C compiler)│
│ files    │                  │ slink (linker)     │
│ (.c/.h)  │ <── .o files ── │                    │
└──────────┘                  └────────────────────┘
```

No actual Amiga hardware needed for compilation.  Testing on real
hardware (or Amiberry) is recommended for I/O, console, and timing.

## Prerequisites

### Install vamos

```sh
pip3 install amitools
# verify:
which vamos   # → /opt/homebrew/bin/vamos (or similar)
```

### Obtain SAS/C 6.58

You need a licensed copy of SAS/C 6.58 for AmigaOS.  The installation
should contain at minimum:

```
sasc658/
├── c/
│   ├── sc          ← the compiler
│   └── slink       ← the linker
├── include/
│   ├── assert.h
│   ├── stdio.h
│   ├── string.h
│   ├── exec/types.h
│   ├── proto/dos.h
│   └── ...
└── lib/
    ├── c.o         ← C startup code
    ├── scnb.lib    ← small/no-buffering C library
    ├── scm881nb.lib ← 68881 FPU math, no buffering
    ├── scmnb.lib   ← software float math, no buffering
    └── amiga.lib   ← AmigaOS library stubs
```

### Set up $SC

Point the `$SC` environment variable at your SAS/C installation:

```sh
export SC=/path/to/sasc658
```

This path is mapped as the `sc:` volume in vamos.

## Compiling a Single File

```sh
# Clear stale RAM: volume state
rm -rf ~/.vamos/volumes/ram

# Compile hello.c
vamos \
  -c vamos_build.cfg \
  -V sc:$SC \
  -V src:/path/to/sources \
  sc:c/sc src:hello.c \
  DATA=FARONLY NOSTACKCHECK NOCHKABORT \
  IDIR=src: IDIR=sc:include NOICONS
```

### Key Compiler Flags

| Flag | Purpose |
|------|---------|
| `DATA=FARONLY` | Use 32-bit data references (required for large programs) |
| `CODE=FAR` | Use 32-bit code references (for files > 32KB of code) |
| `MATH=68881` | Generate inline 68881 FPU instructions |
| `NOSTACKCHECK` | Disable stack checking (faster, required for deep recursion) |
| `NOCHKABORT` | Disable Ctrl-C checking in library calls |
| `ABSFP` | Absolute function pointers (required for cross-module calls) |
| `IDIR=path` | Add include search path |
| `NOICONS` | Don't create .info icon files |
| `CPU=68020` | Target CPU (also 68030, 68040, 68060) |
| `MEMSIZE=HUGE` | Large compiler memory (needed for complex headers) |
| `IDLEN=80` | Increase identifier length limit (default 31 chars) |

### The 31-Character Identifier Limit

SAS/C 6.58 truncates identifiers longer than 31 characters by default.
Two identifiers that share the first 31 characters become the same
symbol, causing silent linker errors.

**Solutions:**
1. Use `IDLEN=80` compiler flag (increases limit to 80)
2. Rename long identifiers with `#define`:
   ```c
   #define js_async_generator_resolve_function_create  js_asgen_resolve_func_create
   ```

### Object File Placement

SAS/C ignores `OBJDIR=` on vamos.  Object files (.o) are always placed
next to the source file.  Plan your build around this.

## Linking

```sh
rm -rf ~/.vamos/volumes/ram

vamos \
  -c vamos_build.cfg \
  -V sc:$SC \
  -V src:/path/to/sources \
  sc:c/slink \
  sc:lib/c.o \
  src:file1.o src:file2.o src:file3.o \
  TO src:output_binary \
  LIB sc:lib/scnb.lib sc:lib/scm881nb.lib sc:lib/amiga.lib \
  NOICONS
```

### Library Selection

| Library | Purpose |
|---------|---------|
| `c.o` | C startup code (always first) |
| `scnb.lib` | Small C library, no buffering |
| `scm881nb.lib` | 68881 FPU math, no buffering |
| `scmnb.lib` | Software float math, no buffering (no-FPU build) |
| `amiga.lib` | AmigaOS library call stubs |

**Important:** Do not mix .o files compiled WITH and WITHOUT `MATH=68881`
in the same link.  FPU-compiled objects emit 68881 opcodes that crash on
CPUs without an FPU.

### Link Warnings

- **Warning 622: Conflicting math types** — You've mixed FPU and non-FPU
  object files.  Recompile all sources consistently.
- **Warning 625: Proper math library not included** — Add `scm881nb.lib`
  (FPU) or `scmnb.lib` (software) to the LIB list.

## vamos Configuration

### Build Config (`vamos_build.cfg`)

```ini
[vamos]
cpu = 68020
memory = 32768
hw_access = disable
```

### Run Config

```sh
vamos -S -C 68040 -m 65536 -H disable -s 2048 \
  -V src:/path/to/sources \
  -- src:output_binary [args...]
```

| Flag | Purpose |
|------|---------|
| `-S` | Skip user vamos config |
| `-C 68040` | Emulate 68040 CPU |
| `-m 65536` | 64 MB memory (in KiB) |
| `-H disable` | Disable hardware access trapping |
| `-s 2048` | 2 MB stack (in KiB) |
| `-V name:path` | Map vamos volume name to host path |
| `--` | Stop vamos option parsing (needed before binary args) |

**Critical:** `-s` and `-m` are in **KiB**, not bytes.  `-s 65536` = 64 MB
stack, which will likely exceed available memory.

### Clearing RAM

Always clear the vamos RAM volume before each invocation:

```sh
rm -rf ~/.vamos/volumes/ram
```

Stale state from previous runs can cause cryptic failures.

## FPU vs No-FPU Builds

AmigaOS machines may or may not have an FPU (68881/68882, or integrated
in 68040/68060).  You typically want two binaries:

| Binary | Compile Flag | Math Library | Requires |
|--------|-------------|--------------|----------|
| `app` (FPU) | `MATH=68881` | `scm881nb.lib` | 68881+ FPU |
| `app_soft` (no-FPU) | *(none)* | `scmnb.lib` | 68020+ any |

Since .o files land next to .c files (same directory), building both
variants requires compiling all files twice.  Build soft first, link
soft, then rebuild FPU.

## CPU-Specific Builds

SAS/C supports CPU-specific instruction scheduling:

```sh
sc source.c CPU=68060 MATH=68881 DATA=FARONLY ...
```

This produces smaller, faster code for the target CPU.  The output
still runs on that CPU and above.

| CPU Flag | Target | Notes |
|----------|--------|-------|
| `CPU=68020` | 68020+ | Default, broadest compatibility |
| `CPU=68030` | 68030+ | Minimal gains |
| `CPU=68040` | 68040+ | Better scheduling |
| `CPU=68060` | 68060 | Best for 060 machines |

Use file suffixes to distinguish: `app.040`, `app.060`.

## Common Pitfalls

### 1. `int64_t` is 32-bit

SAS/C's `long long` is the same as `long` (32 bits).  This means:

- `int64_t` = `uint64_t` = 32-bit `long`
- `sizeof(int64_t)` = 4, not 8
- Shifts ≥ 32 bits are undefined behavior
- `(int64_t)1 << 53` = 0, not 9007199254740992
- `printf("%lld", x)` does not work

**Impact:** Any code that assumes 64-bit integers is silently wrong.
You must audit all int64_t usage and provide 32-bit alternatives.

### 2. `INT32_MIN` Definition Trap

```c
/* WRONG on SAS/C: */
#define INT32_MIN (-2147483648L)
/* 2147483648L exceeds LONG_MAX → becomes unsigned → negation wraps → positive! */

/* CORRECT: */
#define INT32_MIN (-2147483647L - 1L)
```

This subtle bug caused every integer addition in QuickJS to produce
float64 instead of int32.

### 3. printf %g/%e/%f Not Available in scnb.lib

The `scnb.lib` (no-buffering) C library does NOT support float printf
format specifiers.  `printf("%.17g", 3.14)` outputs the literal string
`%.17g` instead of the number.

**Solution:** Implement float-to-string conversion manually using FPU
arithmetic, or use the full `sc.lib` (with buffering).

### 4. `scmieee.lib` Crashes with DATA=FARONLY

The IEEE math library (`scmieee.lib`) accesses `MathIeeeDoubBasBase`
via 16-bit A4-relative offset.  With `DATA=FARONLY`, the data segment
can exceed ±32 KB from A4, causing the access to read NULL → crash.

**Solution:** Use `MATH=68881` (inline FPU opcodes) or implement math
functions in software.

### 5. CODE=FAR Required for Large Files

If a source file compiles to more than ~32 KB of code, the compiler
emits 16-bit BSR instructions that can't reach targets beyond 32 KB.

```
Function too far for PC-relative. Use AbsFunctionPointer option.
```

**Solution:** Add `CODE=FAR ABSFP` for large source files.

### 6. `bool` Type

SAS/C 6.58 is C89, which doesn't have `_Bool` or `<stdbool.h>`.
Provide your own:

```c
/* amiga/stdbool.h */
#ifndef _STDBOOL_H
#define _STDBOOL_H
typedef int bool;
#define true 1
#define false 0
#endif
```

### 7. No `<stdatomic.h>`

SAS/C has no atomics support.  Define `__STDC_NO_ATOMICS__=1` and
provide alternatives or disable atomic-dependent code.

### 8. `inline` Keyword

SAS/C uses `__inline` instead of C99 `inline`:

```c
#ifdef __SASC
#define inline __inline
#endif
```

### 9. vamos Limitations

vamos does not fully emulate all AmigaOS functions:

- **Console I/O:** No real console device.  `SetMode()`, `WaitForChar()`,
  `Write(Output(), ...)` in raw mode may not work as on real hardware.
- **FPU emulation:** Some 68881 instruction modes are not fully emulated.
  You may see `M68kFPU: READ_EA_64: unhandled mode` errors.
- **SystemTagList:** Not supported in vamos.  Process-launching functions
  need testing on real hardware or Amiberry.
- **Network:** No `bsdsocket.library` in vamos.

Always test I/O-dependent code on real hardware or Amiberry.

### 10. AmigaOS Path Conventions

AmigaOS uses `Volume:Directory/File` paths, not Unix `/dir/file`:

```
RAM:scripts/main.js     ← RAM disk
Work:projects/src/app.c ← hard drive
S:Startup-Sequence      ← system scripts
T:tempfile              ← temp directory
```

Code that assumes Unix paths (starts with `/`, uses `/` as separator)
needs adaptation.  `strrchr(path, '/')` should also check for `:`.

### 11. Header File Complexity

Some modern SDK headers (e.g., OpenSSL via AmiSSL) have macro
expansions that exceed SAS/C's preprocessor line buffer:

```
Error 6: line buffer overflow
```

**Solutions:**
- Use `MEMSIZE=HUGE IDLEN=80` compiler flags
- Define `AMISSL_NO_STATIC_FUNCTIONS` before including headers
- Use pragma files directly instead of full headers
- Forward-declare only the types/functions you need

## Build Automation

Create shell helper functions on the macOS side:

```sh
# In your build script:
SC=/path/to/sasc658
QJS=/path/to/sources

amiga_compile() {
    local file="$1"; shift
    rm -rf ~/.vamos/volumes/ram
    vamos -c vamos_build.cfg \
      -V sc:$SC -V src:$QJS \
      sc:c/sc "src:$file" \
      MATH=68881 DATA=FARONLY NOSTACKCHECK NOCHKABORT ABSFP \
      IDIR=src: IDIR=sc:include NOICONS "$@"
}

amiga_link() {
    rm -rf ~/.vamos/volumes/ram
    vamos -c vamos_build.cfg \
      -V sc:$SC -V src:$QJS \
      sc:c/slink sc:lib/c.o \
      src:file1.o src:file2.o \
      TO src:bin/output \
      LIB sc:lib/scnb.lib sc:lib/scm881nb.lib sc:lib/amiga.lib NOICONS
}

amiga_run() {
    rm -rf ~/.vamos/volumes/ram
    vamos -S -C 68040 -m 65536 -H disable -s 2048 \
      -V src:$QJS -- src:bin/output "$@"
}
```

## AmigaOS Version String

Add a `$VER:` string so the AmigaDOS `version` command can identify
your binary:

```c
#ifdef __SASC
static const char amiga_ver[] = "$VER: MyApp 1.0 (25.3.2026)";
#endif
```

Format: `$VER: name major.minor (day.month.year)`

The string must appear in the binary's data segment.  `version MyApp`
at the AmigaDOS shell will find and display it.

## Using AmigaOS Shared Libraries

AmigaOS uses shared libraries opened at runtime, not linked statically:

```c
#include <proto/exec.h>
#include <proto/dos.h>

struct Library *MyBase = NULL;

/* Open at runtime */
MyBase = OpenLibrary("my.library", 37);
if (!MyBase) {
    /* handle error — library not installed */
}

/* Use library functions (dispatched via pragma/inline) */
MyFunction(args);

/* Close on exit */
CloseLibrary(MyBase);
```

The `<proto/xxx.h>` headers provide both the function prototypes AND
the pragma/inline dispatch for SAS/C.  No `.lib` file needed for the
library itself — the calls go through the library's jump table at
runtime.

## Debugging Tips

- **Assertion failures:** SAS/C's `assert()` calls `abort()` which
  prints "Abnormal program termination" and exits.
- **Guru Meditation / hard lock:** Usually a NULL pointer dereference
  or stack overflow.  Try increasing the stack with the AmigaDOS
  `stack` command before running your binary.
- **vamos vs real hardware:** Many bugs only manifest on real hardware.
  Build a test suite that runs in both environments.
- **printf debugging:** Use `FPrintf(Output(), ...)` instead of
  `printf()` if you suspect stdio buffering issues.
