/*
 * toon_decode.c - TOON format decoder
 *
 * Decodes TOON text into JsonValue tree.
 * Implements TOON Spec v3.0 sections 4-14.
 */

#include "toon.h"
#include <ctype.h>

/* ---- Line representation ---- */

typedef struct {
    const char *text;    /* pointer into source (not owned) */
    int len;             /* length of text content (no newline) */
    int depth;           /* indentation depth */
    int spaces;          /* raw leading space count */
    toon_bool blank;     /* is blank line */
} Line;

/* ---- Decoder state ---- */

typedef struct {
    Line *lines;
    int nlines;
    int pos;             /* current line index */
    int indent_size;
    toon_bool strict;
    const char *err;
} Decoder;

/* ---- Forward declarations ---- */
static JsonValue *decode_value_at_depth(Decoder *d, int depth);
static JsonValue *decode_object_at_depth(Decoder *d, int depth);

/* ---- Line scanner ---- */

static void scan_lines(Decoder *d, const char *input, int indent_size)
{
    const char *p = input;
    int cap = 64;
    int count = 0;

    d->lines = (Line *)calloc(cap, sizeof(Line));

    while (*p) {
        /* const char *sol = p; */
        int spaces = 0;
        int text_len;
        const char *eol;

        /* Count leading spaces */
        while (*p == ' ') { spaces++; p++; }

        /* Find end of line */
        eol = p;
        while (*eol && *eol != '\n') eol++;
        text_len = (int)(eol - p);

        /* Grow array if needed */
        if (count >= cap) {
            cap *= 2;
            d->lines = (Line *)realloc(d->lines, cap * sizeof(Line));
        }

        d->lines[count].spaces = spaces;
        d->lines[count].depth = indent_size > 0 ? spaces / indent_size : 0;
        d->lines[count].text = p;
        d->lines[count].len = text_len;
        d->lines[count].blank = (text_len == 0);
        count++;

        p = eol;
        if (*p == '\n') p++;
    }

    d->nlines = count;
}

/* ---- Helpers ---- */

static toon_bool at_end(Decoder *d)
{
    return d->pos >= d->nlines;
}

static Line *cur_line(Decoder *d)
{
    if (d->pos < d->nlines)
        return &d->lines[d->pos];
    return NULL;
}

/* Skip blank lines */
static void skip_blanks(Decoder *d)
{
    while (d->pos < d->nlines && d->lines[d->pos].blank)
        d->pos++;
}

/* Get a NUL-terminated copy of a line's text */
static char *line_strdup(Line *ln)
{
    char *s = (char *)malloc(ln->len + 1);
    memcpy(s, ln->text, ln->len);
    s[ln->len] = '\0';
    return s;
}

/* ---- Primitive parsing (Section 4) ---- */

