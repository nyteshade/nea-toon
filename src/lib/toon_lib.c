/*
 * toon_lib.c - toon.library wrapper for AmigaOS
 *
 * Provides __asm __saveds library entry points that bridge
 * from register-based Amiga calling convention to the internal
 * TOON C functions.
 *
 * Compiled with LIBCODE, linked with libent.o + libinit.o.
 */

#include <exec/types.h>
#include <exec/libraries.h>

/* Internal TOON headers - our actual implementation */
#include "toon.h"

/*
 * Library version info is set by slink:
 *   LIBVERSION, LIBREVISION, LIBPREFIX, LIBFD
 * The _LibID and _LibName symbols are provided by libinit.o
 * based on the .fd file and slink options.
 */

/* Stubs for sc.lib exit handling (not used in library context) */
void __regargs __XCEXIT(long code) { }
long _ONBREAK = 0;

/*
 * __UserLibInit - called when library is first loaded
 * Return 0 for success, non-zero for failure.
 */
int __saveds __asm __UserLibInit(register __a6 struct Library *libbase)
{
    return 0;  /* nothing to initialize */
}

/*
 * __UserLibCleanup - called when library is being expunged
 */
void __saveds __asm __UserLibCleanup(register __a6 struct Library *libbase)
{
    /* nothing to clean up */
}

/* ================================================================
 * Library function implementations
 *
 * Each function has the LIB prefix (stripped by slink via LIBPREFIX)
 * and uses __asm __saveds with register parameters matching the .fd
 * ================================================================ */

/* ---- Core encode/decode (text to text) ---- */

char * __asm __saveds LIBToonEncode(
    register __a0 const char *json,
    register __d0 int indent,
    register __d1 int delimiter)
{
    const char *err = NULL;
    JsonValue *val;
    ToonEncodeOpts opts;
    char *result;

    val = json_parse(json, &err);
    if (!val) return NULL;

    opts.indent = indent > 0 ? indent : 2;
    opts.delim = (ToonDelimiter)delimiter;
    result = toon_encode(val, &opts);
    json_free(val);
    return result;
}

char * __asm __saveds LIBToonDecode(
    register __a0 const char *toon,
    register __d0 int indent,
    register __d1 int strict)
{
    const char *err = NULL;
    ToonDecodeOpts opts;
    JsonValue *val;
    char *result;

    opts.indent = indent > 0 ? indent : 2;
    opts.strict = strict;
    val = toon_decode(toon, &opts, &err);
    if (!val) return NULL;

    result = json_emit(val);
    json_free(val);
    return result;
}

/* ---- Core encode/decode (with JsonValue) ---- */

JsonValue * __asm __saveds LIBToonDecodeValue(
    register __a0 const char *toon,
    register __d0 int indent,
    register __d1 int strict,
    register __a1 const char **err)
{
    ToonDecodeOpts opts;
    opts.indent = indent > 0 ? indent : 2;
    opts.strict = strict;
    return toon_decode(toon, &opts, err);
}

char * __asm __saveds LIBToonEncodeValue(
    register __a0 JsonValue *val,
    register __d0 int indent,
    register __d1 int delimiter)
{
    ToonEncodeOpts opts;
    opts.indent = indent > 0 ? indent : 2;
    opts.delim = (ToonDelimiter)delimiter;
    return toon_encode(val, &opts);
}

/* ---- Path operations ---- */

/*
 * Path operations use toon_path_* from toon_path.c (shared with CLI).
 * These wrappers decode TOON, perform the operation, and return text.
 */

char * __asm __saveds LIBToonGet(
    register __a0 const char *toon,
    register __a1 const char *path,
    register __d0 int format)
{
    const char *err = NULL;
    ToonDecodeOpts dopts;
    JsonValue *root;
    const JsonValue *cur;
    char *result = NULL;

    dopts.indent = 2;
    dopts.strict = FALSE;
    root = toon_decode(toon, &dopts, &err);
    if (!root) return NULL;

    cur = toon_path_get(root, path);
    if (!cur) { json_free(root); return NULL; }

    switch (cur->type) {
    case JSON_NULL:
        result = (char *)malloc(5);
        if (result) strcpy(result, "null");
        break;
    case JSON_BOOL:
        result = (char *)malloc(6);
        if (result) strcpy(result, cur->u.bval ? "true" : "false");
        break;
    case JSON_NUMBER: {
        char nbuf[64];
        format_number(cur->u.nval, nbuf, sizeof(nbuf));
        result = (char *)malloc(strlen(nbuf) + 1);
        if (result) strcpy(result, nbuf);
        break;
    }
    case JSON_STRING:
        result = (char *)malloc(strlen(cur->u.sval) + 1);
        if (result) strcpy(result, cur->u.sval);
        break;
    default:
        if (format == 1) {
            result = json_emit(cur);
        } else {
            ToonEncodeOpts eopts;
            eopts.indent = 2;
            eopts.delim = DELIM_COMMA;
            result = toon_encode(cur, &eopts);
        }
        break;
    }
    json_free(root);
    return result;
}

