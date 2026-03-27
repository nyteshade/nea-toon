/*
 * toon_encode.c - TOON format encoder
 *
 * Encodes JsonValue tree into TOON text.
 * Implements TOON Spec v3.0 sections 2-3, 5-12.
 */

#include "toon.h"
#include <ctype.h>

/* ---- Encoder context ---- */

typedef struct {
    StrBuf sb;
    int indent_size;
    char doc_delim;     /* document delimiter char */
    ToonDelimiter delim_type;
} Encoder;

static char delim_char(ToonDelimiter dt)
{
    switch (dt) {
    case DELIM_TAB:  return '\t';
    case DELIM_PIPE: return '|';
    default:         return ',';
    }
}

/* ---- Forward declarations ---- */
static void encode_value(Encoder *enc, const JsonValue *v, int depth,
                         toon_bool is_root);
static void encode_object_fields(Encoder *enc, const JsonValue *obj,
                                 int depth);

/* ---- Emit helpers ---- */

static void emit_key(Encoder *enc, const char *key)
{
    if (toon_valid_unquoted_key(key)) {
        sb_append(&enc->sb, key);
    } else {
        /* Quoted key */
        const char *p;
        sb_appendc(&enc->sb, '"');
        for (p = key; *p; p++) {
            switch (*p) {
            case '\\': sb_append(&enc->sb, "\\\\"); break;
            case '"':  sb_append(&enc->sb, "\\\""); break;
            case '\n': sb_append(&enc->sb, "\\n"); break;
            case '\r': sb_append(&enc->sb, "\\r"); break;
            case '\t': sb_append(&enc->sb, "\\t"); break;
            default:   sb_appendc(&enc->sb, *p); break;
            }
        }
        sb_appendc(&enc->sb, '"');
    }
}

static void emit_primitive(Encoder *enc, const JsonValue *v, char active_delim)
{
    char nbuf[64];

    switch (v->type) {
    case JSON_NULL:
        sb_append(&enc->sb, "null");
        break;
    case JSON_BOOL:
        sb_append(&enc->sb, v->u.bval ? "true" : "false");
        break;
    case JSON_NUMBER:
        format_number(v->u.nval, nbuf, sizeof(nbuf));
        sb_append(&enc->sb, nbuf);
        break;
    case JSON_STRING:
        if (toon_needs_quote(v->u.sval, active_delim)) {
            const char *p;
            sb_appendc(&enc->sb, '"');
            for (p = v->u.sval; *p; p++) {
                switch (*p) {
                case '\\': sb_append(&enc->sb, "\\\\"); break;
                case '"':  sb_append(&enc->sb, "\\\""); break;
                case '\n': sb_append(&enc->sb, "\\n"); break;
                case '\r': sb_append(&enc->sb, "\\r"); break;
                case '\t': sb_append(&enc->sb, "\\t"); break;
                default:   sb_appendc(&enc->sb, *p); break;
                }
            }
            sb_appendc(&enc->sb, '"');
        } else {
            sb_append(&enc->sb, v->u.sval);
        }
        break;
    default:
        sb_append(&enc->sb, "null");
        break;
    }
}

/* ---- Delimiter symbol for header ---- */

static void emit_delim_sym(Encoder *enc, char delim)
{
    if (delim == '\t') sb_appendc(&enc->sb, '\t');
    else if (delim == '|') sb_appendc(&enc->sb, '|');
    /* comma = no symbol */
}

/* ---- Array type detection ---- */

static toon_bool is_primitive(const JsonValue *v)
{
    return v && (v->type == JSON_NULL || v->type == JSON_BOOL ||
                 v->type == JSON_NUMBER || v->type == JSON_STRING);
}

static toon_bool is_all_primitives(const JsonValue *arr)
{
    int i;
    for (i = 0; i < arr->u.arr.count; i++) {
        if (!is_primitive(arr->u.arr.items[i])) return FALSE;
    }
    return TRUE;
}