static toon_bool is_number_token(const char *s)
{
    const char *p = s;

    /* Leading zero check - "05" etc are strings */
    if (*p == '-') p++;
    if (*p == '0' && p[1] >= '0' && p[1] <= '9') return FALSE;

    p = s;
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

static JsonValue *parse_primitive_token(const char *token)
{
    /* Booleans and null */
    if (strcmp(token, "true") == 0) return json_new_bool(TRUE);
    if (strcmp(token, "false") == 0) return json_new_bool(FALSE);
    if (strcmp(token, "null") == 0) return json_new_null();

    /* Number */
    if (is_number_token(token)) {
        double val = strtod(token, NULL);
        /* Negative zero -> 0 */
        if (val == 0.0) val = 0.0;
        return json_new_number(val);
    }

    /* String */
    return json_new_string(token);
}

/* ---- String unescaping (Section 7.1) ---- */

static char *unescape_string(const char *s, int len, Decoder *d)
{
    StrBuf sb;
    int i;
    sb_init(&sb);

    for (i = 0; i < len; i++) {
        if (s[i] == '\\') {
            i++;
            if (i >= len) {
                d->err = "Unterminated escape";
                sb_free(&sb);
                return NULL;
            }
            switch (s[i]) {
            case '\\': sb_appendc(&sb, '\\'); break;
            case '"':  sb_appendc(&sb, '"'); break;
            case 'n':  sb_appendc(&sb, '\n'); break;
            case 'r':  sb_appendc(&sb, '\r'); break;
            case 't':  sb_appendc(&sb, '\t'); break;
            default:
                if (d->strict) {
                    d->err = "Invalid escape sequence";
                    sb_free(&sb);
                    return NULL;
                }
                sb_appendc(&sb, s[i]);
                break;
            }
        } else {
            sb_appendc(&sb, s[i]);
        }
    }
    return sb_detach(&sb);
}

/* Parse a quoted string starting at s[0]=='"'.
   Returns unescaped content. Sets *endpos to char after closing quote. */
static char *parse_quoted(const char *s, int *endpos, Decoder *d)
{
    int i = 1;  /* skip opening quote */
    StrBuf sb;
    sb_init(&sb);

    while (s[i] && s[i] != '"') {
        if (s[i] == '\\') {
            i++;
            if (!s[i]) {
                d->err = "Unterminated string";
                sb_free(&sb);
                return NULL;
            }
            switch (s[i]) {
            case '\\': sb_appendc(&sb, '\\'); break;
            case '"':  sb_appendc(&sb, '"'); break;
            case 'n':  sb_appendc(&sb, '\n'); break;
            case 'r':  sb_appendc(&sb, '\r'); break;
            case 't':  sb_appendc(&sb, '\t'); break;
            default:
                if (d->strict) {
                    d->err = "Invalid escape sequence";
                    sb_free(&sb);
                    return NULL;
                }
                sb_appendc(&sb, s[i]);
                break;
            }
        } else {
            sb_appendc(&sb, s[i]);
        }
        i++;
    }
    if (s[i] != '"') {
        d->err = "Unterminated string";
        sb_free(&sb);
        return NULL;
    }
    *endpos = i + 1;  /* past closing quote */
    return sb_detach(&sb);
}

/* ---- Header parsing (Section 6) ---- */

typedef struct {
    char *key;          /* NULL for root arrays */
    int length;         /* declared N */
    char delim;         /* ',' '\t' or '|' */
    char **fields;      /* field names for tabular, or NULL */
    int nfields;
    const char *inline_values; /* pointer to inline values after ": " */
    toon_bool has_inline;
} ArrayHeader;

static void free_header(ArrayHeader *h)
{
    int i;
    free(h->key);
    if (h->fields) {
        for (i = 0; i < h->nfields; i++)
            free(h->fields[i]);
        free(h->fields);
    }
}

/* Try to parse a line as an array header.
   Returns TRUE if successful, fills *hdr. */
static toon_bool parse_array_header(const char *line, int linelen,
                                    ArrayHeader *hdr, Decoder *d)
{
    int pos = 0;
    int bracket_start;
    char delim = ',';
    int n = 0;

    memset(hdr, 0, sizeof(*hdr));
    hdr->delim = ',';

    /* Parse optional key */
    if (line[pos] == '"') {
        /* Quoted key */
        int endq;
        hdr->key = parse_quoted(line + pos, &endq, d);
        if (!hdr->key) return FALSE;
        pos += endq;
    } else if (line[pos] != '[') {
        /* Unquoted key - read until '[' */
        int kstart = pos;
        while (pos < linelen && line[pos] != '[') pos++;
        if (pos >= linelen || line[pos] != '[') return FALSE;
        hdr->key = (char *)malloc(pos - kstart + 1);
        memcpy(hdr->key, line + kstart, pos - kstart);
        hdr->key[pos - kstart] = '\0';
    }

    /* Must have '[' */
    if (pos >= linelen || line[pos] != '[') {
        free(hdr->key); hdr->key = NULL;
        return FALSE;
    }
    pos++; /* skip '[' */
    bracket_start = pos;

    /* Parse integer N */
    if (pos >= linelen || !isdigit((unsigned char)line[pos])) {
        free(hdr->key); hdr->key = NULL;
        return FALSE;
    }
    n = 0;
    while (pos < linelen && isdigit((unsigned char)line[pos])) {
        n = n * 10 + (line[pos] - '0');
        pos++;
    }
    hdr->length = n;

    /* Check for delimiter symbol before ']' */
    if (pos < linelen && line[pos] == '\t') {
        delim = '\t';
        pos++;
    } else if (pos < linelen && line[pos] == '|') {
        delim = '|';
        pos++;
    }
    hdr->delim = delim;

    /* Must have ']' */
    if (pos >= linelen || line[pos] != ']') {
        free(hdr->key); hdr->key = NULL;
        return FALSE;
    }
    pos++; /* skip ']' */

    /* Skip whitespace between ] and { or : */
    while (pos < linelen && line[pos] == ' ') pos++;

    /* Check for non-whitespace that isn't '{' or ':' - fallthrough to key-value */
    if (pos < linelen && line[pos] != '{' && line[pos] != ':') {
        free(hdr->key); hdr->key = NULL;
        return FALSE;
    }

    /* Optional fields segment {f1,f2,...} */
    if (pos < linelen && line[pos] == '{') {
        int fcap = 8;
        int fcount = 0;
        char **fields;
        pos++; /* skip '{' */

        fields = (char **)calloc(fcap, sizeof(char *));

        while (pos < linelen && line[pos] != '}') {
            int fstart;
            int flen;

            /* Skip whitespace */
            while (pos < linelen && line[pos] == ' ') pos++;

            if (line[pos] == '"') {
                /* Quoted field name */
                int endq;
                char *fname = parse_quoted(line + pos, &endq, d);
                if (!fname) {
                    int fi;
                    for (fi = 0; fi < fcount; fi++) free(fields[fi]);
                    free(fields);
                    free(hdr->key); hdr->key = NULL;
                    return FALSE;
                }
                pos += endq;
                if (fcount >= fcap) {
                    fcap *= 2;
                    fields = (char **)realloc(fields, fcap * sizeof(char *));
                }
                fields[fcount++] = fname;
            } else {
                /* Unquoted field name - read until delimiter or '}' */
                fstart = pos;
                while (pos < linelen && line[pos] != delim &&
                       line[pos] != '}') pos++;
                flen = pos - fstart;
                /* Trim trailing space */
                while (flen > 0 && line[fstart + flen - 1] == ' ') flen--;
                if (fcount >= fcap) {
                    fcap *= 2;
                    fields = (char **)realloc(fields, fcap * sizeof(char *));
                }
                fields[fcount] = (char *)malloc(flen + 1);
                memcpy(fields[fcount], line + fstart, flen);
                fields[fcount][flen] = '\0';
                fcount++;
            }

            /* Skip delimiter between fields */
            if (pos < linelen && line[pos] == delim) pos++;
        }

        if (pos < linelen && line[pos] == '}') pos++;

        hdr->fields = fields;
        hdr->nfields = fcount;

        /* Skip whitespace */
        while (pos < linelen && line[pos] == ' ') pos++;
    }

    /* Must end with ':' */
    if (pos >= linelen || line[pos] != ':') {
        free_header(hdr);
        memset(hdr, 0, sizeof(*hdr));
        return FALSE;
    }
    pos++; /* skip ':' */

    /* Check for inline values */
    if (pos < linelen && line[pos] == ' ') {
        pos++; /* skip single space after colon */
        hdr->inline_values = line + pos;
        hdr->has_inline = TRUE;
    } else {
        hdr->has_inline = FALSE;
    }

    return TRUE;
}

/* ---- Delimited value splitting ---- */

/* Split a delimited string into tokens. Returns array of strings.
   Sets *count. Handles quoted values. */
static char **split_delimited(const char *s, int slen, char delim,
                              int *count, Decoder *d)
{
    int cap = 8;
    int cnt = 0;
    char **tokens = (char **)calloc(cap, sizeof(char *));
    int pos = 0;

    while (pos <= slen) {
        StrBuf sb;
        sb_init(&sb);

        /* Skip leading whitespace */
        while (pos < slen && s[pos] == ' ') pos++;

        if (pos < slen && s[pos] == '"') {
            /* Quoted value */
            int endq;
            char *qval = parse_quoted(s + pos, &endq, d);
            if (!qval) {
                sb_free(&sb);
                /* cleanup */
                {
                    int i;
                    for (i = 0; i < cnt; i++) free(tokens[i]);
                    free(tokens);
                }
                return NULL;
            }
            sb_append(&sb, qval);
            free(qval);
            pos += endq;

            /* Skip trailing whitespace */
            while (pos < slen && s[pos] == ' ') pos++;

            /* Expect delimiter or end */
            if (pos < slen && s[pos] == delim) pos++;
        } else {
            /* Unquoted value - read until delimiter */
            int vstart = pos;
            while (pos < slen && s[pos] != delim) pos++;
            {
                int vlen = pos - vstart;
                /* Trim trailing whitespace */
                while (vlen > 0 && s[vstart + vlen - 1] == ' ') vlen--;
                sb_appendn(&sb, s + vstart, vlen);
            }
            if (pos < slen && s[pos] == delim) pos++;
            else pos = slen + 1; /* end */
        }

        if (cnt >= cap) {
            cap *= 2;
            tokens = (char **)realloc(tokens, cap * sizeof(char *));
        }
        tokens[cnt++] = sb_detach(&sb);
    }

    *count = cnt;
    return tokens;
}

/* ---- Key-value line parsing ---- */

/* Parse "key: value" or "key:" from a line.
   Returns key (allocated), sets *value_start to content after ": ".
   *value_start is NULL if line is just "key:" */
static char *parse_key_value(const char *line, int linelen,
                             const char **value_start, int *value_len,
                             Decoder *d)
{
    int pos = 0;
    char *key;
    int klen;

    *value_start = NULL;
    *value_len = 0;

    if (line[pos] == '"') {
        /* Quoted key */
        int endq;
        key = parse_quoted(line + pos, &endq, d);
        if (!key) return NULL;
        pos += endq;
    } else {
        /* Unquoted key - read until ':' or '[' */
        int kstart = pos;
        while (pos < linelen && line[pos] != ':' && line[pos] != '[') pos++;
        klen = pos - kstart;
        key = (char *)malloc(klen + 1);
        memcpy(key, line + kstart, klen);
        key[klen] = '\0';
    }

    /* Must have ':' */
    if (pos >= linelen || line[pos] != ':') {
        /* Could be an array header - let caller handle */
        free(key);
        return NULL;
    }
    pos++; /* skip ':' */

    /* Value after colon */
    if (pos < linelen && line[pos] == ' ') {
        pos++; /* skip single space */
        *value_start = line + pos;
        *value_len = linelen - pos;
    } else if (pos < linelen) {
        /* No space - still read value */
        *value_start = line + pos;
        *value_len = linelen - pos;
    }

    return key;
}

/* ---- Parse a value token (quoted or unquoted primitive) ---- */

static JsonValue *parse_value_token(const char *s, int len, Decoder *d)
{
    char *tmp;
    JsonValue *v;

    /* Trim whitespace */
    while (len > 0 && *s == ' ') { s++; len--; }
    while (len > 0 && s[len-1] == ' ') len--;

    if (len == 0) return json_new_string("");

    /* Quoted string */
    if (*s == '"') {
        int endq;
        char *unesc = parse_quoted(s, &endq, d);
        if (!unesc) return NULL;
        v = json_new_string(unesc);
        free(unesc);
        return v;
    }

    /* Unquoted - make NUL-terminated copy */
    tmp = (char *)malloc(len + 1);
    memcpy(tmp, s, len);
    tmp[len] = '\0';

    v = parse_primitive_token(tmp);
    free(tmp);
    return v;
}

/* ---- Tabular row disambiguation (Section 9.3) ---- */

/* Returns TRUE if line looks like a tabular row (has delimiter before colon) */
static toon_bool is_tabular_row(const char *line, int len, char delim)
{
    int first_delim = -1;
    int first_colon = -1;
    int i;
    toon_bool in_quotes = FALSE;

    for (i = 0; i < len; i++) {
        if (line[i] == '"') {
            in_quotes = !in_quotes;
            /* skip to closing quote */
            if (in_quotes) {
                i++;
                while (i < len && line[i] != '"') {
                    if (line[i] == '\\') i++;
                    i++;
                }
            }
            continue;
        }
        if (in_quotes) continue;

        if (first_delim < 0 && line[i] == delim)
            first_delim = i;
        if (first_colon < 0 && line[i] == ':')
            first_colon = i;
    }

    /* No unquoted colon -> row */
    if (first_colon < 0) return TRUE;

    /* Both present: delimiter before colon -> row */
    if (first_delim >= 0 && first_delim < first_colon) return TRUE;

    /* Colon present, no delimiter -> key-value */
    return FALSE;
}

/* ---- List item parsing ---- */

/* Check if a line starts with "- " (list item marker) */
static toon_bool is_list_item(const char *text, int len)
{
    if (len >= 2 && text[0] == '-' && text[1] == ' ')
        return TRUE;
    /* Bare "-" for empty object */
    if (len == 1 && text[0] == '-')
        return TRUE;
    return FALSE;
}

/* ---- Array decoding ---- */

static JsonValue *decode_inline_primitive_array(const char *vals, int vlen,
                                                 ArrayHeader *hdr, Decoder *d)
{
    JsonValue *arr = json_new_array();
    char **tokens;
    int count = 0;
    int i;

    if (hdr->length == 0) return arr;

    tokens = split_delimited(vals, vlen, hdr->delim, &count, d);
    if (!tokens) {
        json_free(arr);
        return NULL;
    }

    if (d->strict && count != hdr->length) {
        d->err = "Array count mismatch";
        for (i = 0; i < count; i++) free(tokens[i]);
        free(tokens);
        json_free(arr);
        return NULL;
    }

    for (i = 0; i < count; i++) {
        JsonValue *item = parse_primitive_token(tokens[i]);
        json_array_push(arr, item);
        free(tokens[i]);
    }
    free(tokens);

    return arr;
}

static JsonValue *decode_tabular_array(Decoder *d, ArrayHeader *hdr,
                                       int content_depth)
{
    JsonValue *arr = json_new_array();
    int row_count = 0;

    while (!at_end(d)) {
        Line *ln;

        skip_blanks(d);
        ln = cur_line(d);
        if (!ln || ln->blank) break;
        if (ln->depth < content_depth) break;
        if (ln->depth != content_depth) break;

        /* Check disambiguation */
        if (!is_tabular_row(ln->text, ln->len, hdr->delim))
            break;

        /* Parse row */
        {
            char **tokens;
            int count = 0;
            int j;
            JsonValue *row_obj;

            tokens = split_delimited(ln->text, ln->len, hdr->delim, &count, d);
            if (!tokens) {
                json_free(arr);
                return NULL;
            }

            if (d->strict && count != hdr->nfields) {
                d->err = "Tabular row width mismatch";
                for (j = 0; j < count; j++) free(tokens[j]);
                free(tokens);
                json_free(arr);
                return NULL;
            }

            row_obj = json_new_object();
            for (j = 0; j < count && j < hdr->nfields; j++) {
                JsonValue *val = parse_primitive_token(tokens[j]);
                json_object_set(row_obj, hdr->fields[j], val);
                free(tokens[j]);
            }
            /* Free any excess tokens */
            for (; j < count; j++) free(tokens[j]);
            free(tokens);

            json_array_push(arr, row_obj);
            row_count++;
        }

        d->pos++;
    }

    if (d->strict && row_count != hdr->length) {
        d->err = "Tabular array count mismatch";
        json_free(arr);
        return NULL;
    }

    return arr;
}

static JsonValue *decode_list_array(Decoder *d, ArrayHeader *hdr,
                                    int content_depth)
{
    JsonValue *arr = json_new_array();
    int item_count = 0;

    while (!at_end(d)) {
        Line *ln;
        const char *after_marker;
        int after_len;

        skip_blanks(d);
        ln = cur_line(d);
        if (!ln || ln->blank) break;
        if (ln->depth < content_depth) break;
        if (ln->depth != content_depth) break;
        if (!is_list_item(ln->text, ln->len)) break;

        /* Strip "- " prefix */
        if (ln->len == 1 && ln->text[0] == '-') {
            /* Bare "-" = empty object */
            json_array_push(arr, json_new_object());
            d->pos++;
            item_count++;
            continue;
        }

        after_marker = ln->text + 2;
        after_len = ln->len - 2;

        /* Check if it's an inline array header: "- [N]: ..." */
        {
            ArrayHeader inner_hdr;
            if (parse_array_header(after_marker, after_len, &inner_hdr, d)) {
                if (inner_hdr.has_inline) {
                    /* Inline primitive array item */
                    int vlen = after_len - (int)(inner_hdr.inline_values - after_marker);
                    JsonValue *inner = decode_inline_primitive_array(
                        inner_hdr.inline_values, vlen, &inner_hdr, d);
                    if (!inner) {
                        free_header(&inner_hdr);
                        json_free(arr);
                        return NULL;
                    }
                    json_array_push(arr, inner);
                    d->pos++;
                } else if (inner_hdr.fields) {
                    /* Tabular array as first field of list-item object */
                    JsonValue *item_obj = json_new_object();
                    JsonValue *tab_arr;
                    d->pos++;
                    tab_arr = decode_tabular_array(d, &inner_hdr,
                                                   content_depth + 2);
                    if (!tab_arr) {
                        free_header(&inner_hdr);
                        json_free(item_obj);
                        json_free(arr);
                        return NULL;
                    }
                    json_object_set(item_obj, inner_hdr.key ? inner_hdr.key : "",
                                    tab_arr);
                    /* Read remaining object fields at depth+1 */
                    while (!at_end(d)) {
                        Line *fln;
                        skip_blanks(d);
                        fln = cur_line(d);
                        if (!fln || fln->blank) break;
                        if (fln->depth != content_depth + 1) break;
                        {
                            JsonValue *sub = decode_value_at_depth(d,
                                content_depth + 1);
                            /* sub was decoded and added by decode_object logic */
                            /* Actually we need to parse key:value here */
                            /* This is handled below in the object-as-list-item path */
                            json_free(sub);
                        }
                    }
                    json_array_push(arr, item_obj);
                } else {
                    /* Nested list array */
                    JsonValue *inner;
                    d->pos++;
                    inner = decode_list_array(d, &inner_hdr, content_depth + 1);
                    if (!inner) {
                        free_header(&inner_hdr);
                        json_free(arr);
                        return NULL;
                    }
                    json_array_push(arr, inner);
                }
                free_header(&inner_hdr);
                item_count++;
                continue;
            }
        }

        /* Check if it's an object-as-list-item: "- key: value" */
        {
            const char *vstart = NULL;
            int vlen = 0;
            char *key = parse_key_value(after_marker, after_len, &vstart, &vlen, d);

            if (key) {
                /* Object as list item */
                JsonValue *item_obj = json_new_object();

                if (vstart && vlen > 0) {
                    /* First field has value on same line */
                    JsonValue *fval = parse_value_token(vstart, vlen, d);
                    if (!fval) {
                        free(key);
                        json_free(item_obj);
                        json_free(arr);
                        return NULL;
                    }
                    json_object_set(item_obj, key, fval);
                } else {
                    /* "- key:" -> nested object/empty value */
                    /* Check for nested content at depth + 2 */
                    d->pos++;
                    {
                        toon_bool has_children = FALSE;
                        Line *nln;
                        skip_blanks(d);
                        nln = cur_line(d);
                        if (nln && nln->depth > content_depth + 1)
                            has_children = TRUE;

                        if (has_children) {
                            /* Nested object under list item field */
                            JsonValue *nested = decode_object_at_depth(d,
                                content_depth + 2);
                            if (!nested) {
                                free(key);
                                json_free(item_obj);
                                json_free(arr);
                                return NULL;
                            }
                            json_object_set(item_obj, key, nested);
                        } else {
                            /* Empty nested object */
                            json_object_set(item_obj, key, json_new_object());
                        }
                    }
                    free(key);

                    /* Read remaining fields at depth + 1 */
                    while (!at_end(d)) {
                        Line *fln;
                        skip_blanks(d);
                        fln = cur_line(d);
                        if (!fln || fln->blank) break;
                        if (fln->depth != content_depth + 1) break;

                        /* Parse as key: value */
                        {
                            char *ftext = line_strdup(fln);
                            const char *fvstart = NULL;
                            int fvlen = 0;
                            ArrayHeader fhdr;
                            char *fkey;

                            /* Check for array header */
                            if (parse_array_header(ftext, fln->len, &fhdr, d)) {
                                JsonValue *farr;
                                if (fhdr.has_inline) {
                                    int fivlen = fln->len - (int)(fhdr.inline_values - ftext);
                                    farr = decode_inline_primitive_array(
                                        fhdr.inline_values, fivlen, &fhdr, d);
                                } else if (fhdr.fields) {
                                    d->pos++;
                                    farr = decode_tabular_array(d, &fhdr,
                                        content_depth + 2);
                                    free(ftext);
                                    if (!farr) {
                                        free_header(&fhdr);
                                        json_free(item_obj);
                                        json_free(arr);
                                        return NULL;
                                    }
                                    json_object_set(item_obj,
                                        fhdr.key ? fhdr.key : "", farr);
                                    free_header(&fhdr);
                                    continue;
                                } else {
                                    d->pos++;
                                    farr = decode_list_array(d, &fhdr,
                                        content_depth + 2);
                                    free(ftext);
                                    if (!farr) {
                                        free_header(&fhdr);
                                        json_free(item_obj);
                                        json_free(arr);
                                        return NULL;
                                    }
                                    json_object_set(item_obj,
                                        fhdr.key ? fhdr.key : "", farr);
                                    free_header(&fhdr);
                                    continue;
                                }
                                if (farr) {
                                    json_object_set(item_obj,
                                        fhdr.key ? fhdr.key : "", farr);
                                }
                                free_header(&fhdr);
                                free(ftext);
                                d->pos++;
                                continue;
                            }

                            fkey = parse_key_value(ftext, fln->len,
                                                   &fvstart, &fvlen, d);
                            if (fkey) {
                                if (fvstart && fvlen > 0) {
                                    JsonValue *fval = parse_value_token(
                                        fvstart, fvlen, d);
                                    json_object_set(item_obj, fkey, fval);
                                    d->pos++;
                                } else {
                                    /* Nested */
                                    d->pos++;
                                    {
                                        Line *nln2;
                                        skip_blanks(d);
                                        nln2 = cur_line(d);
                                        if (nln2 && nln2->depth > content_depth + 1) {
                                            JsonValue *nested2 = decode_object_at_depth(
                                                d, content_depth + 2);
                                            json_object_set(item_obj, fkey, nested2);
                                        } else {
                                            json_object_set(item_obj, fkey,
                                                json_new_object());
                                        }
                                    }
                                }
                                free(fkey);
                            } else {
                                d->pos++;
                            }
                            free(ftext);
                        }
                    }

                    json_array_push(arr, item_obj);
                    item_count++;
                    continue;
                }

                free(key);
                d->pos++;

                /* Read remaining object fields at depth + 1 */
                while (!at_end(d)) {
                    Line *fln;
                    const char *fvstart;
                    int fvlen;
                    char *ftext;
                    char *fkey;
                    ArrayHeader fhdr;

                    skip_blanks(d);
                    fln = cur_line(d);
                    if (!fln || fln->blank) break;
                    if (fln->depth != content_depth + 1) break;

                    ftext = line_strdup(fln);

                    /* Check array header */
                    if (parse_array_header(ftext, fln->len, &fhdr, d)) {
                        JsonValue *farr;
                        if (fhdr.has_inline) {
                            int fivlen = fln->len - (int)(fhdr.inline_values - ftext);
                            farr = decode_inline_primitive_array(
                                fhdr.inline_values, fivlen, &fhdr, d);
                            d->pos++;
                        } else if (fhdr.fields) {
                            d->pos++;
                            farr = decode_tabular_array(d, &fhdr,
                                content_depth + 2);
                        } else {
                            d->pos++;
                            farr = decode_list_array(d, &fhdr,
                                content_depth + 2);
                        }
                        if (farr) {
                            json_object_set(item_obj,
                                fhdr.key ? fhdr.key : "", farr);
                        }
                        free_header(&fhdr);
                        free(ftext);
                        continue;
                    }

                    fvstart = NULL;
                    fvlen = 0;
                    fkey = parse_key_value(ftext, fln->len, &fvstart, &fvlen, d);
                    if (fkey) {
                        if (fvstart && fvlen > 0) {
                            JsonValue *fval = parse_value_token(fvstart, fvlen, d);
                            json_object_set(item_obj, fkey, fval);
                            d->pos++;
                        } else {
                            d->pos++;
                            {
                                Line *nln2;
                                skip_blanks(d);
                                nln2 = cur_line(d);
                                if (nln2 && nln2->depth > content_depth + 1) {
                                    JsonValue *nested2 = decode_object_at_depth(
                                        d, content_depth + 2);
                                    json_object_set(item_obj, fkey, nested2);
                                } else {
                                    json_object_set(item_obj, fkey,
                                        json_new_object());
                                }
                            }
                        }
                        free(fkey);
                    } else {
                        d->pos++;
                    }
                    free(ftext);
                }

                json_array_push(arr, item_obj);
                item_count++;
                continue;
            }
        }

        /* Plain primitive list item: "- value" */
        {
            JsonValue *pval = parse_value_token(after_marker, after_len, d);
            if (!pval) {
                json_free(arr);
                return NULL;
            }
            json_array_push(arr, pval);
            d->pos++;
            item_count++;
        }
    }

    if (d->strict && item_count != hdr->length) {
        d->err = "List array count mismatch";
        json_free(arr);
        return NULL;
    }

    return arr;
}

/* ---- Object decoding ---- */

static JsonValue *decode_object_at_depth(Decoder *d, int depth)
{
    JsonValue *obj = json_new_object();

    while (!at_end(d)) {
        Line *ln;
        char *text;
        ArrayHeader hdr;
        const char *vstart;
        int vlen;
        char *key;

        skip_blanks(d);
        ln = cur_line(d);
        if (!ln || ln->blank) break;
        if (ln->depth < depth) break;
        if (ln->depth != depth) {
            /* Skip lines at unexpected depth */
            d->pos++;
            continue;
        }

        text = line_strdup(ln);

        /* Try array header first */
        if (parse_array_header(text, ln->len, &hdr, d)) {
            JsonValue *arr_val;

            if (hdr.has_inline && !hdr.fields) {
                /* Inline primitive array */
                int ivlen = ln->len - (int)(hdr.inline_values - text);
                arr_val = decode_inline_primitive_array(
                    hdr.inline_values, ivlen, &hdr, d);
                d->pos++;
            } else if (hdr.fields) {
                /* Tabular array */
                d->pos++;
                arr_val = decode_tabular_array(d, &hdr, depth + 1);
            } else {
                /* List array */
                d->pos++;
                arr_val = decode_list_array(d, &hdr, depth + 1);
            }

            if (arr_val) {
                json_object_set(obj, hdr.key ? hdr.key : "", arr_val);
            }
            free_header(&hdr);
            free(text);
            continue;
        }

        /* Parse key: value */
        vstart = NULL;
        vlen = 0;
        key = parse_key_value(text, ln->len, &vstart, &vlen, d);

        if (!key) {
            /* Not a key-value line - skip */
            free(text);
            d->pos++;
            continue;
        }

        if (vstart && vlen > 0) {
            /* Primitive value */
            JsonValue *val = parse_value_token(vstart, vlen, d);
            json_object_set(obj, key, val);
            d->pos++;
        } else {
            /* key: alone -> nested object or empty */
            d->pos++;
            {
                Line *nln;
                skip_blanks(d);
                nln = cur_line(d);
                if (nln && !nln->blank && nln->depth > depth) {
                    JsonValue *nested = decode_object_at_depth(d, depth + 1);
                    json_object_set(obj, key, nested);
                } else {
                    json_object_set(obj, key, json_new_object());
                }
            }
        }

        free(key);
        free(text);
    }

    return obj;
}

/* ---- Decode a value at a given depth ---- */

static JsonValue *decode_value_at_depth(Decoder *d, int depth)
{
    Line *ln;
    char *text;

    skip_blanks(d);
    ln = cur_line(d);
    if (!ln) return json_new_object();

    text = line_strdup(ln);

    /* Check for array header */
    {
        ArrayHeader hdr;
        if (parse_array_header(text, ln->len, &hdr, d)) {
            JsonValue *arr;
            if (hdr.has_inline && !hdr.fields) {
                int ivlen = ln->len - (int)(hdr.inline_values - text);
                arr = decode_inline_primitive_array(
                    hdr.inline_values, ivlen, &hdr, d);
                d->pos++;
            } else if (hdr.fields) {
                d->pos++;
                arr = decode_tabular_array(d, &hdr, depth + 1);
            } else {
                d->pos++;
                arr = decode_list_array(d, &hdr, depth + 1);
            }
            free_header(&hdr);
            free(text);
            return arr;
        }
    }

    free(text);

    /* Try object */
    return decode_object_at_depth(d, depth);
}

/* ---- Root form discovery (Section 5) ---- */

JsonValue *toon_decode(const char *input, const ToonDecodeOpts *opts,
                       const char **errout)
{
    Decoder d;
    int indent_size = 2;
    int first_non_empty = -1;
    int non_empty_count = 0;
    int i;
    JsonValue *result;

    memset(&d, 0, sizeof(d));

    if (opts) {
        indent_size = opts->indent > 0 ? opts->indent : 2;
        d.strict = opts->strict;
    } else {
        d.strict = TRUE;
    }
    d.indent_size = indent_size;

    /* Scan lines */
    scan_lines(&d, input, indent_size);

    /* Count non-empty lines and find first */
    {
        int total_non_empty = 0;
        for (i = 0; i < d.nlines; i++) {
            if (!d.lines[i].blank) {
                total_non_empty++;
                if (d.lines[i].depth == 0) {
                    if (first_non_empty < 0) first_non_empty = i;
                    non_empty_count++;
                }
            }
        }

        /* Empty document -> empty object */
        if (first_non_empty < 0 || non_empty_count == 0) {
            free(d.lines);
            return json_new_object();
        }

        d.pos = 0;

        /* Check root form */
        {
            Line *first = &d.lines[first_non_empty];
            char *ftext = line_strdup(first);
            ArrayHeader hdr;

            /* Root array? (starts with '[') */
            if (ftext[0] == '[' && parse_array_header(ftext, first->len, &hdr, &d)) {
                d.pos = first_non_empty;

                if (hdr.has_inline && !hdr.fields) {
                    int ivlen = first->len - (int)(hdr.inline_values - ftext);
                    result = decode_inline_primitive_array(
                        hdr.inline_values, ivlen, &hdr, &d);
                    d.pos++;
                } else if (hdr.fields) {
                    d.pos++;
                    result = decode_tabular_array(&d, &hdr, 1);
                } else {
                    d.pos++;
                    result = decode_list_array(&d, &hdr, 1);
                }

                free_header(&hdr);
                free(ftext);
                free(d.lines);

                if (!result && errout) *errout = d.err;
                return result;
            }

            /* Single-line primitive? Only if truly one line total */
            if (non_empty_count == 1 && total_non_empty == 1) {
                const char *vs;
                int vl;
                char *key = parse_key_value(ftext, first->len, &vs, &vl, &d);
                if (!key) {
                    /* Check it's not an array header either */
                    if (!parse_array_header(ftext, first->len, &hdr, &d)) {
                        /* Not a key-value, not an array header -> single primitive */
                        result = parse_value_token(ftext, first->len, &d);
                        free(ftext);
                        free(d.lines);
                        return result;
                    }
                    free_header(&hdr);
                }
                free(key);
            }

            free(ftext);
        }
    }

    /* Object */
    d.pos = 0;
    result = decode_object_at_depth(&d, 0);

    free(d.lines);
    if (!result && errout) *errout = d.err;
    return result;
}
