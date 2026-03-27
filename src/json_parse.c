/*
 * json_parse.c - JSON parser for TOON CLI
 *
 * Parses standard JSON into JsonValue tree.
 * Used for json-to-toon encoding path.
 */

#include "toon.h"
#include <ctype.h>

typedef struct {
    const char *src;
    int pos;
    const char *err;
} JParser;

static void jp_skip_ws(JParser *jp)
{
    while (jp->src[jp->pos] == ' ' || jp->src[jp->pos] == '\t' ||
           jp->src[jp->pos] == '\n' || jp->src[jp->pos] == '\r')
        jp->pos++;
}

static char jp_peek(JParser *jp)
{
    jp_skip_ws(jp);
    return jp->src[jp->pos];
}

static char jp_next(JParser *jp)
{
    jp_skip_ws(jp);
    return jp->src[jp->pos++];
}

static toon_bool jp_match(JParser *jp, const char *s)
{
    int len = strlen(s);
    if (strncmp(jp->src + jp->pos, s, len) == 0) {
        jp->pos += len;
        return TRUE;
    }
    return FALSE;
}

static JsonValue *jp_parse_value(JParser *jp);

static JsonValue *jp_parse_string(JParser *jp)
{
    StrBuf sb;
    char c;
    /* skip opening quote */
    jp->pos++;
    sb_init(&sb);

    while ((c = jp->src[jp->pos]) != '\0') {
        if (c == '"') {
            jp->pos++;
            {
                JsonValue *v = json_new_string(sb.data);
                sb_free(&sb);
                return v;
            }
        }
        if (c == '\\') {
            jp->pos++;
            c = jp->src[jp->pos];
            switch (c) {
            case '"': sb_appendc(&sb, '"'); break;
            case '\\': sb_appendc(&sb, '\\'); break;
            case '/': sb_appendc(&sb, '/'); break;
            case 'b': sb_appendc(&sb, '\b'); break;
            case 'f': sb_appendc(&sb, '\f'); break;
            case 'n': sb_appendc(&sb, '\n'); break;
            case 'r': sb_appendc(&sb, '\r'); break;
            case 't': sb_appendc(&sb, '\t'); break;
            case 'u': {
                /* Parse 4 hex digits - just pass through as-is for now */
                /* SAS/C doesn't have great Unicode support */
                int i;
                unsigned int cp = 0;
                jp->pos++;
                for (i = 0; i < 4 && jp->src[jp->pos]; i++, jp->pos++) {
                    char h = jp->src[jp->pos];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= h - '0';
                    else if (h >= 'a' && h <= 'f') cp |= 10 + h - 'a';
                    else if (h >= 'A' && h <= 'F') cp |= 10 + h - 'A';
                }
                /* Simple UTF-8 encoding */
                if (cp < 0x80) {
                    sb_appendc(&sb, (char)cp);
                } else if (cp < 0x800) {
                    sb_appendc(&sb, (char)(0xC0 | (cp >> 6)));
                    sb_appendc(&sb, (char)(0x80 | (cp & 0x3F)));
                } else {
                    sb_appendc(&sb, (char)(0xE0 | (cp >> 12)));
                    sb_appendc(&sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
                    sb_appendc(&sb, (char)(0x80 | (cp & 0x3F)));
                }
                continue; /* already advanced pos */
            }
            default:
                sb_appendc(&sb, c);
                break;
            }
        } else {
            sb_appendc(&sb, c);
        }
        jp->pos++;
    }

    jp->err = "Unterminated string";
    sb_free(&sb);
    return NULL;
}

static JsonValue *jp_parse_number(JParser *jp)
{
    const char *start = jp->src + jp->pos;
    char *end;
    double val;

    /* Advance past number chars */
    if (jp->src[jp->pos] == '-') jp->pos++;
    while (isdigit((unsigned char)jp->src[jp->pos])) jp->pos++;
    if (jp->src[jp->pos] == '.') {
        jp->pos++;
        while (isdigit((unsigned char)jp->src[jp->pos])) jp->pos++;
    }
    if (jp->src[jp->pos] == 'e' || jp->src[jp->pos] == 'E') {
        jp->pos++;
        if (jp->src[jp->pos] == '+' || jp->src[jp->pos] == '-') jp->pos++;
        while (isdigit((unsigned char)jp->src[jp->pos])) jp->pos++;
    }

    val = strtod(start, &end);
    return json_new_number(val);
}

static JsonValue *jp_parse_array(JParser *jp)
{
    JsonValue *arr = json_new_array();
    /* skip [ */
    jp->pos++;

    if (jp_peek(jp) == ']') {
        jp->pos++;
        return arr;
    }

    for (;;) {
        JsonValue *item = jp_parse_value(jp);
        if (!item) {
            json_free(arr);
            return NULL;
        }
        json_array_push(arr, item);

        if (jp_peek(jp) == ',') {
            jp->pos++;
        } else {
            break;
        }
    }

    if (jp_next(jp) != ']') {
        jp->err = "Expected ']'";
        json_free(arr);
        return NULL;
    }
    return arr;
}

static JsonValue *jp_parse_object(JParser *jp)
{
    JsonValue *obj = json_new_object();
    /* skip { */
    jp->pos++;

    if (jp_peek(jp) == '}') {
        jp->pos++;
        return obj;
    }

    for (;;) {
        JsonValue *key_val;
        JsonValue *val;

        if (jp_peek(jp) != '"') {
            jp->err = "Expected string key";
            json_free(obj);
            return NULL;
        }
        key_val = jp_parse_string(jp);
        if (!key_val) {
            json_free(obj);
            return NULL;
        }

        if (jp_next(jp) != ':') {
            jp->err = "Expected ':'";
            json_free(key_val);
            json_free(obj);
            return NULL;
        }

        val = jp_parse_value(jp);
        if (!val) {
            json_free(key_val);
            json_free(obj);
            return NULL;
        }

        json_object_set(obj, key_val->u.sval, val);
        json_free(key_val);

        if (jp_peek(jp) == ',') {
            jp->pos++;
        } else {
            break;
        }
    }

    if (jp_next(jp) != '}') {
        jp->err = "Expected '}'";
        json_free(obj);
        return NULL;
    }
    return obj;
}

static JsonValue *jp_parse_value(JParser *jp)
{
    char c = jp_peek(jp);

    if (c == '"') return jp_parse_string(jp);
    if (c == '{') return jp_parse_object(jp);
    if (c == '[') return jp_parse_array(jp);
    if (c == '-' || (c >= '0' && c <= '9')) return jp_parse_number(jp);
    if (jp_match(jp, "true")) return json_new_bool(TRUE);
    if (jp_match(jp, "false")) return json_new_bool(FALSE);
    if (jp_match(jp, "null")) return json_new_null();

    jp->err = "Unexpected character";
    return NULL;
}

JsonValue *json_parse(const char *input, const char **errout)
{
    JParser jp;
    JsonValue *v;

    jp.src = input;
    jp.pos = 0;
    jp.err = NULL;

    v = jp_parse_value(&jp);
    if (!v && errout) {
        *errout = jp.err ? jp.err : "Parse error";
    }
    return v;
}
