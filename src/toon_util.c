/*
 * toon_util.c - Utility functions for TOON
 */

#include "toon.h"
#include <ctype.h>
#include <math.h>

/* ---- JsonValue constructors ---- */

JsonValue *json_new_null(void)
{
    JsonValue *v = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (v) v->type = JSON_NULL;
    return v;
}

JsonValue *json_new_bool(toon_bool b)
{
    JsonValue *v = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (v) { v->type = JSON_BOOL; v->u.bval = b; }
    return v;
}

JsonValue *json_new_number(double n)
{
    JsonValue *v = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (v) { v->type = JSON_NUMBER; v->u.nval = n; }
    return v;
}

JsonValue *json_new_string(const char *s)
{
    JsonValue *v = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (v) {
        v->type = JSON_STRING;
        v->u.sval = (char *)malloc(strlen(s) + 1);
        if (v->u.sval) strcpy(v->u.sval, s);
    }
    return v;
}

JsonValue *json_new_string_len(const char *s, int len)
{
    JsonValue *v = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (v) {
        v->type = JSON_STRING;
        v->u.sval = (char *)malloc(len + 1);
        if (v->u.sval) {
            memcpy(v->u.sval, s, len);
            v->u.sval[len] = '\0';
        }
    }
    return v;
}

JsonValue *json_new_array(void)
{
    JsonValue *v = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (v) {
        v->type = JSON_ARRAY;
        v->u.arr.capacity = 8;
        v->u.arr.items = (JsonValue **)calloc(8, sizeof(JsonValue *));
    }
    return v;
}

JsonValue *json_new_object(void)
{
    JsonValue *v = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (v) {
        v->type = JSON_OBJECT;
        v->u.obj.capacity = 8;
        v->u.obj.pairs = (JsonPair *)calloc(8, sizeof(JsonPair));
    }
    return v;
}

void json_array_push(JsonValue *arr, JsonValue *item)
{
    JsonArray *a;
    if (!arr || arr->type != JSON_ARRAY) return;
    a = &arr->u.arr;
    if (a->count >= a->capacity) {
        int newcap = a->capacity * 2;
        JsonValue **newitems = (JsonValue **)realloc(a->items,
            newcap * sizeof(JsonValue *));
        if (!newitems) return;
        a->items = newitems;
        a->capacity = newcap;
    }
    a->items[a->count++] = item;
}

void json_object_set(JsonValue *obj, const char *key, JsonValue *val)
{
    JsonObject *o;
    if (!obj || obj->type != JSON_OBJECT) return;
    o = &obj->u.obj;
    if (o->count >= o->capacity) {
        int newcap = o->capacity * 2;
        JsonPair *newpairs = (JsonPair *)realloc(o->pairs,
            newcap * sizeof(JsonPair));
        if (!newpairs) return;
        o->pairs = newpairs;
        o->capacity = newcap;
    }
    o->pairs[o->count].key = (char *)malloc(strlen(key) + 1);
    if (o->pairs[o->count].key) strcpy(o->pairs[o->count].key, key);
    o->pairs[o->count].value = val;
    o->count++;
}

void json_free(JsonValue *v)
{
    int i;
    if (!v) return;
    switch (v->type) {
    case JSON_STRING:
        free(v->u.sval);
        break;
    case JSON_ARRAY:
        for (i = 0; i < v->u.arr.count; i++)
            json_free(v->u.arr.items[i]);
        free(v->u.arr.items);
        break;
    case JSON_OBJECT:
        for (i = 0; i < v->u.obj.count; i++) {
            free(v->u.obj.pairs[i].key);
            json_free(v->u.obj.pairs[i].value);
        }
        free(v->u.obj.pairs);
        break;
    default:
        break;
    }
    free(v);
}

/* ---- String Buffer ---- */

void sb_init(StrBuf *sb)
{
    sb->cap = 256;
    sb->len = 0;
    sb->data = (char *)malloc(sb->cap);
    if (sb->data) sb->data[0] = '\0';
}

