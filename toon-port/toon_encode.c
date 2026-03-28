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
static void emit_list_item_object(Encoder *enc, const JsonValue *item,
                                   int list_depth, char delim);
static void emit_object_field(Encoder *enc, const char *key,
                               const JsonValue *val, int depth, char delim);
static void emit_list_items(Encoder *enc, const JsonValue *arr,
                             int depth, char delim);

/* ---- Emit helpers ---- */

static void emit_key(Encoder *enc, const char *key)
{
    if (toon_valid_unquoted_key(key)) {
        sb_append(&enc->sb, key);
    } else {
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

static toon_bool is_tabular_array(const JsonValue *arr)
{
    int i, j;
    const JsonValue *first;
    const JsonObject *fo;

    if (arr->u.arr.count == 0) return FALSE;

    for (i = 0; i < arr->u.arr.count; i++) {
        if (arr->u.arr.items[i]->type != JSON_OBJECT) return FALSE;
    }

    first = arr->u.arr.items[0];
    fo = &first->u.obj;
    if (fo->count == 0) return FALSE;

    for (i = 0; i < arr->u.arr.count; i++) {
        const JsonObject *o = &arr->u.arr.items[i]->u.obj;
        if (o->count != fo->count) return FALSE;
        for (j = 0; j < o->count; j++) {
            if (!is_primitive(o->pairs[j].value)) return FALSE;
        }
    }

    for (i = 1; i < arr->u.arr.count; i++) {
        const JsonObject *o = &arr->u.arr.items[i]->u.obj;
        for (j = 0; j < fo->count; j++) {
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

static const JsonValue *obj_find(const JsonValue *obj, const char *key)
{
    int i;
    for (i = 0; i < obj->u.obj.count; i++) {
        if (strcmp(obj->u.obj.pairs[i].key, key) == 0)
            return obj->u.obj.pairs[i].value;
    }
    return NULL;
}

/* ---- Emit array header (bracket segment only, no indent/key) ---- */

static void emit_array_header(Encoder *enc, int count, char delim)
{
    char nbuf[16];
    sb_appendc(&enc->sb, '[');
    sprintf(nbuf, "%d", count);
    sb_append(&enc->sb, nbuf);
    emit_delim_sym(enc, delim);
    sb_appendc(&enc->sb, ']');
}

/* ---- Emit tabular header fields segment ---- */

static void emit_fields_seg(Encoder *enc, const JsonObject *fo, char delim)
{
    int i;
    sb_appendc(&enc->sb, '{');
    for (i = 0; i < fo->count; i++) {
        if (i > 0) sb_appendc(&enc->sb, delim);
        emit_key(enc, fo->pairs[i].key);
    }
    sb_appendc(&enc->sb, '}');
}

/* ---- Emit a field's array value inline on a line ---- */
/* Emits: key[N<delim>]: v1,v2  or  key[N<delim>]{f1,f2}: (with rows below) */
/* Returns TRUE if rows follow (tabular/list), FALSE if fully inline */

static toon_bool emit_field_array_header(Encoder *enc, const char *key,
                                         const JsonValue *arr, char delim)
{
    if (key) emit_key(enc, key);

    if (arr->u.arr.count == 0) {
        emit_array_header(enc, 0, delim);
        sb_appendc(&enc->sb, ':');
        return FALSE;
    }

    if (is_all_primitives(arr)) {
        int i;
        emit_array_header(enc, arr->u.arr.count, delim);
        sb_appendc(&enc->sb, ':');
        sb_appendc(&enc->sb, ' ');
        for (i = 0; i < arr->u.arr.count; i++) {
            if (i > 0) sb_appendc(&enc->sb, delim);
            emit_primitive(enc, arr->u.arr.items[i], delim);
        }
        return FALSE;
    }

    if (is_tabular_array(arr)) {
        const JsonObject *fo = &arr->u.arr.items[0]->u.obj;
        emit_array_header(enc, arr->u.arr.count, delim);
        emit_fields_seg(enc, fo, delim);
        sb_appendc(&enc->sb, ':');
        return TRUE; /* rows follow */
    }

    /* List array */
    emit_array_header(enc, arr->u.arr.count, delim);
    sb_appendc(&enc->sb, ':');
    return TRUE; /* items follow */
}

/* ---- Emit tabular rows at a given depth ---- */

static void emit_tabular_rows(Encoder *enc, const JsonValue *arr,
                               int depth, char delim)
{
    const JsonObject *fo = &arr->u.arr.items[0]->u.obj;
    int i, j;
    for (i = 0; i < arr->u.arr.count; i++) {
        sb_appendc(&enc->sb, '\n');
        sb_append_indent(&enc->sb, depth, enc->indent_size);
        for (j = 0; j < fo->count; j++) {
            const JsonValue *val;
            if (j > 0) sb_appendc(&enc->sb, delim);
            val = obj_find(arr->u.arr.items[i], fo->pairs[j].key);
            if (val) emit_primitive(enc, val, delim);
            else sb_append(&enc->sb, "null");
        }
    }
}

/* ---- Emit list items at a given depth ---- */

static void emit_list_items(Encoder *enc, const JsonValue *arr,
                             int depth, char delim)
{
    int i;
    char nbuf[16];

    for (i = 0; i < arr->u.arr.count; i++) {
        const JsonValue *item = arr->u.arr.items[i];

        sb_appendc(&enc->sb, '\n');
        sb_append_indent(&enc->sb, depth, enc->indent_size);

        if (is_primitive(item)) {
            sb_append(&enc->sb, "- ");
            emit_primitive(enc, item, delim);
        } else if (item->type == JSON_ARRAY) {
            sb_append(&enc->sb, "- ");
            if (is_all_primitives(item)) {
                int j;
                emit_array_header(enc, item->u.arr.count, delim);
                sb_appendc(&enc->sb, ':');
                if (item->u.arr.count > 0) {
                    sb_appendc(&enc->sb, ' ');
                    for (j = 0; j < item->u.arr.count; j++) {
                        if (j > 0) sb_appendc(&enc->sb, delim);
                        emit_primitive(enc, item->u.arr.items[j], delim);
                    }
                }
            } else {
                emit_array_header(enc, item->u.arr.count, delim);
                sb_appendc(&enc->sb, ':');
                /* Nested list items */
                emit_list_items(enc, item, depth + 1, delim);
            }
        } else if (item->type == JSON_OBJECT) {
            if (item->u.obj.count == 0) {
                sb_appendc(&enc->sb, '-');
            } else {
                emit_list_item_object(enc, item, depth, delim);
            }
        }
    }
}

/* ---- Emit an object as a list item (Section 10) ---- */

static void emit_list_item_object(Encoder *enc, const JsonValue *item,
                                   int list_depth, char delim)
{
    const JsonObject *o = &item->u.obj;
    int j;
    toon_bool first_is_array;
    toon_bool first_is_tabular;

    first_is_array = (o->pairs[0].value->type == JSON_ARRAY);
    first_is_tabular = first_is_array && is_tabular_array(o->pairs[0].value);

    /* First field on hyphen line */
    sb_append(&enc->sb, "- ");

    if (first_is_tabular) {
        /* Tabular array as first field: header on hyphen line, rows at depth+2 */
        const JsonValue *tab = o->pairs[0].value;
        emit_key(enc, o->pairs[0].key);
        emit_array_header(enc, tab->u.arr.count, delim);
        emit_fields_seg(enc, &tab->u.arr.items[0]->u.obj, delim);
        sb_appendc(&enc->sb, ':');
        emit_tabular_rows(enc, tab, list_depth + 2, delim);

        /* Remaining fields at depth+1 */
        for (j = 1; j < o->count; j++) {
            sb_appendc(&enc->sb, '\n');
            sb_append_indent(&enc->sb, list_depth + 1, enc->indent_size);
            emit_object_field(enc, o->pairs[j].key, o->pairs[j].value,
                              list_depth + 1, delim);
        }
    } else if (first_is_array) {
        /* Non-tabular array as first field: inline header on hyphen line */
        toon_bool has_rows;
        has_rows = emit_field_array_header(enc, o->pairs[0].key,
                                            o->pairs[0].value, delim);
        if (has_rows) {
            const JsonValue *fa = o->pairs[0].value;
            if (is_tabular_array(fa)) {
                emit_tabular_rows(enc, fa, list_depth + 2, delim);
            } else {
                emit_list_items(enc, fa, list_depth + 2, delim);
            }
        }

        /* Remaining fields at depth+1 */
        for (j = 1; j < o->count; j++) {
            sb_appendc(&enc->sb, '\n');
            sb_append_indent(&enc->sb, list_depth + 1, enc->indent_size);
            emit_object_field(enc, o->pairs[j].key, o->pairs[j].value,
                              list_depth + 1, delim);
        }
    } else {
        /* Normal object: first field on hyphen line */
        emit_key(enc, o->pairs[0].key);
        sb_appendc(&enc->sb, ':');

        if (is_primitive(o->pairs[0].value)) {
            sb_appendc(&enc->sb, ' ');
            emit_primitive(enc, o->pairs[0].value, enc->doc_delim);
        } else if (o->pairs[0].value->type == JSON_OBJECT) {
            if (o->pairs[0].value->u.obj.count > 0) {
                sb_appendc(&enc->sb, '\n');
                encode_object_fields(enc, o->pairs[0].value, list_depth + 2);
            }
        }

        /* Remaining fields at depth+1 (under the hyphen) */
        for (j = 1; j < o->count; j++) {
            sb_appendc(&enc->sb, '\n');
            sb_append_indent(&enc->sb, list_depth + 1, enc->indent_size);
            emit_object_field(enc, o->pairs[j].key, o->pairs[j].value,
                              list_depth + 1, delim);
        }
    }
}

/* ---- Emit a single object field (key: value or key[N]: ...) ---- */

static void emit_object_field(Encoder *enc, const char *key,
                               const JsonValue *val, int depth, char delim)
{
    if (val->type == JSON_ARRAY) {
        toon_bool has_rows = emit_field_array_header(enc, key, val, delim);
        if (has_rows) {
            if (is_tabular_array(val)) {
                emit_tabular_rows(enc, val, depth + 1, delim);
            } else {
                emit_list_items(enc, val, depth + 1, delim);
            }
        }
    } else if (val->type == JSON_OBJECT) {
        emit_key(enc, key);
        sb_appendc(&enc->sb, ':');
        if (val->u.obj.count > 0) {
            sb_appendc(&enc->sb, '\n');
            encode_object_fields(enc, val, depth + 1);
        }
    } else {
        emit_key(enc, key);
        sb_appendc(&enc->sb, ':');
        sb_appendc(&enc->sb, ' ');
        emit_primitive(enc, val, enc->doc_delim);
    }
}

/* ---- Object encoding ---- */

static void encode_object_fields(Encoder *enc, const JsonValue *obj, int depth)
{
    int i;
    const JsonObject *o = &obj->u.obj;

    for (i = 0; i < o->count; i++) {
        if (i > 0) sb_appendc(&enc->sb, '\n');
        sb_append_indent(&enc->sb, depth, enc->indent_size);
        emit_object_field(enc, o->pairs[i].key, o->pairs[i].value,
                          depth, enc->doc_delim);
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
        if (!is_root) sb_append_indent(&enc->sb, depth, enc->indent_size);
        emit_primitive(enc, v, delim);
        break;

    case JSON_ARRAY:
        if (v->u.arr.count == 0) {
            if (!is_root) sb_append_indent(&enc->sb, depth, enc->indent_size);
            emit_array_header(enc, 0, delim);
            sb_appendc(&enc->sb, ':');
        } else if (is_all_primitives(v)) {
            if (!is_root) sb_append_indent(&enc->sb, depth, enc->indent_size);
            emit_field_array_header(enc, NULL, v, delim);
        } else if (is_tabular_array(v)) {
            if (!is_root) sb_append_indent(&enc->sb, depth, enc->indent_size);
            emit_field_array_header(enc, NULL, v, delim);
            emit_tabular_rows(enc, v, (is_root ? 0 : depth) + 1, delim);
        } else {
            if (!is_root) sb_append_indent(&enc->sb, depth, enc->indent_size);
            emit_array_header(enc, v->u.arr.count, delim);
            sb_appendc(&enc->sb, ':');
            emit_list_items(enc, v, (is_root ? 0 : depth) + 1, delim);
        }
        break;

    case JSON_OBJECT:
        if (v->u.obj.count > 0) {
            encode_object_fields(enc, v, is_root ? 0 : depth);
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
