#!/usr/bin/env python3
"""
build_port.py - Build the Amiga TOON CLI port from the toon reference repo.

This script takes a fresh checkout of the toon-format/toon repo (and optionally
the toon-format/spec repo) and produces a toon-port/ directory ready to compile
on an Amiga with SAS/C 6.58 (or cross-compile via vamos).

Usage:
    python3 build_port.py [options]

    # Generate toon-port/ directory only (for manual Amiga build)
    python3 build_port.py --clean --with-tests

    # Generate + cross-compile all CPU variants via vamos
    python3 build_port.py --build

    # Full release: generate, compile 68000/020/040/060, package archive
    python3 build_port.py --release --with-tests

The script:
    1. Reads the current spec version from the toon/spec repos
    2. Copies our C89/SAS/C compatible source files
    3. Applies SAS/C compatibility fixups to the sources
    4. Generates SMakefile, SCoptions, .vamosrc
    5. Copies examples from the toon repo
    6. Optionally generates a C test runner from spec test fixtures
    7. Generates README.md with current spec version
    8. Validates C89 compatibility of all sources
    9. (--build) Cross-compiles toon.000/.020/.040/.060 via vamos
   10. (--release) Packages binaries + sources + examples into a .tar.gz

== INSTRUCTIONS FOR FUTURE MAINTENANCE ==

This Amiga port is a CLEAN-ROOM C89 IMPLEMENTATION of the TOON spec.
It is NOT a transpilation of the TypeScript reference implementation.
The C sources in src/ are maintained independently.

When the TOON spec changes:
    1. Read the new SPEC.md for changes
    2. Update the relevant C source files in src/
    3. Run this script to rebuild toon-port/
    4. Test with: vamos ... sc:c/smake  (or on real Amiga hardware)

SAS/C 6.58 constraints to remember:
    - C89 only: no //, no mixed declarations, no VLAs, no designated inits
    - int/long are 32-bit; no 64-bit integer type
    - No %g/%e/%f in scnb.lib printf - use manual float formatting
    - No <stdbool.h>, <stdint.h>, <stdatomic.h>
    - __inline instead of inline (guarded by __SASC)
    - IDLEN=80 needed for identifiers > 31 chars
    - DATA=FARONLY breaks pure/resident - omit for residentable binaries
    - Link with cres.o (not c.o) for pure/resident programs
    - scnb.lib has absolute refs - not fully pure-compatible (see README)
"""

import argparse
import json
import os
import re
import shutil
import sys
import textwrap
from datetime import datetime
from pathlib import Path


# ---------------------------------------------------------------------------
# App version handling
# ---------------------------------------------------------------------------

def get_app_version(src_dir):
    """Extract current app version (e.g. '1.4') from src/toon.h $VER string."""
    toon_h = os.path.join(src_dir, 'toon.h')
    if not os.path.exists(toon_h):
        return None
    with open(toon_h) as f:
        for line in f:
            m = re.search(r'\$VER: toon (\d+\.\d+)', line)
            if m:
                return m.group(1)
    return None


def parse_app_version(ver_str):
    """Parse 'X.Y' into (major, minor) integers."""
    parts = ver_str.split('.')
    if len(parts) != 2:
        raise ValueError(f"Version must be X.Y, got: {ver_str}")
    return int(parts[0]), int(parts[1])


def bump_version(src_dir, new_version):
    """Update all version strings across src/ to new_version (e.g. '1.5').

    Files updated:
      src/toon.h          - $VER string
      src/main.c          - help text version
      src/include/libraries/toon.h - TOON_VERSION / TOON_REVISION defines
      src/include/fd/toon_lib.fd   - comment version
      src/lib/SMakefile    - LIBVERSION / LIBREVISION
      src/Install          - installer dialog text + version check
    """
    major, minor = parse_app_version(new_version)
    today = datetime.now()
    date_str = f"{today.day}.{today.month}.{today.year}"

    replacements = {
        # src/toon.h: $VER string
        'toon.h': [
            (r'\$VER: toon \d+\.\d+ \(\d+\.\d+\.\d+\)',
             f'$VER: toon {new_version} ({date_str})'),
        ],
        # src/main.c: help text
        'main.c': [
            (r'CLI v\d+\.\d+', f'CLI v{new_version}'),
        ],
        # src/include/libraries/toon.h: defines
        os.path.join('include', 'libraries', 'toon.h'): [
            (r'#define TOON_VERSION\s+\d+', f'#define TOON_VERSION    {major}'),
            (r'#define TOON_REVISION\s+\d+', f'#define TOON_REVISION   {minor}'),
        ],
        # src/include/fd/toon_lib.fd: comment
        os.path.join('include', 'fd', 'toon_lib.fd'): [
            (r'library v\d+\.\d+', f'library v{new_version}'),
        ],
        # src/lib/SMakefile: LIBVERSION / LIBREVISION
        os.path.join('lib', 'SMakefile'): [
            (r'LIBVERSION \d+ LIBREVISION \d+',
             f'LIBVERSION {major} LIBREVISION {minor}'),
        ],
        # src/Install: dialog text + version check
        'Install': [
            (r'TOON v\d+\.\d+', f'TOON v{new_version}'),
            (r'Version >NIL: LIBS:toon\.library \d+ \d+',
             f'Version >NIL: LIBS:toon.library {major} {minor}'),
            (r'toon\.library \d+\.\d+\+',
             f'toon.library {new_version}+'),
        ],
    }

    updated = []
    for rel_path, patterns in replacements.items():
        filepath = os.path.join(src_dir, rel_path)
        if not os.path.exists(filepath):
            continue
        with open(filepath, 'r') as f:
            content = f.read()
        original = content
        for pattern, replacement in patterns:
            content = re.sub(pattern, replacement, content)
        if content != original:
            with open(filepath, 'w', newline='\n') as f:
                f.write(content)
            updated.append(rel_path)

    return updated


# ---------------------------------------------------------------------------
# C89 / SAS/C compatibility checks and fixups
# ---------------------------------------------------------------------------

C89_ISSUES = {
    "c++_comment": (
        re.compile(r'(?<!["\':])//(?!["\'])'),
        "C++ style // comment (use /* */ instead)"
    ),
    "mixed_decl": (
        # Heuristic: statement followed by declaration
        # This is imperfect but catches common cases
        None,
        "Mixed declarations and code (C89 requires declarations at block start)"
    ),
    "stdbool": (
        re.compile(r'#include\s*<stdbool\.h>'),
        "<stdbool.h> not available in SAS/C"
    ),
    "stdint": (
        re.compile(r'#include\s*<stdint\.h>'),
        "<stdint.h> not available in SAS/C"
    ),
    "inline_kw": (
        # Skip #define inline and __inline
        re.compile(r'(?<!#define\s)(?<!_)(?<!\w)inline\s+(?!__inline)'),
        "bare 'inline' keyword (SAS/C uses __inline, guard with #ifdef __SASC)"
    ),
    "long_long": (
        re.compile(r'\blong\s+long\b'),
        "long long is 32-bit in SAS/C (same as long)"
    ),
    "int64": (
        re.compile(r'\b(?:u?int64_t|INT64_MIN|INT64_MAX|UINT64_MAX)\b'),
        "64-bit integer types are 32-bit in SAS/C"
    ),
    "designated_init": (
        # Match ".field = value" at start of expression (inside braces), not "obj.field = value"
        re.compile(r'(?<![A-Za-z0-9_)\]])\.\w+\s*='),
        "Designated initializer (C99, not C89)"
    ),
    "vla": (
        # Very rough heuristic for VLAs - skip static/const declarations
        re.compile(r'(?<!static\s)(?<!const\s)\b(?:int|char|long|double|float|void)\s+\w+\[(?![0-9"\')\]])'),
        "Possible VLA (not supported in C89)"
    ),
}

# Fixups applied to source files
FIXUPS = [
    # Replace bare inline with __inline under __SASC guard
    # (only if not already guarded)
    # This is handled by the #define in toon.h, so no source fixup needed

    # Ensure LF line endings (no CR)
    ("line_endings", lambda content: content.replace('\r\n', '\n').replace('\r', '\n')),
]


def check_c89_compat(filepath, content, verbose=False):
    """Check a C source file for C89/SAS/C compatibility issues."""
    issues = []
    lines = content.split('\n')

    for lineno, line in enumerate(lines, 1):
        # Skip preprocessor lines and string literals for some checks
        stripped = line.strip()
        if not stripped or stripped.startswith('#'):
            continue

        for name, (pattern, message) in C89_ISSUES.items():
            if pattern is None:
                continue
            if pattern.search(line):
                # Avoid false positives in comments and strings
                if name == "c++_comment":
                    # Check if // is inside a string or after /*
                    in_string = False
                    in_block = False
                    for i, ch in enumerate(line):
                        if ch == '"' and (i == 0 or line[i-1] != '\\'):
                            in_string = not in_string
                        if not in_string and line[i:i+2] == '/*':
                            in_block = True
                        if not in_string and in_block and line[i:i+2] == '*/':
                            in_block = False
                        if not in_string and not in_block and line[i:i+2] == '//':
                            issues.append((lineno, name, message))
                            break
                elif name == "designated_init":
                    # Skip if inside a string or comment
                    if '"' not in line[:line.find('.')] or stripped.startswith('.'):
                        if not any(x in stripped for x in ['strcmp', 'strchr', 'strlen', 'sprintf']):
                            issues.append((lineno, name, message))
                elif name == "vla":
                    # Rough check - skip if inside a sizeof or has a constant
                    if 'sizeof' not in line and 'malloc' not in line:
                        issues.append((lineno, name, message))
                else:
                    issues.append((lineno, name, message))

    return issues


