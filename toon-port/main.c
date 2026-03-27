/*
 * main.c - TOON CLI for AmigaOS
 *
 * Usage:
 *   toon encode [options] [file.json]    - JSON to TOON
 *   toon decode [options] [file.toon]    - TOON to JSON
 *   toon get <file.toon> <path>          - Get value at path
 *   toon set <file.toon> <path> <value>  - Set value at path
 *   toon del <file.toon> <path>          - Delete value at path
 *
 * Path syntax (all equivalent):
 *   person.name          Dot-separated property access
 *   person.address.city  Nested property access
 *   users[0].name        Bracket array index
 *   users.0.name         Dot-separated array index (shell-friendly)
 *   ["my-key"].value     Quoted key for special characters
 *   data["x.y"]          Quoted key preserving literal dots
 *
 * Options:
 *   -i <n>     Indent size (default 2)
 *   -d <delim> Delimiter: comma, tab, pipe (default comma)
 *   -s         Strict mode (default for decode)
 *   -l         Lenient mode (non-strict)
 *   -o <file>  Output file (default stdout)
 *   -j         Output as JSON (get only)
 *   -t         Output as TOON (get only, default for complex values)
 */

#include "toon.h"
#include <ctype.h>

/* ---- Output format ---- */
typedef enum {
    FMT_AUTO = 0,   /* primitives bare, complex as TOON */
    FMT_JSON,
    FMT_TOON
} OutputFmt;

/* ---- Path segment ---- */
typedef struct {
    int type;       /* 1 = key, 2 = array index */
    char key[256];
    int idx;
} PathSeg;

/* ---- Path parser ---- */

/*
 * Enhanced path parser supporting:
 *   key           -> key segment
 *   [N]           -> array index
 *   ["key"]       -> quoted key (for special chars, literal dots)
 *   .N            -> numeric segment = array index if target is array,
 *                    else key lookup
 *
 * Dotted key ambiguity strategy:
 *   Path traversal is always used. If you need a literal dotted key,
 *   use the quoted bracket syntax: ["user.name"]
 */
static int parse_path_segments(const char *path, PathSeg *segs, int maxsegs)
{
    const char *p = path;
    int nseg = 0;

    while (*p && nseg < maxsegs) {
        /* Skip leading dot separator */
        if (*p == '.') p++;

        if (*p == '[') {
            p++; /* skip '[' */

            if (*p == '"') {
                /* Quoted key: ["key-name"] */
                int ki = 0;
                p++; /* skip opening quote */
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1]) {
                        p++;
                        switch (*p) {
                        case 'n': segs[nseg].key[ki++] = '\n'; break;
                        case 't': segs[nseg].key[ki++] = '\t'; break;
                        case '"': segs[nseg].key[ki++] = '"'; break;
                        case '\\': segs[nseg].key[ki++] = '\\'; break;
                        default: segs[nseg].key[ki++] = *p; break;
                        }
                    } else {
                        if (ki < 255) segs[nseg].key[ki++] = *p;
                    }
                    p++;
                }
                segs[nseg].key[ki] = '\0';
                if (*p == '"') p++;
                if (*p == ']') p++;
                segs[nseg].type = 1; /* key */
                nseg++;
            } else if (isdigit((unsigned char)*p)) {
                /* Numeric index: [N] */
                int idx = 0;
                while (isdigit((unsigned char)*p)) {
                    idx = idx * 10 + (*p - '0');
                    p++;
                }
                if (*p == ']') p++;
                segs[nseg].type = 2; /* array index */
                segs[nseg].idx = idx;
                nseg++;
            } else {
                /* Malformed bracket - skip */
                while (*p && *p != ']') p++;
                if (*p == ']') p++;
            }
        } else {
            /* Unquoted key or numeric segment - read until '.', '[', or end */
            int ki = 0;
            toon_bool all_digits = TRUE;

            while (*p && *p != '.' && *p != '[') {
                if (!isdigit((unsigned char)*p)) all_digits = FALSE;
                if (ki < 255) segs[nseg].key[ki++] = *p;
                p++;
            }
            segs[nseg].key[ki] = '\0';

            if (ki == 0) continue;

            if (all_digits && ki > 0) {
                /* Pure numeric segment: treat as array index.
                   At resolve time we check if target is actually an array;
                   if not, fall back to key lookup. */
                segs[nseg].type = 2;
                segs[nseg].idx = atoi(segs[nseg].key);
            } else {
                segs[nseg].type = 1;
            }
            nseg++;
        }
    }

    return nseg;
}

