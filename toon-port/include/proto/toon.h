/*
 * proto/toon.h - Prototype/auto-open header for toon.library
 *
 * Include this header in your program to use toon.library.
 * It pulls in the type definitions and pragma definitions.
 *
 * You must declare and open the library base yourself:
 *
 *   struct Library *ToonBase = NULL;
 *   ToonBase = OpenLibrary("toon.library", 1);
 *   if (!ToonBase) { ... handle error ... }
 *   ... use library functions ...
 *   CloseLibrary(ToonBase);
 *
 * Or use auto-init (SAS/C _STI_/_STD_ functions).
 */

#ifndef PROTO_TOON_H
#define PROTO_TOON_H

#ifndef LIBRARIES_TOON_H
#include <libraries/toon.h>
#endif

#ifndef EXEC_LIBRARIES_H
#include <exec/libraries.h>
#endif

extern struct Library *ToonBase;

/* ---- Function prototypes ---- */

/* Core encode/decode (text to text) */
char *ToonEncode(const char *json, int indent, int delimiter);
char *ToonDecode(const char *toon, int indent, int strict);

/* Core encode/decode (with JsonValue) */
JsonValue ToonDecodeValue(const char *toon, int indent, int strict,
                          const char **err);
char *ToonEncodeValue(JsonValue val, int indent, int delimiter);

/* Path operations */
char *ToonGet(const char *toon, const char *path, int format);
char *ToonSet(const char *toon, const char *path, const char *value);
char *ToonDel(const char *toon, const char *path);

/* JsonValue construction */
JsonValue ToonNewNull(void);
JsonValue ToonNewBool(int val);
JsonValue ToonNewNumber(double *valptr);
JsonValue ToonNewString(const char *s);
JsonValue ToonNewArray(void);
JsonValue ToonNewObject(void);

/* JsonValue manipulation */
void ToonArrayPush(JsonValue arr, JsonValue item);
void ToonObjectSet(JsonValue obj, const char *key, JsonValue val);
void ToonFreeValue(JsonValue val);

/* JsonValue query */
int ToonGetType(JsonValue val);
const char *ToonGetString(JsonValue val);
int ToonGetNumber(JsonValue val, double *result);
int ToonGetBool(JsonValue val);
int ToonArrayCount(JsonValue val);
JsonValue ToonArrayItem(JsonValue val, int index);
int ToonObjectCount(JsonValue val);
const char *ToonObjectKey(JsonValue val, int index);
JsonValue ToonObjectItem(JsonValue val, int index);

/* JSON parse/emit */
JsonValue ToonParseJSON(const char *json, const char **err);
char *ToonEmitJSON(JsonValue val);

/* Utility */
void ToonFreeString(char *s);
int ToonVersion(void);

/* Pull in pragmas for SAS/C */
#ifdef __SASC
#include <pragmas/toon_pragmas.h>
#endif

#endif /* PROTO_TOON_H */