def apply_fixups(content):
    """Apply SAS/C compatibility fixups to source content."""
    for name, fixup in FIXUPS:
        content = fixup(content)
    return content


# ---------------------------------------------------------------------------
# Spec version detection
# ---------------------------------------------------------------------------

def detect_spec_version(toon_repo, spec_repo=None):
    """Detect the TOON spec version from available repos."""
    # Try spec repo first
    if spec_repo:
        spec_md = os.path.join(spec_repo, 'SPEC.md')
        if os.path.exists(spec_md):
            with open(spec_md, 'r') as f:
                for line in f:
                    m = re.match(r'\*\*Version:\*\*\s*(.+)', line)
                    if m:
                        return m.group(1).strip()

    # Try toon repo SPEC.md
    spec_md = os.path.join(toon_repo, 'SPEC.md')
    if os.path.exists(spec_md):
        with open(spec_md, 'r') as f:
            content = f.read()
            m = re.search(r'Version\s+(\d+\.\d+)', content)
            if m:
                return m.group(1)

    # Try package.json
    pkg_json = os.path.join(toon_repo, 'packages', 'toon', 'package.json')
    if os.path.exists(pkg_json):
        with open(pkg_json, 'r') as f:
            pkg = json.load(f)
            return pkg.get('version', 'unknown')

    return 'unknown'


# ---------------------------------------------------------------------------
# Example file collection
# ---------------------------------------------------------------------------

def collect_examples(toon_repo, spec_repo=None):
    """Collect example files from the toon/spec repos."""
    examples = {}

    # From spec repo examples/
    if spec_repo:
        for subdir in ['conversions', 'valid']:
            dirpath = os.path.join(spec_repo, 'examples', subdir)
            if os.path.isdir(dirpath):
                for fname in os.listdir(dirpath):
                    fpath = os.path.join(dirpath, fname)
                    if os.path.isfile(fpath) and (fname.endswith('.toon') or fname.endswith('.json')):
                        with open(fpath, 'r') as f:
                            examples[fname] = f.read()

    # From toon repo docs examples
    docs_dir = os.path.join(toon_repo, 'docs')
    if os.path.isdir(docs_dir):
        for root, dirs, files in os.walk(docs_dir):
            for fname in files:
                if fname.endswith('.toon') or fname.endswith('.json'):
                    fpath = os.path.join(root, fname)
                    if fname not in examples:
                        with open(fpath, 'r') as f:
                            examples[fname] = f.read()

    return examples


# ---------------------------------------------------------------------------
# Test fixture collection and C test runner generation
# ---------------------------------------------------------------------------

def collect_test_fixtures(spec_repo):
    """Collect test fixtures from the spec repo."""
    fixtures = {}
    if not spec_repo:
        return fixtures

    for category in ['decode', 'encode']:
        fixtures_dir = os.path.join(spec_repo, 'tests', 'fixtures', category)
        if not os.path.isdir(fixtures_dir):
            continue
        for fname in sorted(os.listdir(fixtures_dir)):
            if fname.endswith('.json'):
                fpath = os.path.join(fixtures_dir, fname)
                with open(fpath, 'r') as f:
                    try:
                        data = json.load(f)
                        fixtures[f"{category}/{fname}"] = data
                    except json.JSONDecodeError:
                        pass

    return fixtures


def escape_c_string(s):
    """Escape a string for C source code."""
    result = []
    for ch in s:
        if ch == '\\':
            result.append('\\\\')
        elif ch == '"':
            result.append('\\"')
        elif ch == '\n':
            result.append('\\n')
        elif ch == '\r':
            result.append('\\r')
        elif ch == '\t':
            result.append('\\t')
        elif ord(ch) < 0x20:
            result.append(f'\\x{ord(ch):02x}')
        elif ord(ch) > 0x7e:
            # Pass UTF-8 bytes through as-is for Amiga
            for byte in ch.encode('utf-8'):
                if byte > 0x7e:
                    result.append(f'\\x{byte:02x}')
                else:
                    result.append(chr(byte))
        else:
            result.append(ch)
    return ''.join(result)


def _generate_test_cases(fixtures):
    """Generate the shared test case call lines from fixtures.
       Returns (lines, test_count)."""
    lines = []
    test_count = 0

    for fixture_name, fixture in sorted(fixtures.items()):
        category = fixture_name.split('/')[0]
        tests = fixture.get('tests', [])
        for test in tests:
            name = test.get('name', f'test_{test_count}')
            should_error = test.get('shouldError', False)

            if category == 'decode' and not should_error:
                input_str = test.get('input', '')
                expected = test.get('expected')
                if expected is None:
                    continue
                expected_json = json.dumps(expected, separators=(',', ':'))

                if any(ord(c) > 127 for c in input_str + expected_json):
                    continue

                opts = test.get('options', {})
                opt_indent = opts.get('indent', 2)
                opt_strict = 1 if opts.get('strict', False) else 0

                c_input = escape_c_string(input_str)
                c_expected = escape_c_string(expected_json)
                c_name = escape_c_string(name)
                lines.append(f'    check_decode("{c_name}",')
                lines.append(f'        "{c_input}",')
                lines.append(f'        "{c_expected}", {opt_indent}, {opt_strict});')
                test_count += 1

            elif category == 'encode' and not should_error:
                input_val = test.get('input')
                expected = test.get('expected', '')
                if input_val is None:
                    continue

                input_json = json.dumps(input_val, separators=(',', ':'))
                if any(ord(c) > 127 for c in input_json + expected):
                    continue

                opts = test.get('options', {})
                opt_indent = opts.get('indent', 2)
                delim_str = opts.get('delimiter', ',')
                if delim_str == '\t':
                    opt_delim = 1
                elif delim_str == '|':
                    opt_delim = 2
                else:
                    opt_delim = 0

                c_input = escape_c_string(input_json)
                c_expected = escape_c_string(expected)
                c_name = escape_c_string(name)
                lines.append(f'    check_encode("{c_name}",')
                lines.append(f'        "{c_input}",')
                lines.append(f'        "{c_expected}", {opt_indent}, {opt_delim});')
                test_count += 1

    return lines, test_count


def generate_test_runner(fixtures):
    """Generate test_runner.c — links directly against toon .o files."""
    test_lines, test_count = _generate_test_cases(fixtures)

    lines = []
    lines.append('/*')
    lines.append(' * test_runner.c - Auto-generated conformance test runner (CLI)')
    lines.append(' * Generated by build_port.py from toon-format/spec test fixtures')
    lines.append(' * Links directly against toon object files.')
    lines.append(' */')
    lines.append('')
    lines.append('#include "toon.h"')
    lines.append('#include <string.h>')
    lines.append('')
    lines.append('static int tests_run = 0;')
    lines.append('static int tests_passed = 0;')
    lines.append('static int tests_failed = 0;')
    lines.append('')
    lines.append('static void check_decode(const char *name, const char *input,')
    lines.append('                         const char *expected_json, int indent, int strict)')
    lines.append('{')
    lines.append('    ToonDecodeOpts opts;')
    lines.append('    const char *err = NULL;')
    lines.append('    JsonValue *result;')
    lines.append('    char *json;')
    lines.append('    opts.indent = indent;')
    lines.append('    opts.strict = strict;')
    lines.append('    tests_run++;')
    lines.append('    result = toon_decode(input, &opts, &err);')
    lines.append('    if (!result) {')
    lines.append('        if (expected_json == NULL) {')
    lines.append('            tests_passed++;')
    lines.append('            return;')
    lines.append('        }')
    lines.append('        printf("FAIL %s: decode error: %s\\n", name, err ? err : "unknown");')
    lines.append('        tests_failed++;')
    lines.append('        return;')
    lines.append('    }')
    lines.append('    json = json_emit(result);')
    lines.append('    if (strcmp(json, expected_json) == 0) {')
    lines.append('        tests_passed++;')
    lines.append('    } else {')
    lines.append('        printf("FAIL %s\\n  expected: %s\\n  got:      %s\\n", name, expected_json, json);')
    lines.append('        tests_failed++;')
    lines.append('    }')
    lines.append('    free(json);')
    lines.append('    json_free(result);')
    lines.append('}')
    lines.append('')
    lines.append('static void check_encode(const char *name, const char *input_json,')
    lines.append('                         const char *expected_toon, int indent, int delim)')
    lines.append('{')
    lines.append('    const char *err = NULL;')
    lines.append('    JsonValue *val;')
    lines.append('    ToonEncodeOpts opts;')
    lines.append('    char *toon;')
    lines.append('    tests_run++;')
    lines.append('    val = json_parse(input_json, &err);')
    lines.append('    if (!val) {')
    lines.append('        printf("FAIL %s: json parse error: %s\\n", name, err ? err : "unknown");')
    lines.append('        tests_failed++;')
    lines.append('        return;')
    lines.append('    }')
    lines.append('    opts.indent = indent;')
    lines.append('    opts.delim = (ToonDelimiter)delim;')
    lines.append('    toon = toon_encode(val, &opts);')
    lines.append('    if (strcmp(toon, expected_toon) == 0) {')
    lines.append('        tests_passed++;')
    lines.append('    } else {')
    lines.append('        printf("FAIL %s\\n  expected: %s\\n  got:      %s\\n", name, expected_toon, toon);')
    lines.append('        tests_failed++;')
    lines.append('    }')
    lines.append('    free(toon);')
    lines.append('    json_free(val);')
    lines.append('}')
    lines.append('')
    lines.append('int main(void)')
    lines.append('{')

    lines.extend(test_lines)

    lines.append('')
    lines.append('    printf("\\ntest_runner: %d tests: %d passed, %d failed\\n",')
    lines.append('           tests_run, tests_passed, tests_failed);')
    lines.append('    return tests_failed > 0 ? 1 : 0;')
    lines.append('}')

    return '\n'.join(lines) + '\n'