/* ---- Path resolution (get) ---- */

static const JsonValue *json_get_path(const JsonValue *root, const char *path)
{
    PathSeg segs[64];
    int nseg, i;
    const JsonValue *cur = root;

    nseg = parse_path_segments(path, segs, 64);

    for (i = 0; i < nseg; i++) {
        if (!cur) return NULL;

        if (segs[i].type == 1) {
            /* Key lookup - must be object */
            int j;
            toon_bool found = FALSE;
            if (cur->type != JSON_OBJECT) return NULL;
            for (j = 0; j < cur->u.obj.count; j++) {
                if (strcmp(cur->u.obj.pairs[j].key, segs[i].key) == 0) {
                    cur = cur->u.obj.pairs[j].value;
                    found = TRUE;
                    break;
                }
            }
            if (!found) return NULL;
        } else if (segs[i].type == 2) {
            /* Numeric segment: try array index first, then key fallback */
            if (cur->type == JSON_ARRAY) {
                if (segs[i].idx < 0 || segs[i].idx >= cur->u.arr.count)
                    return NULL;
                cur = cur->u.arr.items[segs[i].idx];
            } else if (cur->type == JSON_OBJECT) {
                /* Fallback: try as key name (e.g., key "0") */
                int j;
                toon_bool found = FALSE;
                for (j = 0; j < cur->u.obj.count; j++) {
                    if (strcmp(cur->u.obj.pairs[j].key, segs[i].key) == 0) {
                        cur = cur->u.obj.pairs[j].value;
                        found = TRUE;
                        break;
                    }
                }
                if (!found) return NULL;
            } else {
                return NULL;
            }
        }
    }

    return cur;
}

/* ---- Determine what the next segment expects ---- */

static toon_bool next_seg_is_index(PathSeg *segs, int nseg, int i)
{
    if (i + 1 < nseg && segs[i + 1].type == 2)
        return TRUE;
    return FALSE;
}

/* ---- Path resolution (set) with deep-create ---- */

