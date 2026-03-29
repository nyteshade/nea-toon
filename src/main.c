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
    FMT_TOON,
    FMT_COUNT       /* -c: print count (array length or object field count) */
} OutputFmt;

/*
 * Path operations are in toon_path.c (shared with toon.library).
 * Functions: toon_is_root_path, toon_path_get, toon_path_set,
 *            toon_path_del, toon_path_append
 */

/* Legacy aliases used below (shorter names for readability) */
#define is_root_path   toon_is_root_path
#define json_get_path  toon_path_get
#define json_set_path  toon_path_set
#define json_del_path  toon_path_del
#define json_append_path toon_path_append

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

    /* Count mode: print array length or object field count */
    if (fmt == FMT_COUNT) {
        if (v->type == JSON_ARRAY)
            printf("%d\n", v->u.arr.count);
        else if (v->type == JSON_OBJECT)
            printf("%d\n", v->u.obj.count);
        else if (v->type == JSON_STRING)
            printf("%d\n", (int)strlen(v->u.sval));
        else
            printf("1\n");
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
    printf("TOON - Token-Oriented Object Notation CLI v1.3\n");
    printf("Implements TOON Spec v3.0\n\n");
    printf("Usage:\n");
    printf("  toon encode [opts] [file.json]   JSON to TOON\n");
    printf("  toon decode [opts] [file.toon]   TOON to JSON\n");
    printf("  toon get [opts] <file> [path]    Get value\n");
    printf("  toon set [opts] <file> <path> <value>  Set value\n");
    printf("  toon del [opts] <file> <path>    Delete value\n\n");
    printf("Options:\n");
    printf("  -i <n>     Indent size (default 2)\n");
    printf("  -d <delim> Delimiter: comma, tab, pipe\n");
    printf("  -s         Strict mode (default for decode)\n");
    printf("  -l         Lenient (non-strict) mode\n");
    printf("  -o <file>  Output file (default stdout)\n");
    printf("  -j         Output as JSON (get command)\n");
    printf("  -t         Output as TOON (get command, default)\n");
    printf("  -c         Count mode (array len / object fields)\n");
    printf("  -a         Append to array (set command)\n\n");
    printf("Path syntax (get/set/del):\n");
    printf("  .                   Root value\n");
    printf("  person.name         Object property\n");
    printf("  users[0].name       Array index + property\n");
    printf("  users.0.name        Alt array index (no brackets)\n");
    printf("  [\"my-key\"].val      Quoted key for special chars\n");
    printf("  data[\"x.y\"]         Literal dotted key\n\n");
    printf("set creates the file if it doesn't exist.\n");
    printf("set -a appends to an array (creates if needed).\n");
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
    toon_bool append_mode = FALSE;
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
        } else if (strcmp(argv[i], "-c") == 0) {
            outfmt = FMT_COUNT;
        } else if (strcmp(argv[i], "-a") == 0) {
            append_mode = TRUE;
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

        if (!infile) {
            fprintf(stderr, "Usage: toon get [opts] <file> [path]\n");
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

        /* No path or root path: return the whole document */
        if (is_root_path(getpath)) {
            print_value(root, outfmt, indent, delim);
            json_free(root);
            return 0;
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
        toon_bool ok;
        ToonDecodeOpts dopts;
        ToonEncodeOpts eopts;

        if (!infile || !getpath || !setval) {
            fprintf(stderr, "Usage: toon set [-a] [opts] <file> <path> <value>\n");
            return 1;
        }

        input = toon_read_file(infile);
        if (!input) {
            /* File doesn't exist - create root based on context */
            if (append_mode && is_root_path(getpath)) {
                root = json_new_array();
            } else if (append_mode) {
                root = json_new_object();
            } else if (is_root_path(getpath)) {
                /* Setting root directly — value becomes the whole doc */
                root = NULL;
            } else {
                root = json_new_object();
            }
        } else {
            dopts.indent = indent;
            dopts.strict = FALSE;
            root = toon_decode(input, &dopts, &err);
            free(input);
        }

        new_val = parse_cli_value(setval);

        if (is_root_path(getpath) && !append_mode) {
            /* Replace the entire document with the new value */
            if (root) json_free(root);
            root = new_val;
            ok = TRUE;
        } else if (append_mode) {
            if (!root) {
                fprintf(stderr, "TOON decode error: %s\n", err ? err : "unknown");
                json_free(new_val);
                return 1;
            }
            ok = json_append_path(root, getpath, new_val);
            if (!ok) {
                fprintf(stderr, "Cannot append to %s (not an array)\n", getpath);
                json_free(new_val);
                json_free(root);
                return 1;
            }
        } else {
            if (!root) {
                fprintf(stderr, "TOON decode error: %s\n", err ? err : "unknown");
                json_free(new_val);
                return 1;
            }
            ok = json_set_path(root, getpath, new_val);
            if (!ok) {
                fprintf(stderr, "Cannot set path: %s\n", getpath);
                json_free(new_val);
                json_free(root);
                return 1;
            }
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