def generate_test_library(fixtures):
    """Generate test_library.c — calls through toon.library via pragmas.
       Runs the same spec conformance tests as test_runner, plus
       library-specific tests for path ops, append, and JsonValue API."""
    test_lines, test_count = _generate_test_cases(fixtures)

    lines = []
    lines.append('/*')
    lines.append(' * test_library.c - Auto-generated conformance test for toon.library')
    lines.append(' * Generated by build_port.py from toon-format/spec test fixtures')
    lines.append(' * Calls through toon.library via pragma dispatch.')
    lines.append(' */')
    lines.append('')
    lines.append('#include <stdio.h>')
    lines.append('#include <stdlib.h>')
    lines.append('#include <string.h>')
    lines.append('#include <proto/exec.h>')
    lines.append('#include <proto/toon.h>')
    lines.append('')
    lines.append('struct Library *ToonBase = NULL;')
    lines.append('')
    lines.append('static int tests_run = 0;')
    lines.append('static int tests_passed = 0;')
    lines.append('static int tests_failed = 0;')
    lines.append('')
    # check_decode uses ToonDecode (text->JSON text) and compares
    lines.append('static void check_decode(const char *name, const char *input,')
    lines.append('                         const char *expected_json, int indent, int strict)')
    lines.append('{')
    lines.append('    char *json;')
    lines.append('    tests_run++;')
    lines.append('    json = ToonDecode(input, indent, strict);')
    lines.append('    if (!json) {')
    lines.append('        if (expected_json == NULL) {')
    lines.append('            tests_passed++;')
    lines.append('            return;')
    lines.append('        }')
    lines.append('        printf("FAIL %s: decode returned NULL\\n", name);')
    lines.append('        tests_failed++;')
    lines.append('        return;')
    lines.append('    }')
    lines.append('    if (strcmp(json, expected_json) == 0) {')
    lines.append('        tests_passed++;')
    lines.append('    } else {')
    lines.append('        printf("FAIL %s\\n  expected: %s\\n  got:      %s\\n", name, expected_json, json);')
    lines.append('        tests_failed++;')
    lines.append('    }')
    lines.append('    ToonFreeString(json);')
    lines.append('}')
    lines.append('')
    # check_encode uses ToonEncode (JSON text->TOON text)
    lines.append('static void check_encode(const char *name, const char *input_json,')
    lines.append('                         const char *expected_toon, int indent, int delim)')
    lines.append('{')
    lines.append('    char *toon;')
    lines.append('    tests_run++;')
    lines.append('    toon = ToonEncode(input_json, indent, delim);')
    lines.append('    if (!toon) {')
    lines.append('        printf("FAIL %s: encode returned NULL\\n", name);')
    lines.append('        tests_failed++;')
    lines.append('        return;')
    lines.append('    }')
    lines.append('    if (strcmp(toon, expected_toon) == 0) {')
    lines.append('        tests_passed++;')
    lines.append('    } else {')
    lines.append('        printf("FAIL %s\\n  expected: %s\\n  got:      %s\\n", name, expected_toon, toon);')
    lines.append('        tests_failed++;')
    lines.append('    }')
    lines.append('    ToonFreeString(toon);')
    lines.append('}')
    lines.append('')
    # Helper for library-specific tests
    lines.append('static void check_str(const char *name, const char *got, const char *expected)')
    lines.append('{')
    lines.append('    tests_run++;')
    lines.append('    if (got && expected && strcmp(got, expected) == 0) {')
    lines.append('        tests_passed++;')
    lines.append('    } else if (!got && !expected) {')
    lines.append('        tests_passed++;')
    lines.append('    } else {')
    lines.append('        printf("FAIL %s\\n  expected: %s\\n  got:      %s\\n",')
    lines.append('               name, expected ? expected : "NULL", got ? got : "NULL");')
    lines.append('        tests_failed++;')
    lines.append('    }')
    lines.append('}')
    lines.append('')
    lines.append('static void check_int(const char *name, int got, int expected)')
    lines.append('{')
    lines.append('    tests_run++;')
    lines.append('    if (got == expected) {')
    lines.append('        tests_passed++;')
    lines.append('    } else {')
    lines.append('        printf("FAIL %s: expected %d, got %d\\n", name, expected, got);')
    lines.append('        tests_failed++;')
    lines.append('    }')
    lines.append('}')
    lines.append('')
    lines.append('static void run_library_api_tests(void)')
    lines.append('{')
    # Version
    lines.append('    check_int("ToonVersion returns 300", ToonVersion(), 300);')
    lines.append('')
    # JsonValue construction and query
    lines.append('    /* JsonValue construction and query */')
    lines.append('    {')
    lines.append('        JsonValue v;')
    lines.append('        double num, result;')
    lines.append('')
    lines.append('        v = ToonNewNull();')
    lines.append('        check_int("ToonNewNull type", ToonGetType(v), TOON_TYPE_NULL);')
    lines.append('        ToonFreeValue(v);')
    lines.append('')
    lines.append('        v = ToonNewBool(1);')
    lines.append('        check_int("ToonNewBool type", ToonGetType(v), TOON_TYPE_BOOL);')
    lines.append('        check_int("ToonGetBool value", ToonGetBool(v), 1);')
    lines.append('        ToonFreeValue(v);')
    lines.append('')
    lines.append('        num = 3.14;')
    lines.append('        v = ToonNewNumber(&num);')
    lines.append('        check_int("ToonNewNumber type", ToonGetType(v), TOON_TYPE_NUMBER);')
    lines.append('        ToonGetNumber(v, &result);')
    lines.append('        check_int("ToonGetNumber value", (int)(result * 100), 314);')
    lines.append('        ToonFreeValue(v);')
    lines.append('')
    lines.append('        v = ToonNewString("hello");')
    lines.append('        check_int("ToonNewString type", ToonGetType(v), TOON_TYPE_STRING);')
    lines.append('        check_str("ToonGetString value", ToonGetString(v), "hello");')
    lines.append('        ToonFreeValue(v);')
    lines.append('    }')
    lines.append('')
    # Array/Object construction
    lines.append('    /* Array and Object construction */')
    lines.append('    {')
    lines.append('        JsonValue arr, obj;')
    lines.append('        char *toon;')
    lines.append('')
    lines.append('        arr = ToonNewArray();')
    lines.append('        ToonArrayPush(arr, ToonNewString("a"));')
    lines.append('        ToonArrayPush(arr, ToonNewString("b"));')
    lines.append('        check_int("ToonArrayCount", ToonArrayCount(arr), 2);')
    lines.append('        check_str("ToonArrayItem 0", ToonGetString(ToonArrayItem(arr, 0)), "a");')
    lines.append('        check_str("ToonArrayItem 1", ToonGetString(ToonArrayItem(arr, 1)), "b");')
    lines.append('        ToonFreeValue(arr);')
    lines.append('')
    lines.append('        obj = ToonNewObject();')
    lines.append('        ToonObjectSet(obj, "name", ToonNewString("Ada"));')
    lines.append('        ToonObjectSet(obj, "age", ToonNewBool(0));')
    lines.append('        check_int("ToonObjectCount", ToonObjectCount(obj), 2);')
    lines.append('        check_str("ToonObjectKey 0", ToonObjectKey(obj, 0), "name");')
    lines.append('        check_str("ToonGetString from obj", ToonGetString(ToonObjectItem(obj, 0)), "Ada");')
    lines.append('        ToonFreeValue(obj);')
    lines.append('    }')
    lines.append('')
    # ToonGet / ToonSet / ToonDel
    lines.append('    /* Path operations via library */')
    lines.append('    {')
    lines.append('        char *r;')
    lines.append('')
    lines.append('        r = ToonGet("name: Alice\\nage: 30", "name", TOON_FMT_AUTO);')
    lines.append('        check_str("ToonGet name", r, "Alice");')
    lines.append('        ToonFreeString(r);')
    lines.append('')
    lines.append('        r = ToonGet("name: Alice\\nage: 30", "age", TOON_FMT_AUTO);')
    lines.append('        check_str("ToonGet age", r, "30");')
    lines.append('        ToonFreeString(r);')
    lines.append('')
    lines.append('        r = ToonGet("items[2]: a,b", "items.1", TOON_FMT_AUTO);')
    lines.append('        check_str("ToonGet array index", r, "b");')
    lines.append('        ToonFreeString(r);')
    lines.append('')
    lines.append('        r = ToonGet("name: Alice\\nage: 30", ".", TOON_FMT_JSON);')
    lines.append('        check_str("ToonGet root JSON", r, "{\\"name\\":\\"Alice\\",\\"age\\":30}");')
    lines.append('        ToonFreeString(r);')
    lines.append('')
    lines.append('        r = ToonSet("name: Alice", "name", "Bob");')
    lines.append('        check_str("ToonSet replace", r, "name: Bob");')
    lines.append('        ToonFreeString(r);')
    lines.append('')
    lines.append('        r = ToonSet("name: Alice", "age", "30");')
    lines.append('        check_str("ToonSet add key", r, "name: Alice\\nage: 30");')
    lines.append('        ToonFreeString(r);')
    lines.append('')
    lines.append('        r = ToonDel("name: Alice\\nage: 30", "age");')
    lines.append('        check_str("ToonDel key", r, "name: Alice");')
    lines.append('        ToonFreeString(r);')
    lines.append('    }')
    lines.append('')
    # JSON parse/emit via library
    lines.append('    /* JSON parse/emit */')
    lines.append('    {')
    lines.append('        const char *err = NULL;')
    lines.append('        JsonValue v;')
    lines.append('        char *json;')
    lines.append('')
    lines.append('        v = ToonParseJSON("{\\"x\\":1}", &err);')
    lines.append('        check_int("ToonParseJSON type", ToonGetType(v), TOON_TYPE_OBJECT);')
    lines.append('        json = ToonEmitJSON(v);')
    lines.append('        check_str("ToonEmitJSON", json, "{\\"x\\":1}");')
    lines.append('        ToonFreeString(json);')
    lines.append('        ToonFreeValue(v);')
    lines.append('    }')
    lines.append('}')
    lines.append('')
    # main
    lines.append('int main(void)')
    lines.append('{')
    lines.append('    ToonBase = OpenLibrary(TOON_NAME, TOON_VERSION);')
    lines.append('    if (!ToonBase) {')
    lines.append('        printf("Cannot open %s v%d\\n", TOON_NAME, TOON_VERSION);')
    lines.append('        return 1;')
    lines.append('    }')
    lines.append('')
    lines.append('    printf("toon.library opened (spec %d.%d)\\n\\n",')
    lines.append('           ToonVersion() / 100, ToonVersion() % 100);')
    lines.append('')
    lines.append('    /* --- Spec conformance tests (same as test_runner) --- */')

    lines.extend(test_lines)

    lines.append('')
    lines.append('    /* --- Library-specific API tests --- */')
    lines.append('    run_library_api_tests();')
    lines.append('')
    lines.append('    printf("\\ntest_library: %d tests: %d passed, %d failed\\n",')
    lines.append('           tests_run, tests_passed, tests_failed);')
    lines.append('')
    lines.append('    CloseLibrary(ToonBase);')
    lines.append('    return tests_failed > 0 ? 1 : 0;')
    lines.append('}')

    return '\n'.join(lines) + '\n'