static toon_bool json_set_path(JsonValue *root, const char *path,
                               JsonValue *new_val)
{
    PathSeg segs[64];
    int nseg, i;
    JsonValue *cur = root;

    nseg = parse_path_segments(path, segs, 64);
    if (nseg == 0) return FALSE;

    /* Navigate to parent, creating intermediates as needed */
    for (i = 0; i < nseg - 1; i++) {
        if (!cur) return FALSE;

        if (segs[i].type == 1) {
            /* Key navigation */
            if (cur->type != JSON_OBJECT) return FALSE;
            {
                int j;
                toon_bool found = FALSE;
                for (j = 0; j < cur->u.obj.count; j++) {
                    if (strcmp(cur->u.obj.pairs[j].key, segs[i].key) == 0) {
                        cur = cur->u.obj.pairs[j].value;
                        found = TRUE;
                        break;
                    }
                }
                if (!found) {
                    /* Create intermediate */
                    JsonValue *mid;
                    if (next_seg_is_index(segs, nseg, i))
                        mid = json_new_array();
                    else
                        mid = json_new_object();
                    json_object_set(cur, segs[i].key, mid);
                    cur = mid;
                }
            }
        } else if (segs[i].type == 2) {
            /* Index navigation */
            if (cur->type == JSON_ARRAY) {
                /* Extend array if needed */
                while (cur->u.arr.count <= segs[i].idx)
                    json_array_push(cur, json_new_null());
                /* If the existing element is null and we need to go deeper,
                   replace with appropriate container */
                if (cur->u.arr.items[segs[i].idx]->type == JSON_NULL) {
                    JsonValue *mid;
                    if (next_seg_is_index(segs, nseg, i))
                        mid = json_new_array();
                    else
                        mid = json_new_object();
                    json_free(cur->u.arr.items[segs[i].idx]);
                    cur->u.arr.items[segs[i].idx] = mid;
                }
                cur = cur->u.arr.items[segs[i].idx];
            } else if (cur->type == JSON_OBJECT) {
                /* Numeric key fallback */
                int j;
                toon_bool found = FALSE;
                for (j = 0; j < cur->u.obj.count; j++) {
                    if (strcmp(cur->u.obj.pairs[j].key, segs[i].key) == 0) {
                        cur = cur->u.obj.pairs[j].value;
                        found = TRUE;
                        break;
                    }
                }
                if (!found) {
                    JsonValue *mid;
                    if (next_seg_is_index(segs, nseg, i))
                        mid = json_new_array();
                    else
                        mid = json_new_object();
                    json_object_set(cur, segs[i].key, mid);
                    cur = mid;
                }
            } else {
                return FALSE;
            }
        }
    }

    /* Set the final segment */
    {
        PathSeg *last = &segs[nseg - 1];

        if (last->type == 1) {
            if (cur->type != JSON_OBJECT) return FALSE;
            {
                int j;
                /* Replace existing key */
                for (j = 0; j < cur->u.obj.count; j++) {
                    if (strcmp(cur->u.obj.pairs[j].key, last->key) == 0) {
                        json_free(cur->u.obj.pairs[j].value);
                        cur->u.obj.pairs[j].value = new_val;
                        return TRUE;
                    }
                }
                /* Add new key */
                json_object_set(cur, last->key, new_val);
                return TRUE;
            }
        } else if (last->type == 2) {
            if (cur->type == JSON_ARRAY) {
                /* Extend if needed */
                while (cur->u.arr.count <= last->idx)
                    json_array_push(cur, json_new_null());
                json_free(cur->u.arr.items[last->idx]);
                cur->u.arr.items[last->idx] = new_val;
                return TRUE;
            } else if (cur->type == JSON_OBJECT) {
                /* Numeric key fallback */
                int j;
                for (j = 0; j < cur->u.obj.count; j++) {
                    if (strcmp(cur->u.obj.pairs[j].key, last->key) == 0) {
                        json_free(cur->u.obj.pairs[j].value);
                        cur->u.obj.pairs[j].value = new_val;
                        return TRUE;
                    }
                }
                json_object_set(cur, last->key, new_val);
                return TRUE;
            }
        }
    }

    return FALSE;
}

/* ---- Path resolution (del) ---- */

static toon_bool json_del_path(JsonValue *root, const char *path)
{
    PathSeg segs[64];
    int nseg, i;
    JsonValue *cur = root;

    nseg = parse_path_segments(path, segs, 64);
    if (nseg == 0) return FALSE;

    /* Navigate to parent */
    for (i = 0; i < nseg - 1; i++) {
        if (!cur) return FALSE;

        if (segs[i].type == 1) {
            if (cur->type != JSON_OBJECT) return FALSE;
            {
                int j;
                toon_bool found = FALSE;
                for (j = 0; j < cur->u.obj.count; j++) {
                    if (strcmp(cur->u.obj.pairs[j].key, segs[i].key) == 0) {
                        cur = cur->u.obj.pairs[j].value;
                        found = TRUE;
                        break;
                    }
                }
                if (!found) return FALSE;
            }
        } else if (segs[i].type == 2) {
            if (cur->type == JSON_ARRAY) {
                if (segs[i].idx < 0 || segs[i].idx >= cur->u.arr.count)
                    return FALSE;
                cur = cur->u.arr.items[segs[i].idx];
            } else if (cur->type == JSON_OBJECT) {
                int j;
                toon_bool found = FALSE;
                for (j = 0; j < cur->u.obj.count; j++) {
                    if (strcmp(cur->u.obj.pairs[j].key, segs[i].key) == 0) {
                        cur = cur->u.obj.pairs[j].value;
                        found = TRUE;
                        break;
                    }
                }
                if (!found) return FALSE;
            } else {
                return FALSE;
            }
        }
    }

    /* Delete the final segment */
    {
        PathSeg *last = &segs[nseg - 1];

        if (last->type == 1) {
            if (cur->type != JSON_OBJECT) return FALSE;
            {
                int j;
                for (j = 0; j < cur->u.obj.count; j++) {
                    if (strcmp(cur->u.obj.pairs[j].key, last->key) == 0) {
                        /* Free the pair */
                        free(cur->u.obj.pairs[j].key);
                        json_free(cur->u.obj.pairs[j].value);
                        /* Shift remaining pairs down */
                        {
                            int k;
                            for (k = j; k < cur->u.obj.count - 1; k++)
                                cur->u.obj.pairs[k] = cur->u.obj.pairs[k + 1];
                        }
                        cur->u.obj.count--;
                        return TRUE;
                    }
                }
                return FALSE; /* key not found */
            }
        } else if (last->type == 2) {
            if (cur->type == JSON_ARRAY) {
                if (last->idx < 0 || last->idx >= cur->u.arr.count)
                    return FALSE;
                json_free(cur->u.arr.items[last->idx]);
                /* Shift remaining items down */
                {
                    int k;
                    for (k = last->idx; k < cur->u.arr.count - 1; k++)
                        cur->u.arr.items[k] = cur->u.arr.items[k + 1];
                }
                cur->u.arr.count--;
                return TRUE;
            } else if (cur->type == JSON_OBJECT) {
                /* Numeric key fallback */
                int j;
                for (j = 0; j < cur->u.obj.count; j++) {
                    if (strcmp(cur->u.obj.pairs[j].key, last->key) == 0) {
                        free(cur->u.obj.pairs[j].key);
                        json_free(cur->u.obj.pairs[j].value);
                        {
                            int k;
                            for (k = j; k < cur->u.obj.count - 1; k++)
                                cur->u.obj.pairs[k] = cur->u.obj.pairs[k + 1];
                        }
                        cur->u.obj.count--;
                        return TRUE;
                    }
                }
                return FALSE;
            }
        }
    }

    return FALSE;
}