/* Check if array qualifies for tabular format */
static toon_bool is_tabular_array(const JsonValue *arr)
{
    int i, j;
    const JsonValue *first;
    const JsonObject *fo;

    if (arr->u.arr.count == 0) return FALSE;

    /* All elements must be objects */
    for (i = 0; i < arr->u.arr.count; i++) {
        if (arr->u.arr.items[i]->type != JSON_OBJECT) return FALSE;
    }

    first = arr->u.arr.items[0];
    fo = &first->u.obj;
    if (fo->count == 0) return FALSE;

    /* All values must be primitives */
    for (i = 0; i < arr->u.arr.count; i++) {
        const JsonObject *o = &arr->u.arr.items[i]->u.obj;
        if (o->count != fo->count) return FALSE;
        for (j = 0; j < o->count; j++) {
            if (!is_primitive(o->pairs[j].value)) return FALSE;
        }
    }

    /* All objects must have same keys */
    for (i = 1; i < arr->u.arr.count; i++) {
        const JsonObject *o = &arr->u.arr.items[i]->u.obj;
        for (j = 0; j < fo->count; j++) {
            /* Check that each key from first exists in this object */
            toon_bool found = FALSE;
            int k;
            for (k = 0; k < o->count; k++) {
                if (strcmp(fo->pairs[j].key, o->pairs[k].key) == 0) {
                    found = TRUE;
                    break;
                }
            }
            if (!found) return FALSE;
        }
    }

    return TRUE;
}

/* Find value for a key in an object */
static const JsonValue *obj_find(const JsonValue *obj, const char *key)
{
    int i;
    for (i = 0; i < obj->u.obj.count; i++) {
        if (strcmp(obj->u.obj.pairs[i].key, key) == 0)
            return obj->u.obj.pairs[i].value;
    }
    return NULL;
}

/* ---- Array encoding ---- */

static void encode_inline_array(Encoder *enc, const char *key,
                                const JsonValue *arr, int depth, char delim)
{
    int i;
    char nbuf[16];

    sb_append_indent(&enc->sb, depth, enc->indent_size);
    if (key) emit_key(enc, key);

    sb_appendc(&enc->sb, '[');
    sprintf(nbuf, "%d", arr->u.arr.count);
    sb_append(&enc->sb, nbuf);
    emit_delim_sym(enc, delim);
    sb_appendc(&enc->sb, ']');
    sb_appendc(&enc->sb, ':');

    if (arr->u.arr.count > 0) {
        sb_appendc(&enc->sb, ' ');
        for (i = 0; i < arr->u.arr.count; i++) {
            if (i > 0) sb_appendc(&enc->sb, delim);
            emit_primitive(enc, arr->u.arr.items[i], delim);
        }
    }
}

static void encode_tabular_array(Encoder *enc, const char *key,
                                 const JsonValue *arr, int depth, char delim)
{
    int i, j;
    const JsonObject *fo = &arr->u.arr.items[0]->u.obj;
    char nbuf[16];

    sb_append_indent(&enc->sb, depth, enc->indent_size);
    if (key) emit_key(enc, key);

    sb_appendc(&enc->sb, '[');
    sprintf(nbuf, "%d", arr->u.arr.count);
    sb_append(&enc->sb, nbuf);
    emit_delim_sym(enc, delim);
    sb_appendc(&enc->sb, ']');

    /* Fields segment */
    sb_appendc(&enc->sb, '{');
    for (i = 0; i < fo->count; i++) {
        if (i > 0) sb_appendc(&enc->sb, delim);
        emit_key(enc, fo->pairs[i].key);
    }
    sb_appendc(&enc->sb, '}');
    sb_appendc(&enc->sb, ':');

    /* Rows */
    for (i = 0; i < arr->u.arr.count; i++) {
        sb_appendc(&enc->sb, '\n');
        sb_append_indent(&enc->sb, depth + 1, enc->indent_size);
        for (j = 0; j < fo->count; j++) {
            const JsonValue *val;
            if (j > 0) sb_appendc(&enc->sb, delim);
            val = obj_find(arr->u.arr.items[i], fo->pairs[j].key);
            if (val) {
                emit_primitive(enc, val, delim);
            } else {
                sb_append(&enc->sb, "null");
            }
        }
    }
}