# ---------------------------------------------------------------------------
# Build system generation
# ---------------------------------------------------------------------------

"""
CPU build profiles: (name, cpu_flag, optimization_flags, description)
"""
CPU_PROFILES = [
    ("000", "CPU=68000", "OPT STRMER OPTPEEP",
     "68000 — broadest compatibility, size-optimized"),
    ("020", "CPU=68020", "OPT OPTTIME OPTINLINE OPTPEEP OPTLOOP STRMER",
     "68020+ — balanced speed optimization"),
    ("040", "CPU=68040", "OPT OPTTIME OPTSCHED OPTINLINE OPTPEEP OPTLOOP STRMER",
     "68040+ — speed-optimized with instruction scheduling"),
    ("060", "CPU=68060", "OPT OPTTIME OPTSCHED OPTINLINE OPTPEEP OPTLOOP STRMER",
     "68060 — speed-optimized with 060 scheduling"),
]

SOURCE_FILES_C = ['main.c', 'toon_decode.c', 'toon_encode.c',
                  'json_parse.c', 'json_emit.c', 'toon_util.c', 'toon_path.c']
OBJS_STR = ' '.join(f.replace('.c', '.o') for f in SOURCE_FILES_C)


def generate_smakefile(with_tests=False):
    """Generate SMakefile for SAS/C with multi-CPU targets."""
    test_objs = "test_runner.o toon_decode.o toon_encode.o json_parse.o json_emit.o toon_util.o"

    lines = [
        "# SMakefile for toon - TOON format CLI for AmigaOS",
        "# SAS/C 6.58",
        "#",
        "# Usage: smake              build default (toon.020)",
        "#        smake all          build all CPU variants",
        "#        smake toon.000     build 68000 version",
        "#        smake toon.020     build 68020 version",
        "#        smake toon.040     build 68040 version",
        "#        smake toon.060     build 68060 version",
        "#        smake clean        remove all build artifacts",
        f"#        smake test         build and run conformance tests" if with_tests else "",
        "#",
        "# Requires SC: assign pointing to your SAS/C installation.",
        "",
        "CC = sc:c/sc",
        "LD = sc:c/slink",
        "LIBS = lib:scnb.lib lib:scmnb.lib lib:amiga.lib",
        f"OBJS = {OBJS_STR}",
        "SRCS = " + " ".join(SOURCE_FILES_C),
        "",
    ]

    # Default target builds 020
    lines += [
        "# Default: build 68020 variant",
        "default: toon.020",
        "",
        "# Build all CPU variants",
        "all: toon.000 toon.020 toon.040 toon.060",
        "",
    ]

    # Each CPU variant: compile all .c files with CPU-specific flags, link, clean .o
    # smake doesn't support per-target CFLAGS, so each target is a full sequence
    for suffix, cpu_flag, opt_flags, desc in CPU_PROFILES:
        cflags = f"{cpu_flag} {opt_flags}"
        lines += [
            f"# {desc}",
            f"toon.{suffix}:",
        ]
        for src in SOURCE_FILES_C:
            lines.append(f"\t$(CC) {cflags} {src}")
        lines += [
            f"\t$(LD) lib:c.o $(OBJS) TO toon.{suffix} LIB $(LIBS) NOICONS",
            "\t-delete \\#?.o quiet",
            "",
        ]

    # Dependencies (for incremental builds of individual files)
    lines += [
        "# Dependencies",
        "main.o: main.c toon.h",
        "toon_decode.o: toon_decode.c toon.h",
        "toon_encode.o: toon_encode.c toon.h",
        "json_parse.o: json_parse.c toon.h",
        "json_emit.o: json_emit.c toon.h",
        "toon_util.o: toon_util.c toon.h",
        "",
    ]

    if with_tests:
        lines += [
            "# Test runner (uses 68020 flags)",
            "test_runner:",
        ]
        for src in SOURCE_FILES_C:
            lines.append(f"\t$(CC) CPU=68020 OPT OPTTIME STRMER {src}")
        lines += [
            "\t$(CC) CPU=68020 OPT OPTTIME STRMER test_runner.c",
            f"\t$(LD) lib:c.o {test_objs} TO test_runner LIB $(LIBS) NOICONS",
            "\t-delete \\#?.o quiet",
            "",
            "test: test_runner",
            "\ttest_runner",
            "",
        ]

    lines += [
        "clean:",
        "\t-delete \\#?.o quiet",
        "\t-delete toon.\\#? quiet",
    ]

    if with_tests:
        lines.append("\t-delete test_runner quiet")

    lines.append("")
    return '\n'.join(lines)


def generate_lib_smakefile(lib_major=1, lib_minor=4):
    """Generate SMakefile for toon.library in toon-port/lib/ layout.

    In toon-port, the core .c files live in the parent directory (//),
    not in //src/ like the git repo layout.
    """
    lib_core = ['toon_decode', 'toon_encode', 'json_parse', 'json_emit',
                'toon_util', 'toon_path']
    libobjs = 'toon_lib.o ' + ' '.join(f'{s}.o' for s in lib_core)

    lines = [
        "# SMakefile for toon.library - TOON format shared library for AmigaOS",
        "# SAS/C 6.58 (generated by build_port.py for toon-port layout)",
        "#",
        "# Usage: smake                 build toon.library",
        "#        smake example         build example caller",
        "#        smake clean           remove build artifacts",
        "#",
        "# Requires SC: assign pointing to SAS/C installation.",
        "",
        "CC = sc:c/sc",
        "LD = sc:c/slink",
        "",
        f"LIBOBJS = {libobjs}",
        "",
        "# In toon-port layout, core sources are in parent dir (//)",
        "LIBCFLAGS = LIBCODE NOSTACKCHECK NOCHKABORT NOICONS IDLEN=80 IDIR=INCLUDE: IDIR=//",
        "LIBFDFILE = //include/fd/toon_lib.fd",
        "",
        "EXCFLAGS = NOSTACKCHECK NOCHKABORT NOICONS IDLEN=80 IDIR=//include IDIR=INCLUDE:",
        "",
        "LIBS = lib:sc.lib lib:amiga.lib",
        "",
        "all: toon.library",
        "",
        "toon.library: $(LIBOBJS)",
        "\t$(LD) with <<",
        "LIBPREFIX _LIB",
        "LIBFD $(LIBFDFILE)",
        "FROM lib:libent.o lib:libinit.o $(LIBOBJS)",
        "TO toon.library",
        "LIB $(LIBS)",
        "NOICONS SD",
        "DEFINE __XCEXIT=___stub",
        f"LIBVERSION {lib_major} LIBREVISION {lib_minor}",
        "<",
        "",
        "toon_lib.o: toon_lib.c //toon.h",
        "\t$(CC) $(LIBCFLAGS) LIBRARYFDFILE=$(LIBFDFILE) toon_lib.c",
        "",
    ]

    for s in lib_core:
        lines += [
            f"{s}.o: //{s}.c //toon.h",
            f"\t$(CC) $(LIBCFLAGS) //{s}.c",
            "",
        ]

    lines += [
        "example: toon_example",
        "toon_example: toon_example.o",
        "\t$(CC) LINK toon_example.o TO toon_example LIB lib:sc.lib lib:amiga.lib NOICONS",
        "",
        "toon_example.o: toon_example.c",
        "\t$(CC) $(EXCFLAGS) toon_example.c",
        "",
        "clean:",
        "\t-delete \\#?.o quiet",
        "\t-delete toon.library quiet",
        "\t-delete toon_example quiet",
    ]

    return '\n'.join(lines) + '\n'


