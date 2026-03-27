/*
 * json_emit.c - JSON emitter for TOON CLI
 *
 * Converts JsonValue tree to JSON text.
 * Used for toon-to-json decoding output.
 */

#include "toon.h"

static void emit_value(StrBuf *sb, const JsonValue *v, int depth);

static void emit_string(StrBuf *sb, const char *s)
{
    const char *p;
    sb_appendc(sb, '"');
    for (p = s; *p; p++) {
        switch (*p) {
        case '"':  sb_append(sb, "\\\""); break;
        case '\\': sb_append(sb, "\\\\"); break;
        case '\n': sb_append(sb, "\\n"); break;
        case '\r': sb_append(sb, "\\r"); break;
        case '\t': sb_append(sb, "\\t"); break;
        case '\b': sb_append(sb, "\\b"); break;
        case '\f': sb_append(sb, "\\f"); break;
        default:
            if ((unsigned char)*p < 0x20) {
                char hex[8];
                sprintf(hex, "\\u%04x", (unsigned char)*p);
                sb_append(sb, hex);
            } else {
                sb_appendc(sb, *p);
            }
            break;
        }
    }
    sb_appendc(sb, '"');
}

static void emit_value(StrBuf *sb, const JsonValue *v, int depth)
{
    int i;
    char nbuf[64];

    if (!v) {
        sb_append(sb, "null");
        return;
    }

    switch (v->type) {
    case JSON_NULL:
        sb_append(sb, "null");
        break;
    case JSON_BOOL:
        sb_append(sb, v->u.bval ? "true" : "false");
        break;
    case JSON_NUMBER:
        format_number(v->u.nval, nbuf, sizeof(nbuf));
        sb_append(sb, nbuf);
        break;
    case JSON_STRING:
        emit_string(sb, v->u.sval);
        break;
    case JSON_ARRAY:
        sb_appendc(sb, '[');
        for (i = 0; i < v->u.arr.count; i++) {
            if (i > 0) sb_appendc(sb, ',');
            emit_value(sb, v->u.arr.items[i], depth + 1);
        }
        sb_appendc(sb, ']');
        break;
    case JSON_OBJECT:
        sb_appendc(sb, '{');
        for (i = 0; i < v->u.obj.count; i++) {
            if (i > 0) sb_appendc(sb, ',');
            emit_string(sb, v->u.obj.pairs[i].key);
            sb_appendc(sb, ':');
            emit_value(sb, v->u.obj.pairs[i].value, depth + 1);
        }
        sb_appendc(sb, '}');
        break;
    }
}

char *json_emit(const JsonValue *v)
{
    StrBuf sb;
    sb_init(&sb);
    emit_value(&sb, v, 0);
    return sb_detach(&sb);
}