static void sb_grow(StrBuf *sb, int need)
{
    if (sb->len + need + 1 > sb->cap) {
        int newcap = sb->cap * 2;
        char *nd;
        while (newcap < sb->len + need + 1)
            newcap *= 2;
        nd = (char *)realloc(sb->data, newcap);
        if (nd) {
            sb->data = nd;
            sb->cap = newcap;
        }
    }
}

void sb_append(StrBuf *sb, const char *s)
{
    int slen = strlen(s);
    sb_grow(sb, slen);
    memcpy(sb->data + sb->len, s, slen);
    sb->len += slen;
    sb->data[sb->len] = '\0';
}

void sb_appendn(StrBuf *sb, const char *s, int n)
{
    sb_grow(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_appendc(StrBuf *sb, char c)
{
    sb_grow(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

void sb_append_indent(StrBuf *sb, int depth, int indent_size)
{
    int i, total = depth * indent_size;
    sb_grow(sb, total);
    for (i = 0; i < total; i++)
        sb->data[sb->len++] = ' ';
    sb->data[sb->len] = '\0';
}

char *sb_detach(StrBuf *sb)
{
    char *r = sb->data;
    sb->data = NULL;
    sb->len = sb->cap = 0;
    return r;
}

void sb_free(StrBuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

/* ---- Number formatting ---- */

/*
 * Manual double-to-string conversion for SAS/C scnb.lib,
 * which does not support %g/%e/%f in printf.
 * Uses FPU arithmetic to decompose the number.
 */
void format_number(double val, char *buf, int bufsize)
{
    double intpart;
    double fracpart;
    long lval;
    int neg = 0;
    int pos = 0;
    int i;

    /* Negative zero -> 0 */
    if (val == 0.0) {
        strcpy(buf, "0");
        return;
    }

    /* Handle negative */
    if (val < 0.0) {
        neg = 1;
        val = -val;
    }

    /* Check if integer (fits in long) */
    intpart = floor(val);
    fracpart = val - intpart;

    if (fracpart < 1e-17 * (intpart > 1.0 ? intpart : 1.0) && intpart <= 2147483647.0) {
        lval = (long)intpart;
        if (neg) {
            sprintf(buf, "-%ld", lval);
        } else {
            sprintf(buf, "%ld", lval);
        }
        return;
    }

    /* Floating point: emit integer part + fractional digits manually.
       IEEE 754 double has ~15.9 significant digits. We use 16 total. */
    {
        char intbuf[32];
        char fracbuf[24];
        int ipos = 0;
        int fpos = 0;
        int max_sigfigs = 16;
        int int_digits;
        int frac_digits;
        double frac;
        long ipart;

        /* Integer part */
        if (intpart == 0.0) {
            intbuf[ipos++] = '0';
        } else if (intpart <= 2147483647.0) {
            ipart = (long)intpart;
            sprintf(intbuf, "%ld", ipart);
            ipos = strlen(intbuf);
        } else {
            char digits[32];
            int nd = 0;
            double rem = intpart;
            while (rem >= 1.0 && nd < 30) {
                double d = floor(rem / 10.0);
                int digit = (int)(rem - d * 10.0 + 0.5);
                if (digit >= 10) { digit = 0; d += 1.0; }
                digits[nd++] = '0' + digit;
                rem = d;
            }
            for (i = nd - 1; i >= 0; i--)
                intbuf[ipos++] = digits[i];
        }
        intbuf[ipos] = '\0';

        /* Count significant digits already used by integer part */
        int_digits = ipos;
        if (int_digits == 1 && intbuf[0] == '0')
            int_digits = 0; /* leading zero doesn't count */

        /* Fractional part - limit to remaining significant digits */
        frac_digits = max_sigfigs - int_digits;
        if (frac_digits < 1) frac_digits = 1;
        if (frac_digits > 20) frac_digits = 20;

        frac = fracpart;
        for (i = 0; i < frac_digits && frac > 0.0; i++) {
            int digit;
            frac *= 10.0;
            digit = (int)frac;
            if (digit > 9) digit = 9;
            fracbuf[fpos++] = '0' + digit;
            frac -= (double)digit;
            /* Round up if remaining fraction is close to 1 */
            if (frac > 0.9999999 && i + 1 >= frac_digits) {
                /* Carry: increment last digit */
                fracbuf[fpos - 1]++;
                break;
            }
        }
        fracbuf[fpos] = '\0';

        /* Strip trailing zeros from fractional part */
        while (fpos > 0 && fracbuf[fpos - 1] == '0') {
            fpos--;
            fracbuf[fpos] = '\0';
        }

        /* Assemble result */
        pos = 0;
        if (neg) buf[pos++] = '-';
        for (i = 0; intbuf[i] && pos < bufsize - 1; i++)
            buf[pos++] = intbuf[i];
        if (fpos > 0 && pos < bufsize - 2) {
            buf[pos++] = '.';
            for (i = 0; fracbuf[i] && pos < bufsize - 1; i++)
                buf[pos++] = fracbuf[i];
        }
        buf[pos] = '\0';
    }
}

/* ---- Quoting helpers ---- */

static toon_bool is_numeric_like(const char *s)
{
    const char *p = s;
    if (!*p) return FALSE;

    /* Leading zero decimals like "05" */
    if (*p == '0' && p[1] >= '0' && p[1] <= '9') return TRUE;

    /* Match /^-?\d+(?:\.\d+)?(?:e[+-]?\d+)?$/i */
    if (*p == '-') p++;
    if (!isdigit((unsigned char)*p)) return FALSE;
    while (isdigit((unsigned char)*p)) p++;
    if (*p == '.') {
        p++;
        if (!isdigit((unsigned char)*p)) return FALSE;
        while (isdigit((unsigned char)*p)) p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!isdigit((unsigned char)*p)) return FALSE;
        while (isdigit((unsigned char)*p)) p++;
    }
    return (*p == '\0') ? TRUE : FALSE;
}

toon_bool toon_needs_quote(const char *s, char delim)
{
    const char *p;

    /* Empty string */
    if (!s || !*s) return TRUE;

    /* Leading/trailing whitespace */
    if (*s == ' ' || *s == '\t') return TRUE;
    {
        int len = strlen(s);
        if (s[len-1] == ' ' || s[len-1] == '\t') return TRUE;
    }

    /* Equals true, false, null */
    if (strcmp(s, "true") == 0 || strcmp(s, "false") == 0 ||
        strcmp(s, "null") == 0) return TRUE;

    /* Numeric-like */
    if (is_numeric_like(s)) return TRUE;

    /* Starts with or equals "-" */
    if (*s == '-') return TRUE;

    /* Check for special characters */
    for (p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ':' || c == '"' || c == '\\' ||
            c == '[' || c == ']' || c == '{' || c == '}' ||
            c == '\n' || c == '\r' || c == '\t')
            return TRUE;
        /* Contains the delimiter */
        if (delim && c == (unsigned char)delim)
            return TRUE;
    }

    return FALSE;
}

toon_bool toon_valid_unquoted_key(const char *s)
{
    const char *p;
    if (!s || !*s) return FALSE;
    /* First char: [A-Za-z_] */
    if (!isalpha((unsigned char)*s) && *s != '_') return FALSE;
    /* Rest: [A-Za-z0-9_.] */
    for (p = s + 1; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!isalnum(c) && c != '_' && c != '.') return FALSE;
    }
    return TRUE;
}

/* ---- File I/O ---- */

char *toon_read_file(const char *path)
{
    FILE *f;
    long size;
    char *buf;

    f = fopen(path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    buf = (char *)malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }

    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

char *toon_read_stdin(void)
{
    StrBuf sb;
    char buf[4096];
    int n;

    sb_init(&sb);
    while ((n = fread(buf, 1, sizeof(buf) - 1, stdin)) > 0) {
        buf[n] = '\0';
        sb_append(&sb, buf);
    }
    return sb_detach(&sb);
}

int toon_write_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(data, f);
    fclose(f);
    return 0;
}