def generate_scoptions():
    """Generate SCoptions for SAS/C."""
    return textwrap.dedent("""\
        NOSTACKCHECK
        NOCHKABORT
        NOICONS
        IDLEN=80
        IDIR=INCLUDE:
    """)


def generate_vamosrc():
    """Generate .vamosrc for cross-compilation."""
    return textwrap.dedent("""\
        [vamos]
        cpu = 68020
        ram_size = 8192
        hw_access = emu

        [assigns]
        include = sc:include
        lib = sc:lib

        [icon.library]
        mode = fake
    """)


# ---------------------------------------------------------------------------
# README generation
# ---------------------------------------------------------------------------

def generate_readme(spec_version, with_tests=False):
    """Generate README.md."""
    today = datetime.now().strftime("%Y-%m-%d")
    test_section = ""
    if with_tests:
        test_section = textwrap.dedent("""\

        ### Running Tests

        Build and run the conformance test suite (auto-generated from spec fixtures):

        ```
        smake test
        ```

        The test runner exercises decode and encode against the official TOON spec
        test fixtures.
        """)

    return textwrap.dedent(f"""\
        # toon - TOON Format CLI for AmigaOS

        A native AmigaOS command-line tool for working with [TOON](https://github.com/toon-format/spec) (Token-Oriented Object Notation) files. Implements the TOON Specification v{spec_version}.

        TOON is a line-oriented, indentation-based text format that encodes the JSON data model with explicit structure and minimal quoting. It is particularly compact for arrays of uniform objects.

        ```
        users[3]{{id,name,role,active}}:
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
        {test_section}
        ### Cross-Compiling with vamos (macOS/Linux)

        You can also build on a modern system using [vamos](https://github.com/cnvogelg/amitools) (a lightweight AmigaOS API emulator). A `.vamosrc` is included that sets up the necessary assigns and enables `icon.library` in fake mode (required for smake under vamos).

        ```sh
        pip3 install amitools
        export SC=/path/to/sasc658

        vamos -V sc:$SC -V src:$(pwd) --cwd src: -c .vamosrc sc:c/smake
        ```

        To run the built binary:

        ```sh
        vamos -S -C 68020 -m 8192 -H emu -s 512 \\
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
        echo '{{"name":"Alice","age":30}}' | toon encode
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
        users[2]{{id,name,active}}:
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
        data[2|]{{name|value}}:
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

        Implements TOON Specification v{spec_version} with the following coverage:

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

        ---
        *Port generated on {today} by build_port.py*
    """)


# ---------------------------------------------------------------------------
# Version stamp update
# ---------------------------------------------------------------------------

def update_version_stamp(header_content, spec_version, app_version):
    """Update the $VER string and spec version in toon.h for toon-port."""
    today = datetime.now()
    date_str = f"{today.day}.{today.month}.{today.year}"

    # Update $VER string with real app version
    header_content = re.sub(
        r'\$VER: toon \S+ \(\S+\)',
        f'$VER: toon {app_version} ({date_str})',
        header_content
    )

    # Update spec version comment
    header_content = re.sub(
        r'Implements TOON Specification v\S+',
        f'Implements TOON Specification v{spec_version}',
        header_content
    )

    return header_content


# ---------------------------------------------------------------------------
# Fallback examples (if no spec repo available)
# ---------------------------------------------------------------------------

FALLBACK_EXAMPLES = {
    "users.toon": textwrap.dedent("""\
        users[3]{id,name,role,active}:
          1,Alice,admin,true
          2,Bob,developer,true
          3,Charlie,designer,false"""),
    "users.json": textwrap.dedent("""\
        {
          "users": [
            { "id": 1, "name": "Alice", "role": "admin", "active": true },
            { "id": 2, "name": "Bob", "role": "developer", "active": true },
            { "id": 3, "name": "Charlie", "role": "designer", "active": false }
          ]
        }"""),
    "config.toon": textwrap.dedent("""\
        server:
          host: localhost
          port: 8080
          timeout: 30000
        database:
          connection:
            host: db.example.com
            port: 5432
            username: admin
            database: myapp_prod
          pool:
            min: 2
            max: 10
        logging:
          level: info
          format: json"""),
    "config.json": textwrap.dedent("""\
        {
          "server": {
            "host": "localhost",
            "port": 8080,
            "timeout": 30000
          },
          "database": {
            "connection": {
              "host": "db.example.com",
              "port": 5432,
              "username": "admin",
              "database": "myapp_prod"
            },
            "pool": { "min": 2, "max": 10 }
          },
          "logging": {
            "level": "info",
            "format": "json"
          }
        }"""),
    "mixed.toon": textwrap.dedent("""\
        title: My Application
        version: 2.1
        tags[3]: amiga,utility,cli
        features:
          networking: false
          scripting: true
        authors[2]:
          - name: Alice
            email: alice@example.com
          - name: Bob
            email: bob@example.com"""),
    "products.toon": textwrap.dedent("""\
        products[4]{sku,name,qty,price}:
          A001,Widget,50,9.99
          A002,Gadget,25,14.5
          B001,Doohickey,100,3.25
          B002,Thingamajig,12,29.99
        warehouse: Building 7
        last_updated: 2026-03-26"""),
}


# ---------------------------------------------------------------------------
# Cross-compilation via vamos
# ---------------------------------------------------------------------------

def find_sasc():
    """Find the SAS/C installation directory."""
    # Check $SC environment variable
    sc = os.environ.get('SC')
    if sc and os.path.isdir(sc):
        return sc

    # Common locations
    for candidate in [
        os.path.expanduser('~/sasc658'),
        '/Users/brie/sasc658',
        os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sasc658'),
    ]:
        if os.path.isdir(candidate) and os.path.exists(os.path.join(candidate, 'c', 'sc')):
            return candidate

    return None


def vamos_compile(sc_dir, port_dir, source, flags, vamosrc):
    """Compile a single file via vamos."""
    import subprocess
    # Clear RAM
    ram_dir = os.path.expanduser('~/.vamos/volumes/ram')
    if os.path.exists(ram_dir):
        shutil.rmtree(ram_dir)

    cmd = [
        'vamos',
        '-c', vamosrc,
        '-V', f'sc:{sc_dir}',
        '-V', f'src:{port_dir}',
        'sc:c/sc', f'src:{source}',
        'NOSTACKCHECK', 'NOCHKABORT', 'NOICONS', 'IDLEN=80',
        'IDIR=sc:include',
    ] + flags + ['OBJNAME=src:']

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120,
                            stdin=subprocess.DEVNULL)
    # Check for errors (not warnings)
    for line in (result.stdout + result.stderr).split('\n'):
        if 'Error' in line and 'WARNING' not in line:
            return False, line
    return True, ""


def vamos_link(sc_dir, port_dir, output_name, vamosrc):
    """Link object files via vamos."""
    import subprocess
    ram_dir = os.path.expanduser('~/.vamos/volumes/ram')
    if os.path.exists(ram_dir):
        shutil.rmtree(ram_dir)

    objs = [f'src:{o}' for o in OBJS_STR.split()]
    cmd = [
        'vamos',
        '-c', vamosrc,
        '-V', f'sc:{sc_dir}',
        '-V', f'src:{port_dir}',
        'sc:c/slink', 'lib:c.o',
    ] + objs + [
        'TO', f'src:{output_name}',
        'LIB', 'lib:scnb.lib', 'lib:scmnb.lib', 'lib:amiga.lib',
        'NOICONS',
    ]

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120,
                            stdin=subprocess.DEVNULL)
    output = result.stdout + result.stderr
    # Check for link errors (not Warning 624 which is expected from scnb.lib)
    for line in output.split('\n'):
        if 'Error' in line and 'Warning' not in line and 'WARNING' not in line:
            return False, line
    return True, ""


LIB_CORE_SOURCES = ['toon_decode.c', 'toon_encode.c', 'json_parse.c',
                     'json_emit.c', 'toon_util.c', 'toon_path.c']