/* ---- Parse a CLI value string into a JsonValue ---- */

static JsonValue *parse_cli_value(const char *s)
{
    /* Try JSON parse first (for objects/arrays/quoted strings) */
    if (*s == '{' || *s == '[' || *s == '"') {
        const char *err = NULL;
        JsonValue *v = json_parse(s, &err);
        if (v) return v;
    }

    /* Primitives */
    if (strcmp(s, "true") == 0) return json_new_bool(TRUE);
    if (strcmp(s, "false") == 0) return json_new_bool(FALSE);
    if (strcmp(s, "null") == 0) return json_new_null();

    /* Number? */
    {
        const char *p = s;
        toon_bool is_num = TRUE;
        if (*p == '-') p++;
        if (!isdigit((unsigned char)*p)) is_num = FALSE;
        if (is_num) {
            while (isdigit((unsigned char)*p)) p++;
            if (*p == '.') { p++; while (isdigit((unsigned char)*p)) p++; }
            if (*p == 'e' || *p == 'E') {
                p++;
                if (*p == '+' || *p == '-') p++;
                while (isdigit((unsigned char)*p)) p++;
            }
            if (*p == '\0') return json_new_number(strtod(s, NULL));
        }
    }

    /* Default: string */
    return json_new_string(s);
}

/* ---- Output value ---- */

static void print_value(const JsonValue *v, OutputFmt fmt,
                        int indent, ToonDelimiter delim)
{
    if (!v) {
        printf("null\n");
        return;
    }

    /* Primitives always print bare regardless of format */
    switch (v->type) {
    case JSON_NULL:
        printf("null\n");
        return;
    case JSON_BOOL:
        printf("%s\n", v->u.bval ? "true" : "false");
        return;
    case JSON_NUMBER: {
        char nbuf[64];
        format_number(v->u.nval, nbuf, sizeof(nbuf));
        printf("%s\n", nbuf);
        return;
    }
    case JSON_STRING:
        printf("%s\n", v->u.sval);
        return;
    default:
        break;
    }

    /* Complex types: use requested format */
    if (fmt == FMT_JSON) {
        char *s = json_emit(v);
        if (s) {
            printf("%s\n", s);
            free(s);
        }
    } else {
        /* FMT_AUTO or FMT_TOON -> TOON output */
        ToonEncodeOpts eopts;
        char *s;
        eopts.indent = indent;
        eopts.delim = delim;
        s = toon_encode(v, &eopts);
        if (s) {
            printf("%s\n", s);
            free(s);
        }
    }
}

/* ---- Usage ---- */