static void encode_list_array(Encoder *enc, const char *key,
                              const JsonValue *arr, int depth, char delim)
{
    int i;
    char nbuf[16];

    sb_append_indent(&enc->sb, depth, enc->indent_size);
    if (key) emit_key(enc, key);

    sb_appendc(&enc->sb, '[');
    sprintf(nbuf, "%d", arr->u.arr.count);
    sb_append(&enc->sb, nbuf);
    emit_delim_sym(enc, delim);
    sb_appendc(&enc->sb, ']');
    sb_appendc(&enc->sb, ':');

    for (i = 0; i < arr->u.arr.count; i++) {
        const JsonValue *item = arr->u.arr.items[i];

        sb_appendc(&enc->sb, '\n');
        sb_append_indent(&enc->sb, depth + 1, enc->indent_size);

        if (is_primitive(item)) {
            sb_append(&enc->sb, "- ");
            emit_primitive(enc, item, delim);
        } else if (item->type == JSON_ARRAY) {
            /* Nested array as list item */
            sb_append(&enc->sb, "- ");
            if (is_all_primitives(item)) {
                /* Inline */
                int j;
                sb_appendc(&enc->sb, '[');
                sprintf(nbuf, "%d", item->u.arr.count);
                sb_append(&enc->sb, nbuf);
                emit_delim_sym(enc, delim);
                sb_appendc(&enc->sb, ']');
                sb_appendc(&enc->sb, ':');
                if (item->u.arr.count > 0) {
                    sb_appendc(&enc->sb, ' ');
                    for (j = 0; j < item->u.arr.count; j++) {
                        if (j > 0) sb_appendc(&enc->sb, delim);
                        emit_primitive(enc, item->u.arr.items[j], delim);
                    }
                }
            } else {
                /* Nested list */
                int j;
                sb_appendc(&enc->sb, '[');
                sprintf(nbuf, "%d", item->u.arr.count);
                sb_append(&enc->sb, nbuf);
                emit_delim_sym(enc, delim);
                sb_appendc(&enc->sb, ']');
                sb_appendc(&enc->sb, ':');
                for (j = 0; j < item->u.arr.count; j++) {
                    sb_appendc(&enc->sb, '\n');
                    sb_append_indent(&enc->sb, depth + 2, enc->indent_size);
                    sb_append(&enc->sb, "- ");
                    if (is_primitive(item->u.arr.items[j])) {
                        emit_primitive(enc, item->u.arr.items[j], delim);
                    } else {
                        /* TODO: deeper nesting */
                        sb_append(&enc->sb, "null");
                    }
                }
            }
        } else if (item->type == JSON_OBJECT) {
            /* Object as list item */
            if (item->u.obj.count == 0) {
                sb_appendc(&enc->sb, '-');
            } else {
                int j;
                const JsonObject *o = &item->u.obj;

                /* Check if first field is tabular array */
                if (o->count > 0 && o->pairs[0].value->type == JSON_ARRAY &&
                    is_tabular_array(o->pairs[0].value)) {
                    /* First field tabular on hyphen line */
                    const JsonValue *tab = o->pairs[0].value;
                    const JsonObject *tfo = &tab->u.arr.items[0]->u.obj;
                    int r, c;

                    sb_append(&enc->sb, "- ");
                    emit_key(enc, o->pairs[0].key);
                    sb_appendc(&enc->sb, '[');
                    sprintf(nbuf, "%d", tab->u.arr.count);
                    sb_append(&enc->sb, nbuf);
                    emit_delim_sym(enc, delim);
                    sb_appendc(&enc->sb, ']');
                    sb_appendc(&enc->sb, '{');
                    for (c = 0; c < tfo->count; c++) {
                        if (c > 0) sb_appendc(&enc->sb, delim);
                        emit_key(enc, tfo->pairs[c].key);
                    }
                    sb_appendc(&enc->sb, '}');
                    sb_appendc(&enc->sb, ':');

                    /* Tabular rows at depth + 2 */
                    for (r = 0; r < tab->u.arr.count; r++) {
                        sb_appendc(&enc->sb, '\n');
                        sb_append_indent(&enc->sb, depth + 3, enc->indent_size);
                        for (c = 0; c < tfo->count; c++) {
                            const JsonValue *cv;
                            if (c > 0) sb_appendc(&enc->sb, delim);
                            cv = obj_find(tab->u.arr.items[r],
                                          tfo->pairs[c].key);
                            if (cv) emit_primitive(enc, cv, delim);
                            else sb_append(&enc->sb, "null");
                        }
                    }

                    /* Remaining fields at depth + 1 */
                    for (j = 1; j < o->count; j++) {
                        sb_appendc(&enc->sb, '\n');
                        encode_value(enc, o->pairs[j].value, depth + 2, FALSE);
                    }
                } else {
                    /* Normal object: first field on hyphen line */
                    sb_append(&enc->sb, "- ");
                    emit_key(enc, o->pairs[0].key);
                    sb_appendc(&enc->sb, ':');

                    if (is_primitive(o->pairs[0].value)) {
                        sb_appendc(&enc->sb, ' ');
                        emit_primitive(enc, o->pairs[0].value, enc->doc_delim);
                    } else if (o->pairs[0].value->type == JSON_ARRAY) {
                        /* Array as first field value */
                        const JsonValue *fa = o->pairs[0].value;
                        if (is_all_primitives(fa)) {
                            sb_appendc(&enc->sb, '\n');
                            encode_value(enc, fa, depth + 2, FALSE);
                        } else {
                            sb_appendc(&enc->sb, '\n');
                            encode_value(enc, fa, depth + 2, FALSE);
                        }
                    } else if (o->pairs[0].value->type == JSON_OBJECT) {
                        /* Nested object */
                        sb_appendc(&enc->sb, '\n');
                        encode_object_fields(enc, o->pairs[0].value,
                                             depth + 2);
                    }

                    /* Remaining fields at depth+2 (one level under hyphen) */
                    for (j = 1; j < o->count; j++) {
                        sb_appendc(&enc->sb, '\n');
                        sb_append_indent(&enc->sb, depth + 2, enc->indent_size);
                        emit_key(enc, o->pairs[j].key);
                        sb_appendc(&enc->sb, ':');

                        if (is_primitive(o->pairs[j].value)) {
                            sb_appendc(&enc->sb, ' ');
                            emit_primitive(enc, o->pairs[j].value,
                                           enc->doc_delim);
                        } else if (o->pairs[j].value->type == JSON_ARRAY) {
                            sb_appendc(&enc->sb, '\n');
                            encode_value(enc, o->pairs[j].value,
                                         depth + 3, FALSE);
                        } else if (o->pairs[j].value->type == JSON_OBJECT) {
                            if (o->pairs[j].value->u.obj.count == 0) {
                                /* empty */
                            } else {
                                sb_appendc(&enc->sb, '\n');
                                encode_object_fields(enc, o->pairs[j].value,
                                                     depth + 3);
                            }
                        }
                    }
                }
            }
        }
    }
}