def build_library(sc_dir, port_dir, vamosrc, lib_major=1, lib_minor=4):
    """Build toon.library (shared) and toon.lib (static) via vamos."""
    import subprocess

    lib_dir = os.path.join(port_dir, 'lib')
    if not os.path.exists(os.path.join(lib_dir, 'toon_lib.c')):
        print("  Skipping library build (lib/toon_lib.c not found)")
        return False

    fd_file = os.path.join(port_dir, 'include', 'fd', 'toon_lib.fd')
    if not os.path.exists(fd_file):
        print("  Skipping library build (include/fd/toon_lib.fd not found)")
        return False

    print("  Building toon.library...")

    # Clean .o files in lib dir
    for f in Path(lib_dir).glob('*.o'):
        f.unlink()

    # Common LIBCODE flags
    lib_flags = ['LIBCODE', 'NOSTACKCHECK', 'NOCHKABORT', 'NOICONS', 'IDLEN=80']

    # Compile core sources (from port_dir, not lib_dir) with LIBCODE
    ok = True
    for src in LIB_CORE_SOURCES:
        ram_dir = os.path.expanduser('~/.vamos/volumes/ram')
        if os.path.exists(ram_dir):
            shutil.rmtree(ram_dir)

        cmd = [
            'vamos',
            '-c', vamosrc,
            '-V', f'sc:{sc_dir}',
            '-V', f'src:{port_dir}',
            'sc:c/sc', f'src:{src}',
            'IDIR=sc:include',
        ] + lib_flags + [f'OBJNAME=src:lib/']

        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120,
                                stdin=subprocess.DEVNULL)
        for line in (result.stdout + result.stderr).split('\n'):
            if 'Error' in line and 'WARNING' not in line:
                print(f"    COMPILE FAILED: {src}: {line}")
                ok = False
                break
        if not ok:
            break

    if not ok:
        return False

    # Compile toon_lib.c with LIBRARYFDFILE
    ram_dir = os.path.expanduser('~/.vamos/volumes/ram')
    if os.path.exists(ram_dir):
        shutil.rmtree(ram_dir)

    cmd = [
        'vamos',
        '-c', vamosrc,
        '-V', f'sc:{sc_dir}',
        '-V', f'src:{port_dir}',
        'sc:c/sc', 'src:lib/toon_lib.c',
        'IDIR=sc:include', 'IDIR=src:',
        f'LIBRARYFDFILE=src:include/fd/toon_lib.fd',
    ] + lib_flags + ['OBJNAME=src:lib/']

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120,
                            stdin=subprocess.DEVNULL)
    for line in (result.stdout + result.stderr).split('\n'):
        if 'Error' in line and 'WARNING' not in line:
            print(f"    COMPILE FAILED: toon_lib.c: {line}")
            return False

    # Link shared library with slink via WITH file
    # slink needs stdin closed (DEVNULL) to prevent hanging on prompts
    ram_dir = os.path.expanduser('~/.vamos/volumes/ram')
    if os.path.exists(ram_dir):
        shutil.rmtree(ram_dir)

    lib_obj_names = ['src:lib/toon_lib.o'] + \
        [f'src:lib/{s.replace(".c", ".o")}' for s in LIB_CORE_SOURCES]

    with_content = '\n'.join([
        'LIBPREFIX _LIB',
        'LIBFD src:include/fd/toon_lib.fd',
        'FROM lib:libent.o lib:libinit.o ' + ' '.join(lib_obj_names),
        'TO src:lib/toon.library',
        'LIB lib:sc.lib lib:amiga.lib',
        'NOICONS SD',
        'DEFINE __XCEXIT=___stub',
        f'LIBVERSION {lib_major} LIBREVISION {lib_minor}',
    ]) + '\n'

    with_file = os.path.join(port_dir, 'lib', '_slink.with')
    with open(with_file, 'w', newline='\n') as f:
        f.write(with_content)

    cmd = [
        'vamos',
        '-c', vamosrc,
        '-V', f'sc:{sc_dir}',
        '-V', f'src:{port_dir}',
        'sc:c/slink', 'WITH', 'src:lib/_slink.with',
    ]

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120,
                            stdin=subprocess.DEVNULL)
    output = result.stdout + result.stderr

    # Clean up WITH file
    if os.path.exists(with_file):
        os.unlink(with_file)

    for line in output.split('\n'):
        if 'Error' in line and 'Warning' not in line and 'WARNING' not in line:
            print(f"    LINK FAILED: {line}")
            return False

    lib_path = os.path.join(lib_dir, 'toon.library')
    if not os.path.exists(lib_path):
        print("    FAILED: toon.library not created")
        return False

    size = os.path.getsize(lib_path)
    print(f"    toon.library OK ({size:,} bytes)")

    # Build static library with oml
    print("  Building toon.lib (static)...")
    ram_dir = os.path.expanduser('~/.vamos/volumes/ram')
    if os.path.exists(ram_dir):
        shutil.rmtree(ram_dir)

    # oml creates a static lib from the .o files (excluding toon_lib.o which is the wrapper)
    static_objs = [f'src:lib/{s.replace(".c", ".o")}' for s in LIB_CORE_SOURCES]
    cmd = [
        'vamos',
        '-c', vamosrc,
        '-V', f'sc:{sc_dir}',
        '-V', f'src:{port_dir}',
        'sc:c/oml', 'src:lib/toon.lib', 'R',
    ] + static_objs

    result = subprocess.run(cmd, capture_output=True, timeout=120,
                            stdin=subprocess.DEVNULL)
    output = result.stdout.decode('latin-1', errors='replace') + \
             result.stderr.decode('latin-1', errors='replace')
    for line in output.split('\n'):
        if 'Error' in line and 'Warning' not in line and 'WARNING' not in line:
            print(f"    OML FAILED: {line}")
            # Non-fatal: static lib is optional
            break

    slib_path = os.path.join(lib_dir, 'toon.lib')
    if os.path.exists(slib_path):
        size = os.path.getsize(slib_path)
        print(f"    toon.lib OK ({size:,} bytes)")
    else:
        print("    toon.lib not created (non-fatal)")

    # Clean .o files in lib dir
    for f in Path(lib_dir).glob('*.o'):
        f.unlink()

    return True


def cross_compile(port_dir, args, lib_major=1, lib_minor=4):
    """Cross-compile all CPU variants + library via vamos."""
    sc_dir = find_sasc()
    if not sc_dir:
        print("Error: SAS/C not found. Set $SC environment variable.", file=sys.stderr)
        print("  export SC=/path/to/sasc658", file=sys.stderr)
        sys.exit(1)

    # Check vamos is available
    import subprocess
    try:
        subprocess.run(['vamos', '--help'], capture_output=True, timeout=5)
    except FileNotFoundError:
        print("Error: vamos not found. Install with: pip3 install amitools", file=sys.stderr)
        sys.exit(1)

    vamosrc = os.path.join(port_dir, '.vamosrc')
    print(f"Cross-compiling via vamos (SAS/C at {sc_dir})...")

    results = {}
    for suffix, cpu_flag, opt_flags, desc in CPU_PROFILES:
        print(f"  Building toon.{suffix} ({desc})...")

        # Clean .o files
        for f in Path(port_dir).glob('*.o'):
            f.unlink()

        # Compile each source file
        flags = [cpu_flag] + opt_flags.split()
        ok = True
        for src in SOURCE_FILES_C:
            success, err = vamos_compile(sc_dir, port_dir, src, flags, vamosrc)
            if not success:
                print(f"    COMPILE FAILED: {src}: {err}")
                ok = False
                break

        if not ok:
            results[suffix] = None
            continue

        # Link
        success, err = vamos_link(sc_dir, port_dir, f'toon.{suffix}', vamosrc)
        if not success:
            print(f"    LINK FAILED: {err}")
            results[suffix] = None
            continue

        # Get file size
        binary_path = os.path.join(port_dir, f'toon.{suffix}')
        if os.path.exists(binary_path):
            size = os.path.getsize(binary_path)
            results[suffix] = size
            print(f"    OK ({size:,} bytes)")
        else:
            results[suffix] = None
            print(f"    FAILED: binary not created")

    # Clean .o files
    for f in Path(port_dir).glob('*.o'):
        f.unlink()

    # Build shared library (toon.library)
    lib_ok = build_library(sc_dir, port_dir, vamosrc, lib_major, lib_minor)

    # Summary
    print()
    print("Build results:")
    for suffix, cpu_flag, opt_flags, desc in CPU_PROFILES:
        size = results.get(suffix)
        if size:
            print(f"  toon.{suffix}: {size:>6,} bytes  ({desc.split('—')[0].strip()})")
        else:
            print(f"  toon.{suffix}: FAILED")

    lib_path = os.path.join(port_dir, 'lib', 'toon.library')
    if os.path.exists(lib_path):
        size = os.path.getsize(lib_path)
        print(f"  toon.library: {size:>6,} bytes")
    else:
        print(f"  toon.library: FAILED")

    slib_path = os.path.join(port_dir, 'lib', 'toon.lib')
    if os.path.exists(slib_path):
        size = os.path.getsize(slib_path)
        print(f"  toon.lib:     {size:>6,} bytes  (static)")
    else:
        print(f"  toon.lib:     FAILED")

    if not all(results.values()) or not lib_ok:
        print("\nSome builds failed!", file=sys.stderr)
        sys.exit(1)


# ---------------------------------------------------------------------------
# Release packaging
# ---------------------------------------------------------------------------