static void print_usage(void)
{
    printf("TOON - Token-Oriented Object Notation CLI v1.0\n");
    printf("Implements TOON Spec v3.0\n\n");
    printf("Usage:\n");
    printf("  toon encode [opts] [file.json]   JSON to TOON\n");
    printf("  toon decode [opts] [file.toon]   TOON to JSON\n");
    printf("  toon get [opts] <file> <path>    Get value\n");
    printf("  toon set [opts] <file> <path> <value>  Set value\n");
    printf("  toon del [opts] <file> <path>    Delete value\n\n");
    printf("Options:\n");
    printf("  -i <n>     Indent size (default 2)\n");
    printf("  -d <delim> Delimiter: comma, tab, pipe\n");
    printf("  -s         Strict mode (default for decode)\n");
    printf("  -l         Lenient (non-strict) mode\n");
    printf("  -o <file>  Output file (default stdout)\n");
    printf("  -j         Output as JSON (get command)\n");
    printf("  -t         Output as TOON (get command, default)\n\n");
    printf("Path syntax (get/set/del):\n");
    printf("  person.name         Object property\n");
    printf("  users[0].name       Array index + property\n");
    printf("  users.0.name        Alt array index (no brackets)\n");
    printf("  [\"my-key\"].val      Quoted key for special chars\n");
    printf("  data[\"x.y\"]         Literal dotted key\n");
}

/* ---- Main ---- */