/* ---- Object encoding ---- */

static void encode_object_fields(Encoder *enc, const JsonValue *obj, int depth)
{
    int i;
    const JsonObject *o = &obj->u.obj;

    for (i = 0; i < o->count; i++) {
        const char *key = o->pairs[i].key;
        const JsonValue *val = o->pairs[i].value;

        if (i > 0) sb_appendc(&enc->sb, '\n');

        if (val->type == JSON_ARRAY) {
            char delim = enc->doc_delim;

            if (val->u.arr.count == 0) {
                /* Empty array */
                sb_append_indent(&enc->sb, depth, enc->indent_size);
                emit_key(enc, key);
                sb_appendc(&enc->sb, '[');
                sb_appendc(&enc->sb, '0');
                emit_delim_sym(enc, delim);
                sb_appendc(&enc->sb, ']');
                sb_appendc(&enc->sb, ':');
            } else if (is_all_primitives(val)) {
                encode_inline_array(enc, key, val, depth, delim);
            } else if (is_tabular_array(val)) {
                encode_tabular_array(enc, key, val, depth, delim);
            } else {
                encode_list_array(enc, key, val, depth, delim);
            }
        } else if (val->type == JSON_OBJECT) {
            sb_append_indent(&enc->sb, depth, enc->indent_size);
            emit_key(enc, key);
            sb_appendc(&enc->sb, ':');
            if (val->u.obj.count > 0) {
                sb_appendc(&enc->sb, '\n');
                encode_object_fields(enc, val, depth + 1);
            }
        } else {
            /* Primitive */
            sb_append_indent(&enc->sb, depth, enc->indent_size);
            emit_key(enc, key);
            sb_appendc(&enc->sb, ':');
            sb_appendc(&enc->sb, ' ');
            emit_primitive(enc, val, enc->doc_delim);
        }
    }
}

