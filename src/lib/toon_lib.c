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
 * Internal helpers for path operations.
 * These decode TOON, perform the operation, and return text.
 */

char * __asm __saveds LIBToonGet(
    register __a0 const char *toon,
    register __a1 const char *path,
    register __d0 int format)
{
    const char *err = NULL;
    ToonDecodeOpts dopts;
    JsonValue *root;
    /* We need json_get_path from main.c - replicate the logic here */
    /* For the library, we'll implement a minimal path walker */

    dopts.indent = 2;
    dopts.strict = FALSE;
    root = toon_decode(toon, &dopts, &err);
    if (!root) return NULL;

    /* Walk the path */
    {
        JsonValue *cur = root;
        const char *p = path;
        char keybuf[256];
        int ki;

        while (*p && cur) {
            if (*p == '.') p++;

            if (*p == '[') {
                p++;
                if (*p == '"') {
                    /* Quoted key ["key"] */
                    ki = 0;
                    p++;
                    while (*p && *p != '"') {
                        if (*p == '\\' && p[1]) { p++; }
                        if (ki < 255) keybuf[ki++] = *p;
                        p++;
                    }
                    keybuf[ki] = '\0';
                    if (*p == '"') p++;
                    if (*p == ']') p++;
                    /* Key lookup */
                    if (cur->type == JSON_OBJECT) {
                        int i;
                        toon_bool found = FALSE;
                        for (i = 0; i < cur->u.obj.count; i++) {
                            if (strcmp(cur->u.obj.pairs[i].key, keybuf) == 0) {
                                cur = cur->u.obj.pairs[i].value;
                                found = TRUE;
                                break;
                            }
                        }
                        if (!found) { json_free(root); return NULL; }
                    } else {
                        json_free(root); return NULL;
                    }
                } else {
                    /* Numeric index [N] */
                    int idx = 0;
                    while (*p >= '0' && *p <= '9') {
                        idx = idx * 10 + (*p - '0');
                        p++;
                    }
                    if (*p == ']') p++;
                    if (cur->type == JSON_ARRAY) {
                        if (idx < 0 || idx >= cur->u.arr.count) {
                            json_free(root); return NULL;
                        }
                        cur = cur->u.arr.items[idx];
                    } else {
                        json_free(root); return NULL;
                    }
                }
            } else {
                /* Key segment */
                toon_bool all_digits = TRUE;
                ki = 0;
                while (*p && *p != '.' && *p != '[') {
                    if (*p < '0' || *p > '9') all_digits = FALSE;
                    if (ki < 255) keybuf[ki++] = *p;
                    p++;
                }
                keybuf[ki] = '\0';
                if (ki == 0) continue;

                if (all_digits && cur->type == JSON_ARRAY) {
                    int idx = atoi(keybuf);
                    if (idx < 0 || idx >= cur->u.arr.count) {
                        json_free(root); return NULL;
                    }
                    cur = cur->u.arr.items[idx];
                } else if (cur->type == JSON_OBJECT) {
                    int i;
                    toon_bool found = FALSE;
                    for (i = 0; i < cur->u.obj.count; i++) {
                        if (strcmp(cur->u.obj.pairs[i].key, keybuf) == 0) {
                            cur = cur->u.obj.pairs[i].value;
                            found = TRUE;
                            break;
                        }
                    }
                    if (!found) { json_free(root); return NULL; }
                } else {
                    json_free(root); return NULL;
                }
            }
        }

        /* Format the result */
        if (!cur) { json_free(root); return NULL; }

        {
            char *result = NULL;
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
                /* Complex: format as JSON or TOON */
                if (format == 1) {
                    /* JSON */
                    result = json_emit(cur);
                } else {
                    /* TOON (default) */
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
    }
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
        /* Try number */
        const char *p = value;
        toon_bool is_num = TRUE;
        if (*p == '-') p++;
        if (*p < '0' || *p > '9') is_num = FALSE;
        if (is_num) {
            while (*p >= '0' && *p <= '9') p++;
            if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
            if (*p == '\0') {
                new_val = json_new_number(strtod(value, NULL));
            } else {
                new_val = json_new_string(value);
            }
        } else {
            new_val = json_new_string(value);
        }
    }

    /* Walk path and set - simplified version */
    /* For v1.0, set only works on existing paths in objects */
    {
        JsonValue *cur = root;
        const char *p = path;
        char keybuf[256];
        int ki;
        char last_key[256];
        int last_idx = -1;
        int last_type = 0; /* 1=key, 2=index */
        JsonValue *parent = NULL;

        /* Parse path segments, keeping track of parent + last segment */
        while (*p) {
            parent = cur;
            if (*p == '.') p++;

            if (*p == '[') {
                p++;
                if (*p >= '0' && *p <= '9') {
                    int idx = 0;
                    while (*p >= '0' && *p <= '9') {
                        idx = idx * 10 + (*p - '0');
                        p++;
                    }
                    if (*p == ']') p++;

                    /* Check if there's more path */
                    if (*p == '\0' || (*p == '.' && p[1] == '\0')) {
                        last_type = 2;
                        last_idx = idx;
                        break;
                    }
                    if (cur->type == JSON_ARRAY && idx < cur->u.arr.count)
                        cur = cur->u.arr.items[idx];
                    else { json_free(root); json_free(new_val); return NULL; }
                } else {
                    json_free(root); json_free(new_val); return NULL;
                }
            } else {
                ki = 0;
                while (*p && *p != '.' && *p != '[') {
                    if (ki < 255) keybuf[ki++] = *p;
                    p++;
                }
                keybuf[ki] = '\0';

                /* Check if there's more path */
                if (*p == '\0') {
                    last_type = 1;
                    strcpy(last_key, keybuf);
                    break;
                }

                /* Navigate */
                if (cur->type == JSON_OBJECT) {
                    int i;
                    toon_bool found = FALSE;
                    for (i = 0; i < cur->u.obj.count; i++) {
                        if (strcmp(cur->u.obj.pairs[i].key, keybuf) == 0) {
                            cur = cur->u.obj.pairs[i].value;
                            found = TRUE;
                            break;
                        }
                    }
                    if (!found) {
                        /* Create intermediate object */
                        JsonValue *mid = json_new_object();
                        json_object_set(cur, keybuf, mid);
                        cur = mid;
                    }
                } else if (cur->type == JSON_ARRAY) {
                    toon_bool all_digits = TRUE;
                    const char *q;
                    int idx;
                    for (q = keybuf; *q; q++) {
                        if (*q < '0' || *q > '9') { all_digits = FALSE; break; }
                    }
                    if (all_digits) {
                        idx = atoi(keybuf);
                        if (idx < cur->u.arr.count)
                            cur = cur->u.arr.items[idx];
                        else { json_free(root); json_free(new_val); return NULL; }
                    } else {
                        json_free(root); json_free(new_val); return NULL;
                    }
                } else {
                    json_free(root); json_free(new_val); return NULL;
                }
            }
        }

        /* Apply the set */
        if (last_type == 1 && cur->type == JSON_OBJECT) {
            int i;
            for (i = 0; i < cur->u.obj.count; i++) {
                if (strcmp(cur->u.obj.pairs[i].key, last_key) == 0) {
                    json_free(cur->u.obj.pairs[i].value);
                    cur->u.obj.pairs[i].value = new_val;
                    goto set_done;
                }
            }
            json_object_set(cur, last_key, new_val);
        } else if (last_type == 2 && cur->type == JSON_ARRAY) {
            while (cur->u.arr.count <= last_idx)
                json_array_push(cur, json_new_null());
            json_free(cur->u.arr.items[last_idx]);
            cur->u.arr.items[last_idx] = new_val;
        } else {
            json_free(root);
            json_free(new_val);
            return NULL;
        }
    }

set_done:
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
    JsonValue *cur;
    const char *p;
    char keybuf[256];
    int ki;
    char *result;

    dopts.indent = 2;
    dopts.strict = FALSE;
    root = toon_decode(toon, &dopts, &err);
    if (!root) return NULL;

    /* Walk to parent of target */
    cur = root;
    p = path;

    while (*p) {
        char seg[256];
        int si = 0;

        if (*p == '.') p++;
        while (*p && *p != '.' && *p != '[') {
            if (si < 255) seg[si++] = *p;
            p++;
        }
        seg[si] = '\0';

        if (*p == '\0') {
            /* This is the last segment - delete it */
            if (cur->type == JSON_OBJECT) {
                int i;
                for (i = 0; i < cur->u.obj.count; i++) {
                    if (strcmp(cur->u.obj.pairs[i].key, seg) == 0) {
                        int k;
                        free(cur->u.obj.pairs[i].key);
                        json_free(cur->u.obj.pairs[i].value);
                        for (k = i; k < cur->u.obj.count - 1; k++)
                            cur->u.obj.pairs[k] = cur->u.obj.pairs[k + 1];
                        cur->u.obj.count--;
                        goto del_done;
                    }
                }
            }
            json_free(root);
            return NULL;
        }

        /* Navigate deeper */
        if (*p == '[') {
            /* Handle bracket notation at this point */
            int idx;
            p++;
            idx = 0;
            while (*p >= '0' && *p <= '9') {
                idx = idx * 10 + (*p - '0');
                p++;
            }
            if (*p == ']') p++;

            /* First navigate to the key */
            if (cur->type == JSON_OBJECT && si > 0) {
                int i;
                toon_bool found = FALSE;
                for (i = 0; i < cur->u.obj.count; i++) {
                    if (strcmp(cur->u.obj.pairs[i].key, seg) == 0) {
                        cur = cur->u.obj.pairs[i].value;
                        found = TRUE;
                        break;
                    }
                }
                if (!found) { json_free(root); return NULL; }
            }

            /* If this is the last segment, delete from array */
            if (*p == '\0') {
                if (cur->type == JSON_ARRAY && idx < cur->u.arr.count) {
                    int k;
                    json_free(cur->u.arr.items[idx]);
                    for (k = idx; k < cur->u.arr.count - 1; k++)
                        cur->u.arr.items[k] = cur->u.arr.items[k + 1];
                    cur->u.arr.count--;
                    goto del_done;
                }
                json_free(root);
                return NULL;
            }

            /* Navigate into array element */
            if (cur->type == JSON_ARRAY && idx < cur->u.arr.count) {
                cur = cur->u.arr.items[idx];
            } else {
                json_free(root);
                return NULL;
            }
        } else if (cur->type == JSON_OBJECT) {
            int i;
            toon_bool found = FALSE;
            for (i = 0; i < cur->u.obj.count; i++) {
                if (strcmp(cur->u.obj.pairs[i].key, seg) == 0) {
                    cur = cur->u.obj.pairs[i].value;
                    found = TRUE;
                    break;
                }
            }
            if (!found) { json_free(root); return NULL; }
        } else {
            json_free(root);
            return NULL;
        }
    }

    json_free(root);
    return NULL;

del_done:
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