int main(int argc, char *argv[])
{
    const char *command = NULL;
    const char *infile = NULL;
    const char *outfile = NULL;
    const char *getpath = NULL;
    const char *setval = NULL;
    int indent = 2;
    ToonDelimiter delim = DELIM_COMMA;
    toon_bool strict = TRUE;
    OutputFmt outfmt = FMT_AUTO;
    int i;

    if (argc < 2) {
        print_usage();
        return 0;
    }

    command = argv[1];

    /* Parse options and arguments */
    i = 2;
    while (i < argc) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            indent = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "tab") == 0) delim = DELIM_TAB;
            else if (strcmp(argv[i], "pipe") == 0) delim = DELIM_PIPE;
            else delim = DELIM_COMMA;
        } else if (strcmp(argv[i], "-s") == 0) {
            strict = TRUE;
        } else if (strcmp(argv[i], "-l") == 0) {
            strict = FALSE;
        } else if (strcmp(argv[i], "-j") == 0) {
            outfmt = FMT_JSON;
        } else if (strcmp(argv[i], "-t") == 0) {
            outfmt = FMT_TOON;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outfile = argv[++i];
        } else if (argv[i][0] != '-') {
            /* Positional args depend on command */
            if (strcmp(command, "get") == 0 || strcmp(command, "del") == 0) {
                if (!infile) infile = argv[i];
                else if (!getpath) getpath = argv[i];
            } else if (strcmp(command, "set") == 0) {
                if (!infile) infile = argv[i];
                else if (!getpath) getpath = argv[i];
                else if (!setval) setval = argv[i];
            } else {
                if (!infile) infile = argv[i];
            }
        }
        i++;
    }

    /* ---- ENCODE: JSON -> TOON ---- */
    if (strcmp(command, "encode") == 0) {
        char *input;
        const char *err = NULL;
        JsonValue *val;
        char *output;
        ToonEncodeOpts opts;

        input = infile ? toon_read_file(infile) : toon_read_stdin();
        if (!input) {
            fprintf(stderr, "Error: Cannot read input\n");
            return 1;
        }

        val = json_parse(input, &err);
        free(input);
        if (!val) {
            fprintf(stderr, "JSON parse error: %s\n", err ? err : "unknown");
            return 1;
        }

        opts.indent = indent;
        opts.delim = delim;
        output = toon_encode(val, &opts);
        json_free(val);

        if (outfile) {
            toon_write_file(outfile, output);
        } else {
            printf("%s\n", output);
        }
        free(output);
        return 0;
    }

    /* ---- DECODE: TOON -> JSON ---- */
    if (strcmp(command, "decode") == 0) {
        char *input;
        const char *err = NULL;
        JsonValue *val;
        char *output;
        ToonDecodeOpts opts;

        input = infile ? toon_read_file(infile) : toon_read_stdin();
        if (!input) {
            fprintf(stderr, "Error: Cannot read input\n");
            return 1;
        }

        opts.indent = indent;
        opts.strict = strict;
        val = toon_decode(input, &opts, &err);
        free(input);
        if (!val) {
            fprintf(stderr, "TOON decode error: %s\n", err ? err : "unknown");
            return 1;
        }

        output = json_emit(val);
        json_free(val);

        if (outfile) {
            toon_write_file(outfile, output);
        } else {
            printf("%s\n", output);
        }
        free(output);
        return 0;
    }

    /* ---- GET: Read value from TOON file ---- */
    if (strcmp(command, "get") == 0) {
        char *input;
        const char *err = NULL;
        JsonValue *root;
        const JsonValue *result;
        ToonDecodeOpts opts;

        if (!infile || !getpath) {
            fprintf(stderr, "Usage: toon get [opts] <file> <path>\n");
            return 1;
        }

        input = toon_read_file(infile);
        if (!input) {
            fprintf(stderr, "Error: Cannot read %s\n", infile);
            return 1;
        }

        opts.indent = indent;
        opts.strict = FALSE;
        root = toon_decode(input, &opts, &err);
        free(input);
        if (!root) {
            fprintf(stderr, "TOON decode error: %s\n", err ? err : "unknown");
            return 1;
        }

        result = json_get_path(root, getpath);
        if (!result) {
            fprintf(stderr, "Path not found: %s\n", getpath);
            json_free(root);
            return 1;
        }

        print_value(result, outfmt, indent, delim);
        json_free(root);
        return 0;
    }

    /* ---- SET: Write value in TOON file ---- */
    if (strcmp(command, "set") == 0) {
        char *input;
        const char *err = NULL;
        JsonValue *root;
        JsonValue *new_val;
        char *output;
        ToonDecodeOpts dopts;
        ToonEncodeOpts eopts;

        if (!infile || !getpath || !setval) {
            fprintf(stderr, "Usage: toon set [opts] <file> <path> <value>\n");
            return 1;
        }

        input = toon_read_file(infile);
        if (!input) {
            fprintf(stderr, "Error: Cannot read %s\n", infile);
            return 1;
        }

        dopts.indent = indent;
        dopts.strict = FALSE;
        root = toon_decode(input, &dopts, &err);
        free(input);
        if (!root) {
            fprintf(stderr, "TOON decode error: %s\n", err ? err : "unknown");
            return 1;
        }

        new_val = parse_cli_value(setval);
        if (!json_set_path(root, getpath, new_val)) {
            fprintf(stderr, "Cannot set path: %s\n", getpath);
            json_free(new_val);
            json_free(root);
            return 1;
        }

        eopts.indent = indent;
        eopts.delim = delim;
        output = toon_encode(root, &eopts);
        json_free(root);

        if (toon_write_file(infile, output) != 0) {
            fprintf(stderr, "Error: Cannot write %s\n", infile);
            free(output);
            return 1;
        }

        printf("%s\n", output);
        free(output);
        return 0;
    }

    /* ---- DEL: Delete value from TOON file ---- */
    if (strcmp(command, "del") == 0) {
        char *input;
        const char *err = NULL;
        JsonValue *root;
        char *output;
        ToonDecodeOpts dopts;
        ToonEncodeOpts eopts;

        if (!infile || !getpath) {
            fprintf(stderr, "Usage: toon del [opts] <file> <path>\n");
            return 1;
        }

        input = toon_read_file(infile);
        if (!input) {
            fprintf(stderr, "Error: Cannot read %s\n", infile);
            return 1;
        }

        dopts.indent = indent;
        dopts.strict = FALSE;
        root = toon_decode(input, &dopts, &err);
        free(input);
        if (!root) {
            fprintf(stderr, "TOON decode error: %s\n", err ? err : "unknown");
            return 1;
        }

        if (!json_del_path(root, getpath)) {
            fprintf(stderr, "Cannot delete path: %s\n", getpath);
            json_free(root);
            return 1;
        }

        eopts.indent = indent;
        eopts.delim = delim;
        output = toon_encode(root, &eopts);
        json_free(root);

        if (toon_write_file(infile, output) != 0) {
            fprintf(stderr, "Error: Cannot write %s\n", infile);
            free(output);
            return 1;
        }

        printf("%s\n", output);
        free(output);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", command);
    print_usage();
    return 1;
}