def package_release(port_dir, spec_version, app_version, args):
    """Package binaries, examples, and docs into a release archive."""
    release_name = f"toon-{app_version}-amiga"
    release_dir = os.path.join(os.path.dirname(port_dir), 'releases', release_name)

    print(f"Packaging release: {release_name}")

    # Clean and create release directory tree
    if os.path.exists(release_dir):
        shutil.rmtree(release_dir)

    # Amiga-style layout
    dirs = [
        'C',                              # CLI binaries
        'Libs',                           # toon.library
        'sdk/autodocs',                   # toon.doc
        'sdk/fd',                         # toon_lib.fd
        'sdk/include/libraries',          # libraries/toon.h
        'sdk/include/proto',              # proto/toon.h
        'sdk/include/pragmas',            # pragmas/toon_pragmas.h
        'sdk/lib',                        # toon.lib (static)
        'sdk/src',                        # C sources + build files
        'sdk/src/lib',                    # library wrapper sources
        'examples',                       # TOON format examples
    ]
    for d in dirs:
        os.makedirs(os.path.join(release_dir, d), exist_ok=True)

    # C/ — CLI binaries
    binaries_copied = 0
    for suffix, _, _, _ in CPU_PROFILES:
        binary = os.path.join(port_dir, f'toon.{suffix}')
        if os.path.exists(binary):
            shutil.copy2(binary, os.path.join(release_dir, 'C'))
            binaries_copied += 1

    if binaries_copied == 0:
        print("  No binaries found. Run with --build first.", file=sys.stderr)
        sys.exit(1)

    # Libs/ — shared library
    lib_src = os.path.join(port_dir, 'lib', 'toon.library')
    if os.path.exists(lib_src):
        shutil.copy2(lib_src, os.path.join(release_dir, 'Libs'))

    # sdk/autodocs/ — autodoc
    autodoc = os.path.join(port_dir, 'toon.doc')
    if os.path.exists(autodoc):
        shutil.copy2(autodoc, os.path.join(release_dir, 'sdk', 'autodocs'))

    # sdk/fd/ — function descriptors
    fd_src = os.path.join(port_dir, 'include', 'fd', 'toon_lib.fd')
    if os.path.exists(fd_src):
        shutil.copy2(fd_src, os.path.join(release_dir, 'sdk', 'fd'))

    # sdk/include/ — headers
    sdk_headers = [
        ('include/libraries/toon.h', 'sdk/include/libraries/toon.h'),
        ('include/proto/toon.h', 'sdk/include/proto/toon.h'),
        ('include/pragmas/toon_pragmas.h', 'sdk/include/pragmas/toon_pragmas.h'),
    ]
    for src_rel, dst_rel in sdk_headers:
        src = os.path.join(port_dir, src_rel)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(release_dir, dst_rel))

    # sdk/lib/ — static link library
    slib_src = os.path.join(port_dir, 'lib', 'toon.lib')
    if os.path.exists(slib_src):
        shutil.copy2(slib_src, os.path.join(release_dir, 'sdk', 'lib'))

    # sdk/src/ — source files for building from scratch
    for sf in SOURCE_FILES_C + ['toon.h']:
        src = os.path.join(port_dir, sf)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(release_dir, 'sdk', 'src', sf))

    for bf in ['SMakefile', 'SCoptions']:
        src = os.path.join(port_dir, bf)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(release_dir, 'sdk', 'src', bf))

    # sdk/src/lib/ — library wrapper sources
    for lf in ['toon_lib.c', 'toon_example.c', 'SMakefile']:
        src = os.path.join(port_dir, 'lib', lf)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(release_dir, 'sdk', 'src', 'lib', lf))

    # examples/
    examples_dir = os.path.join(port_dir, 'examples')
    if os.path.isdir(examples_dir):
        for fname in os.listdir(examples_dir):
            shutil.copy2(
                os.path.join(examples_dir, fname),
                os.path.join(release_dir, 'examples', fname)
            )

    # Install script
    install = os.path.join(port_dir, 'Install')
    if os.path.exists(install):
        shutil.copy2(install, release_dir)

    # Generate release README
    today = datetime.now().strftime("%Y-%m-%d")
    readme = textwrap.dedent(f"""\
        toon {app_version} for AmigaOS
        {'=' * (len(f'toon {app_version} for AmigaOS'))}

        TOON (Token-Oriented Object Notation) CLI tool for AmigaOS.
        Version {app_version}. Implements TOON Specification v{spec_version}.

        Archive Layout
        --------------
        C/              CLI binaries (toon.000/.020/.040/.060)
        Libs/           toon.library (shared)
        sdk/            Developer SDK
          autodocs/     toon.doc (API reference)
          fd/           toon_lib.fd (function descriptors)
          include/      Headers (libraries/, proto/, pragmas/)
          lib/          toon.lib (static link library)
          src/          Full source + build files
        examples/       TOON format example files
        Install         AmigaDOS installation script

        Quick Install
        -------------
        Copy C/toon.020 (or appropriate CPU) to C: and rename to "toon":
          copy C:toon.020 C:toon

        Copy Libs/toon.library to LIBS:
          copy Libs:toon.library LIBS:

        Or run the Install script for guided installation.

        Quick Start
        -----------
        toon encode data.json           Convert JSON to TOON
        toon decode data.toon           Convert TOON to JSON
        toon get data.toon server.host  Read a value
        toon set data.toon key value    Write a value
        toon del data.toon key          Delete a value

        Run "toon" with no arguments for full usage information.

        Library Usage
        -------------
        Copy sdk/include/ to your compiler's INCLUDE: path.
        Copy sdk/fd/ to your FD: path.
        Copy sdk/lib/toon.lib to your LIB: path.
        See sdk/autodocs/toon.doc for full API reference (29 functions).

        #include <proto/toon.h>
        struct Library *ToonBase;
        ToonBase = OpenLibrary("toon.library", 1);

        Building from Source
        --------------------
        The sdk/src/ directory contains complete source code. With SAS/C 6.58:
          cd sdk/src
          smake all             ; builds all CLI CPU variants
          cd lib
          smake                 ; builds toon.library

        Requirements: AmigaOS 2.x+, 512KB free RAM.

        More Information
        ----------------
        TOON spec: https://github.com/toon-format/spec
        This port: https://github.com/nyteshade/nea-toon

        Released {today}
    """)

    with open(os.path.join(release_dir, 'README'), 'w', newline='\n') as f:
        f.write(readme)

    # Create tar.gz archive
    archive_base = os.path.join(os.path.dirname(port_dir), 'releases', release_name)
    archive_path = shutil.make_archive(
        archive_base, 'gztar',
        root_dir=os.path.dirname(release_dir),
        base_dir=release_name
    )

    # Summary
    archive_size = os.path.getsize(archive_path)
    print(f"  Release directory: {release_dir}/")
    print(f"  Archive: {archive_path} ({archive_size:,} bytes)")
    print()

    # Print sizes
    print("  Contents:")

    print("    C/")
    for suffix, _, _, desc in CPU_PROFILES:
        binary = os.path.join(release_dir, 'C', f'toon.{suffix}')
        if os.path.exists(binary):
            size = os.path.getsize(binary)
            print(f"      toon.{suffix}: {size:>6,} bytes  {desc.split('—')[0].strip()}")

    lib_path = os.path.join(release_dir, 'Libs', 'toon.library')
    if os.path.exists(lib_path):
        size = os.path.getsize(lib_path)
        print(f"    Libs/")
        print(f"      toon.library: {size:>6,} bytes")

    print("    sdk/")
    if os.path.exists(os.path.join(release_dir, 'sdk', 'autodocs', 'toon.doc')):
        print(f"      autodocs/toon.doc")
    if os.path.exists(os.path.join(release_dir, 'sdk', 'fd', 'toon_lib.fd')):
        print(f"      fd/toon_lib.fd")
    n_headers = sum(1 for _ in Path(os.path.join(release_dir, 'sdk', 'include')).rglob('*') if _.is_file())
    if n_headers:
        print(f"      include/: {n_headers} headers")
    slib = os.path.join(release_dir, 'sdk', 'lib', 'toon.lib')
    if os.path.exists(slib):
        size = os.path.getsize(slib)
        print(f"      lib/toon.lib: {size:>6,} bytes")
    sdk_src = os.path.join(release_dir, 'sdk', 'src')
    n_sources = len([f for f in os.listdir(sdk_src) if os.path.isfile(os.path.join(sdk_src, f))])
    print(f"      src/: {n_sources} files (+ lib/)")

    n_examples = len(os.listdir(os.path.join(release_dir, 'examples')))
    print(f"    examples/: {n_examples} files")
    if os.path.exists(os.path.join(release_dir, 'Install')):
        print(f"    Install")
    print(f"    README")

    print()
    print(f"Ready for GitHub release: {archive_path}")
    print(f"  gh release create v{app_version} {archive_path} --title 'toon {app_version} for AmigaOS'")


# ---------------------------------------------------------------------------
# Main build logic
# ---------------------------------------------------------------------------

