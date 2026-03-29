/*
 * libraries/toon.h - Public header for toon.library
 *
 * TOON (Token-Oriented Object Notation) shared library for AmigaOS
 * Implements TOON Specification v3.0
 *
 * Usage:
 *   #include <proto/toon.h>
 *   struct Library *ToonBase;
 *   ToonBase = OpenLibrary("toon.library", 1);
 */

#ifndef LIBRARIES_TOON_H
#define LIBRARIES_TOON_H

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif

/* ---- Library identity ---- */

#define TOON_NAME       "toon.library"
#define TOON_VERSION    1
#define TOON_REVISION   3
#define TOON_SPEC       "3.0"

/* ---- JSON value types ---- */

#define TOON_TYPE_NULL    0
#define TOON_TYPE_BOOL    1
#define TOON_TYPE_NUMBER  2
#define TOON_TYPE_STRING  3
#define TOON_TYPE_ARRAY   4
#define TOON_TYPE_OBJECT  5

/* ---- Delimiter constants for ToonEncode ---- */

#define TOON_DELIM_COMMA  0
#define TOON_DELIM_TAB    1
#define TOON_DELIM_PIPE   2

/* ---- Output format constants for ToonGet ---- */

#define TOON_FMT_AUTO    0   /* primitives bare, complex as TOON */
#define TOON_FMT_JSON    1   /* always JSON */
#define TOON_FMT_TOON    2   /* always TOON */

/* ---- Opaque JsonValue handle ---- */

/*
 * JsonValue is an opaque pointer. Library users should not access
 * its internals. Use ToonGetType, ToonGetString, etc. to query,
 * and ToonNewString, ToonArrayPush, ToonObjectSet to construct.
 *
 * All JsonValue pointers returned by the library must be freed
 * with ToonFreeValue(). All char* strings returned by the library
 * must be freed with ToonFreeString().
 */
typedef void *JsonValue;

#endif /* LIBRARIES_TOON_H */
