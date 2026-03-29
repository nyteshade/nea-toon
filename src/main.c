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

/* ---- Help ---- */

static void print_usage(void)
{
    printf("TOON - Token-Oriented Object Notation CLI v1.4\n");
    printf("Implements TOON Spec v3.0\n\n");
    printf("Commands:\n");
    printf("  toon encode [opts] [file]       JSON to TOON\n");
    printf("  toon decode [opts] [file]       TOON to JSON\n");
    printf("  toon get [opts] <file> [path]   Read a value\n");
    printf("  toon set [opts] <file> <path> <value>\n");
    printf("                                  Write a value\n");
    printf("  toon del [opts] <file> <path>   Delete a value\n\n");
    printf("Use 'toon <command> -h' for detailed help.\n");
}

static void print_path_help(void)
{
    printf("\nPath syntax:\n");
    printf("  .                 Root (whole document)\n");
    printf("  name              Object property\n");
    printf("  person.name       Nested property\n");
    printf("  users[0].name     Array index + property\n");
    printf("  users.0.name      Same (shell-friendly, no brackets)\n");
    printf("  [\"my-key\"].val    Quoted key (special chars)\n");
    printf("  data[\"x.y\"]       Literal dotted key\n");
}

static void help_encode(void)
{
    printf("toon encode - Convert JSON to TOON\n\n");
    printf("Usage: toon encode [options] [file.json]\n\n");
    printf("Reads JSON from <file> or stdin, writes TOON to stdout.\n\n");
    printf("Options:\n");
    printf("  -i <n>     Indent size (default 2)\n");
    printf("  -d <delim> Delimiter: comma, tab, pipe\n");
    printf("  -o <file>  Write to file instead of stdout\n\n");
    printf("Examples:\n");
    printf("  toon encode data.json\n");
    printf("  toon encode data.json -o data.toon\n");
    printf("  toon encode -d pipe data.json\n");
    printf("  echo '{\"a\":1}' | toon encode\n");
}

static void help_decode(void)
{
    printf("toon decode - Convert TOON to JSON\n\n");
    printf("Usage: toon decode [options] [file.toon]\n\n");
    printf("Reads TOON from <file> or stdin, writes JSON to stdout.\n\n");
    printf("Options:\n");
    printf("  -i <n>     Indent size (default 2)\n");
    printf("  -s         Strict mode (default)\n");
    printf("  -l         Lenient mode\n");
    printf("  -o <file>  Write to file instead of stdout\n\n");
    printf("Examples:\n");
    printf("  toon decode data.toon\n");
    printf("  toon decode -l data.toon\n");
    printf("  type data.toon | toon decode\n");
}

static void help_get(void)
{
    printf("toon get - Read a value from a TOON file\n\n");
    printf("Usage: toon get [options] <file> [path]\n\n");
    printf("Reads the value at <path> from <file>.\n");
    printf("If no path is given, shows the whole file.\n\n");
    printf("Options:\n");
    printf("  -j         Output as JSON\n");
    printf("  -t         Output as TOON (default for complex values)\n");
    printf("  -c         Count mode: array length or object field count\n");
    printf("  -i <n>     Indent size (default 2)\n");
    printf("  -d <delim> Delimiter for TOON output\n");
    print_path_help();
    printf("\nExamples:\n");
    printf("  toon get data.toon              Show whole file\n");
    printf("  toon get data.toon server.host  Read nested value\n");
    printf("  toon get data.toon users.0.name First user's name\n");
    printf("  toon get -c data.toon users     Count array elements\n");
    printf("  toon get -j data.toon server    Output as JSON\n");
}

static void help_set(void)
{
    printf("toon set - Write a value to a TOON file\n\n");
    printf("Usage: toon set [options] <file> <path> <value>\n\n");
    printf("Sets <path> to <value> in <file>, creating the file\n");
    printf("and any intermediate objects if they don't exist.\n\n");
    printf("Options:\n");
    printf("  -a         Append mode: push <value> onto the array\n");
    printf("             at <path>, creating it if needed\n");
    printf("  -i <n>     Indent size (default 2)\n");
    printf("  -d <delim> Delimiter for output\n");
    print_path_help();
    printf("\nValues:\n");
    printf("  Strings:   toon set f.toon key hello\n");
    printf("  Numbers:   toon set f.toon key 42\n");
    printf("  Booleans:  toon set f.toon key true\n");
    printf("  Null:      toon set f.toon key null\n");
    printf("  JSON:      toon set f.toon key '{\"a\":1}'\n");
    printf("  Root:      toon set f.toon . '{\"new\":\"doc\"}'\n");
    printf("\nAppend examples:\n");
    printf("  toon set -a f.toon friends Kristin\n");
    printf("  toon set -a f.toon friends Lacy\n");
    printf("  toon set -a f.toon . item      Root array append\n");
}

static void help_del(void)
{
    printf("toon del - Delete a value from a TOON file\n\n");
    printf("Usage: toon del [options] <file> <path>\n\n");
    printf("Removes the key or array element at <path>.\n");
    printf("Array elements shift down after deletion.\n\n");
    printf("Options:\n");
    printf("  -i <n>     Indent size (default 2)\n");
    printf("  -d <delim> Delimiter for output\n");
    print_path_help();
    printf("\nExamples:\n");
    printf("  toon del data.toon server.timeout\n");
    printf("  toon del data.toon users[1]\n");
    printf("  toon del data.toon users.1       Same (no brackets)\n");
}

static toon_bool is_help_flag(const char *s)
{
    return (strcmp(s, "-h") == 0 || strcmp(s, "--help") == 0 ||
            strcmp(s, "help") == 0 || strcmp(s, "?") == 0);
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

    /* Handle 'toon help [command]' and 'toon -h' */
    if (is_help_flag(command)) {
        if (argc >= 3) {
            command = argv[2];
            if (strcmp(command, "encode") == 0) { help_encode(); return 0; }
            if (strcmp(command, "decode") == 0) { help_decode(); return 0; }
            if (strcmp(command, "get") == 0)    { help_get(); return 0; }
            if (strcmp(command, "set") == 0)    { help_set(); return 0; }
            if (strcmp(command, "del") == 0)    { help_del(); return 0; }
        }
        print_usage();
        return 0;
    }

    /* Check for 'toon <command> -h' */
    if (argc >= 3 && is_help_flag(argv[2])) {
        if (strcmp(command, "encode") == 0) { help_encode(); return 0; }
        if (strcmp(command, "decode") == 0) { help_decode(); return 0; }
        if (strcmp(command, "get") == 0)    { help_get(); return 0; }
        if (strcmp(command, "set") == 0)    { help_set(); return 0; }
        if (strcmp(command, "del") == 0)    { help_del(); return 0; }
    }

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
            help_get();
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
            help_set();
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
            help_del();
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