char * __asm __saveds LIBToonSet(
    register __a0 const char *toon,
    register __a1 const char *path,
    register __a2 const char *value)
{
    const char *err = NULL;
    ToonDecodeOpts dopts;
    ToonEncodeOpts eopts;
    JsonValue *root;
    JsonValue *new_val;
    char *result;

    dopts.indent = 2;
    dopts.strict = FALSE;
    root = toon_decode(toon, &dopts, &err);
    if (!root) return NULL;

    /* Parse the value string */
    if (strcmp(value, "true") == 0) {
        new_val = json_new_bool(TRUE);
    } else if (strcmp(value, "false") == 0) {
        new_val = json_new_bool(FALSE);
    } else if (strcmp(value, "null") == 0) {
        new_val = json_new_null();
    } else if (*value == '{' || *value == '[' || *value == '"') {
        new_val = json_parse(value, &err);
        if (!new_val) new_val = json_new_string(value);
    } else {
        const char *p = value;
        toon_bool is_num = TRUE;
        if (*p == '-') p++;
        if (*p < '0' || *p > '9') is_num = FALSE;
        if (is_num) {
            while (*p >= '0' && *p <= '9') p++;
            if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
            if (*p == '\0')
                new_val = json_new_number(strtod(value, NULL));
            else
                new_val = json_new_string(value);
        } else {
            new_val = json_new_string(value);
        }
    }

    if (toon_is_root_path(path)) {
        /* Replace root */
        json_free(root);
        root = new_val;
    } else if (!toon_path_set(root, path, new_val)) {
        json_free(new_val);
        json_free(root);
        return NULL;
    }

    eopts.indent = 2;
    eopts.delim = DELIM_COMMA;
    result = toon_encode(root, &eopts);
    json_free(root);
    return result;
}

char * __asm __saveds LIBToonDel(
    register __a0 const char *toon,
    register __a1 const char *path)
{
    const char *err = NULL;
    ToonDecodeOpts dopts;
    ToonEncodeOpts eopts;
    JsonValue *root;
    char *result;

    dopts.indent = 2;
    dopts.strict = FALSE;
    root = toon_decode(toon, &dopts, &err);
    if (!root) return NULL;

    if (!toon_path_del(root, path)) {
        json_free(root);
        return NULL;
    }

    eopts.indent = 2;
    eopts.delim = DELIM_COMMA;
    result = toon_encode(root, &eopts);
    json_free(root);
    return result;
}

/* ---- JsonValue construction ---- */

JsonValue * __asm __saveds LIBToonNewNull(void)
{
    return json_new_null();
}

int __asm __saveds LIBToonNewBool(register __d0 int val)
{
    return (int)json_new_bool(val);
}

JsonValue * __asm __saveds LIBToonNewNumber(register __a0 double *valptr)
{
    return json_new_number(*valptr);
}

JsonValue * __asm __saveds LIBToonNewString(register __a0 const char *s)
{
    return json_new_string(s);
}

JsonValue * __asm __saveds LIBToonNewArray(void)
{
    return json_new_array();
}

JsonValue * __asm __saveds LIBToonNewObject(void)
{
    return json_new_object();
}

/* ---- JsonValue manipulation ---- */

void __asm __saveds LIBToonArrayPush(
    register __a0 JsonValue *arr,
    register __a1 JsonValue *item)
{
    json_array_push(arr, item);
}

void __asm __saveds LIBToonObjectSet(
    register __a0 JsonValue *obj,
    register __a1 const char *key,
    register __a2 JsonValue *val)
{
    json_object_set(obj, key, val);
}

void __asm __saveds LIBToonFreeValue(register __a0 JsonValue *val)
{
    json_free(val);
}

/* ---- JsonValue query ---- */

int __asm __saveds LIBToonGetType(register __a0 JsonValue *val)
{
    if (!val) return JSON_NULL;
    return (int)val->type;
}

const char * __asm __saveds LIBToonGetString(register __a0 JsonValue *val)
{
    if (!val || val->type != JSON_STRING) return NULL;
    return val->u.sval;
}

int __asm __saveds LIBToonGetNumber(
    register __a0 JsonValue *val,
    register __a1 double *result)
{
    if (!val || val->type != JSON_NUMBER) return FALSE;
    *result = val->u.nval;
    return TRUE;
}

int __asm __saveds LIBToonGetBool(register __a0 JsonValue *val)
{
    if (!val || val->type != JSON_BOOL) return FALSE;
    return val->u.bval;
}

int __asm __saveds LIBToonArrayCount(register __a0 JsonValue *val)
{
    if (!val || val->type != JSON_ARRAY) return 0;
    return val->u.arr.count;
}

JsonValue * __asm __saveds LIBToonArrayItem(
    register __a0 JsonValue *val,
    register __d0 int index)
{
    if (!val || val->type != JSON_ARRAY) return NULL;
    if (index < 0 || index >= val->u.arr.count) return NULL;
    return val->u.arr.items[index];
}

int __asm __saveds LIBToonObjectCount(register __a0 JsonValue *val)
{
    if (!val || val->type != JSON_OBJECT) return 0;
    return val->u.obj.count;
}

const char * __asm __saveds LIBToonObjectKey(
    register __a0 JsonValue *val,
    register __d0 int index)
{
    if (!val || val->type != JSON_OBJECT) return NULL;
    if (index < 0 || index >= val->u.obj.count) return NULL;
    return val->u.obj.pairs[index].key;
}

JsonValue * __asm __saveds LIBToonObjectItem(
    register __a0 JsonValue *val,
    register __d0 int index)
{
    if (!val || val->type != JSON_OBJECT) return NULL;
    if (index < 0 || index >= val->u.obj.count) return NULL;
    return val->u.obj.pairs[index].value;
}

/* ---- JSON parse/emit ---- */

JsonValue * __asm __saveds LIBToonParseJSON(
    register __a0 const char *json,
    register __a1 const char **err)
{
    return json_parse(json, err);
}

char * __asm __saveds LIBToonEmitJSON(register __a0 JsonValue *val)
{
    return json_emit(val);
}

/* ---- Utility ---- */

void __asm __saveds LIBToonFreeString(register __a0 char *s)
{
    if (s) free(s);
}

int __asm __saveds LIBToonVersion(void)
{
    return 300;  /* spec version 3.0 = 300 */
}