def build_port(args):
    toon_repo = os.path.abspath(args.toon_repo)
    output_dir = os.path.abspath(args.output)
    src_dir = os.path.abspath(args.src)
    verbose = args.verbose

    # Auto-detect spec repo
    spec_repo = None
    if args.spec_repo:
        spec_repo = os.path.abspath(args.spec_repo)
    else:
        # Check common locations
        for candidate in [
            os.path.join(os.path.dirname(toon_repo), 'spec'),
            '/tmp/toon-spec',
            os.path.join(toon_repo, '..', 'spec'),
        ]:
            if os.path.isdir(candidate) and os.path.exists(os.path.join(candidate, 'SPEC.md')):
                spec_repo = os.path.abspath(candidate)
                break

        # Auto-clone spec repo if not found and tests are requested
        if not spec_repo and (args.with_tests or args.release):
            spec_clone_dir = os.path.join(os.path.dirname(toon_repo), 'spec')
            print(f"Spec repo not found. Cloning toon-format/spec to {spec_clone_dir}...")
            import subprocess
            try:
                subprocess.run(
                    ['git', 'clone', 'https://github.com/toon-format/spec.git', spec_clone_dir],
                    check=True, capture_output=True, text=True, timeout=60
                )
                spec_repo = os.path.abspath(spec_clone_dir)
                print(f"  Cloned successfully.")
            except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired) as e:
                print(f"  Warning: Could not clone spec repo: {e}")
                print(f"  Tests will be generated without spec fixtures.")

    # Validate inputs
    if not os.path.isdir(toon_repo):
        print(f"Error: toon repo not found at {toon_repo}", file=sys.stderr)
        sys.exit(1)

    if not os.path.isdir(src_dir):
        print(f"Error: source directory not found at {src_dir}", file=sys.stderr)
        sys.exit(1)

    source_files = ['toon.h', 'main.c', 'toon_decode.c', 'toon_encode.c',
                    'json_parse.c', 'json_emit.c', 'toon_util.c', 'toon_path.c']
    for sf in source_files:
        if not os.path.exists(os.path.join(src_dir, sf)):
            print(f"Error: source file {sf} not found in {src_dir}", file=sys.stderr)
            sys.exit(1)

    # Detect spec version
    spec_version = args.version or detect_spec_version(toon_repo, spec_repo)

    # Bump app version in source files if requested
    if args.app_version:
        print(f"Bumping app version to {args.app_version}...")
        updated = bump_version(src_dir, args.app_version)
        for f in updated:
            print(f"  Updated {f}")
        if not updated:
            print("  (no files needed updating)")
        print()

    # Read current app version from src/toon.h
    app_version = get_app_version(src_dir) or '1.0'
    app_major, app_minor = parse_app_version(app_version)

    print(f"TOON spec version: {spec_version}")
    print(f"App version: {app_version}")
    if spec_repo:
        print(f"Spec repo: {spec_repo}")
    print(f"Source dir: {src_dir}")
    print(f"Output dir: {output_dir}")
    print()

    # Clean output if requested
    if args.clean and os.path.exists(output_dir):
        shutil.rmtree(output_dir)

    # Create output directory
    os.makedirs(output_dir, exist_ok=True)
    os.makedirs(os.path.join(output_dir, 'examples'), exist_ok=True)
    os.makedirs(os.path.join(output_dir, 'lib'), exist_ok=True)
    os.makedirs(os.path.join(output_dir, 'include', 'fd'), exist_ok=True)
    os.makedirs(os.path.join(output_dir, 'include', 'libraries'), exist_ok=True)
    os.makedirs(os.path.join(output_dir, 'include', 'proto'), exist_ok=True)
    os.makedirs(os.path.join(output_dir, 'include', 'pragmas'), exist_ok=True)

    # Copy and process source files
    print("Processing source files...")
    all_issues = {}
    for sf in source_files:
        src_path = os.path.join(src_dir, sf)
        dst_path = os.path.join(output_dir, sf)

        with open(src_path, 'r') as f:
            content = f.read()

        # Apply fixups
        content = apply_fixups(content)

        # Update version stamp in header
        if sf == 'toon.h':
            content = update_version_stamp(content, spec_version, app_version)

        # Check C89 compatibility
        issues = check_c89_compat(sf, content, verbose)
        if issues:
            all_issues[sf] = issues

        # Write output
        with open(dst_path, 'w', newline='\n') as f:
            f.write(content)

        if verbose:
            print(f"  {sf}: {len(issues)} issues")

    # Report C89 issues
    if all_issues:
        print(f"\nC89/SAS/C compatibility warnings:")
        for filename, issues in all_issues.items():
            for lineno, name, message in issues:
                print(f"  {filename}:{lineno}: {message}")
        print()

    # Generate build files
    print("Generating build system...")
    with_tests = args.with_tests and spec_repo is not None

    with open(os.path.join(output_dir, 'SMakefile'), 'w', newline='\n') as f:
        f.write(generate_smakefile(with_tests))

    with open(os.path.join(output_dir, 'SCoptions'), 'w', newline='\n') as f:
        f.write(generate_scoptions())

    with open(os.path.join(output_dir, '.vamosrc'), 'w', newline='\n') as f:
        f.write(generate_vamosrc())

    # Generate test runner and test library
    if with_tests:
        print("Generating tests from spec fixtures...")
        fixtures = collect_test_fixtures(spec_repo)
        if fixtures:
            test_source = generate_test_runner(fixtures)
            with open(os.path.join(output_dir, 'test_runner.c'), 'w', newline='\n') as f:
                f.write(test_source)
            test_count = test_source.count('check_decode(') + test_source.count('check_encode(')
            print(f"  test_runner.c: {test_count} spec tests")

            lib_source = generate_test_library(fixtures)
            with open(os.path.join(output_dir, 'test_library.c'), 'w', newline='\n') as f:
                f.write(lib_source)
            lib_test_count = lib_source.count('check_decode(') + lib_source.count('check_encode(') + lib_source.count('check_int(') + lib_source.count('check_str(')
            print(f"  test_library.c: {test_count} spec + {lib_test_count - test_count} API tests")

    # Collect and write examples
    print("Collecting examples...")
    examples = collect_examples(toon_repo, spec_repo)
    if not examples:
        print("  No examples found in repos, using built-in examples")
        examples = FALLBACK_EXAMPLES

    for fname, content in sorted(examples.items()):
        dst_path = os.path.join(output_dir, 'examples', fname)
        with open(dst_path, 'w', newline='\n') as f:
            f.write(content)
        if verbose:
            print(f"  examples/{fname}")

    print(f"  {len(examples)} example files")

    # Copy autodoc
    autodoc_src = os.path.join(src_dir, 'toon.doc')
    if os.path.exists(autodoc_src):
        shutil.copy2(autodoc_src, os.path.join(output_dir, 'toon.doc'))
        print("Copied toon.doc (autodoc)")

    # Copy shared library sources and SDK headers
    print("Copying library sources and SDK...")
    lib_dir = os.path.join(src_dir, 'lib')
    inc_dir = os.path.join(src_dir, 'include')

    # Library wrapper source
    for lf in ['toon_lib.c', 'toon_example.c']:
        src = os.path.join(lib_dir, lf)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(output_dir, 'lib', lf))

    # Generate lib SMakefile adapted for toon-port layout
    with open(os.path.join(output_dir, 'lib', 'SMakefile'), 'w', newline='\n') as f:
        f.write(generate_lib_smakefile(app_major, app_minor))

    # SDK headers
    sdk_files = [
        ('fd/toon_lib.fd', 'include/fd/toon_lib.fd'),
        ('libraries/toon.h', 'include/libraries/toon.h'),
        ('proto/toon.h', 'include/proto/toon.h'),
        ('pragmas/toon_pragmas.h', 'include/pragmas/toon_pragmas.h'),
    ]
    for rel_path, dst_rel in sdk_files:
        src = os.path.join(inc_dir, rel_path)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(output_dir, dst_rel))

    # Install script
    install_src = os.path.join(src_dir, 'Install')
    if os.path.exists(install_src):
        shutil.copy2(install_src, os.path.join(output_dir, 'Install'))

    # Generate README
    print("Generating README.md...")
    with open(os.path.join(output_dir, 'README.md'), 'w', newline='\n') as f:
        f.write(generate_readme(spec_version, with_tests))

    # Summary
    print()
    print(f"toon-port built successfully in {output_dir}/")
    print(f"  Spec version: {spec_version}")
    print(f"  Source files: {len(source_files)}")
    print(f"  Examples: {len(examples)}")
    if with_tests:
        print(f"  Test runner: test_runner.c")

    # Cross-compile via vamos if requested
    if args.build or args.release:
        print()
        cross_compile(output_dir, args, app_major, app_minor)

    # Package release if requested
    if args.release:
        print()
        package_release(output_dir, spec_version, app_version, args)

    if not args.build and not args.release:
        print()
        print("To build on Amiga:  smake all")
        print("To cross-compile:   python3 build_port.py --build")
        print("To make a release:  python3 build_port.py --release")

    # Clean up Python cache
    pycache = os.path.join(os.path.dirname(os.path.abspath(__file__)), '__pycache__')
    if os.path.isdir(pycache):
        shutil.rmtree(pycache)


def main():
    parser = argparse.ArgumentParser(
        description='Build the Amiga TOON CLI port from the toon reference repo.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            Examples:
              %(prog)s                                    Generate toon-port/ only
              %(prog)s --build                            Generate + cross-compile all CPU variants
              %(prog)s --release --with-tests             Full release: generate, compile, package
              %(prog)s --app-version 1.5 --release        Bump version and build release
              %(prog)s --clean --with-tests -v            Verbose regeneration with tests
        """)
    )
    parser.add_argument('--toon-repo', default='./toon',
                        help='Path to toon-format/toon checkout (default: ./toon)')
    parser.add_argument('--spec-repo', default=None,
                        help='Path to toon-format/spec checkout (auto-detected if not given)')
    parser.add_argument('--output', default='./toon-port',
                        help='Output directory (default: ./toon-port)')
    parser.add_argument('--src', default='./src',
                        help='Path to our C sources (default: ./src)')
    parser.add_argument('--version', default=None,
                        help='Override spec version string')
    parser.add_argument('--app-version', default=None, metavar='X.Y',
                        help='Bump app version (e.g. 1.5) across all source files')
    parser.add_argument('--with-tests', action='store_true',
                        help='Generate test runner from spec fixtures')
    parser.add_argument('--build', action='store_true',
                        help='Cross-compile all CPU variants via vamos (requires $SC)')
    parser.add_argument('--release', action='store_true',
                        help='Build + package release archive (implies --build)')
    parser.add_argument('--clean', action='store_true',
                        help='Remove output directory before building')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Verbose output')

    args = parser.parse_args()
    # --release implies --build and --clean
    if args.release:
        args.build = True
        args.clean = True
    build_port(args)


if __name__ == '__main__':
    main()