/* ---- Encode value (general) ---- */

static void encode_value(Encoder *enc, const JsonValue *v, int depth,
                         toon_bool is_root)
{
    char delim = enc->doc_delim;

    if (!v) return;

    switch (v->type) {
    case JSON_NULL:
    case JSON_BOOL:
    case JSON_NUMBER:
    case JSON_STRING:
        if (is_root) {
            emit_primitive(enc, v, delim);
        } else {
            sb_append_indent(&enc->sb, depth, enc->indent_size);
            emit_primitive(enc, v, delim);
        }
        break;

    case JSON_ARRAY:
        if (v->u.arr.count == 0) {
            if (is_root) {
                sb_append(&enc->sb, "[0]:");
            } else {
                sb_append_indent(&enc->sb, depth, enc->indent_size);
                sb_append(&enc->sb, "[0]:");
            }
        } else if (is_all_primitives(v)) {
            if (is_root) {
                encode_inline_array(enc, NULL, v, 0, delim);
            } else {
                encode_inline_array(enc, NULL, v, depth, delim);
            }
        } else if (is_tabular_array(v)) {
            if (is_root) {
                encode_tabular_array(enc, NULL, v, 0, delim);
            } else {
                encode_tabular_array(enc, NULL, v, depth, delim);
            }
        } else {
            if (is_root) {
                encode_list_array(enc, NULL, v, 0, delim);
            } else {
                encode_list_array(enc, NULL, v, depth, delim);
            }
        }
        break;

    case JSON_OBJECT:
        if (v->u.obj.count == 0) {
            /* Empty object = empty document at root */
        } else {
            if (is_root) {
                encode_object_fields(enc, v, 0);
            } else {
                encode_object_fields(enc, v, depth);
            }
        }
        break;
    }
}

/* ---- Public API ---- */

char *toon_encode(const JsonValue *v, const ToonEncodeOpts *opts)
{
    Encoder enc;

    memset(&enc, 0, sizeof(enc));
    sb_init(&enc.sb);

    enc.indent_size = 2;
    enc.delim_type = DELIM_COMMA;
    enc.doc_delim = ',';

    if (opts) {
        if (opts->indent > 0) enc.indent_size = opts->indent;
        enc.delim_type = opts->delim;
        enc.doc_delim = delim_char(opts->delim);
    }

    encode_value(&enc, v, 0, TRUE);

    return sb_detach(&enc.sb);
}
