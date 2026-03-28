/*
 * toon.h - TOON format (Token-Oriented Object Notation) for Amiga
 *
 * Implements TOON Specification v3.0
 * SAS/C 6.58 compatible (C89)
 */

#ifndef TOON_H
#define TOON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SAS/C compatibility */
#ifdef __SASC
static const char amiga_ver[] = "$VER: toon 1.0 (28.3.2026)";
#define inline __inline
#endif

/* Boolean type for C89 */
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
typedef int toon_bool;

/* ---- JSON Value Representation ---- */

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

/* Forward declarations */
typedef struct JsonValue JsonValue;
typedef struct JsonPair JsonPair;
typedef struct JsonArray JsonArray;

struct JsonArray {
    JsonValue **items;
    int count;
    int capacity;
};

struct JsonPair {
    char *key;
    JsonValue *value;
};

typedef struct {
    JsonPair *pairs;
    int count;
    int capacity;
} JsonObject;

struct JsonValue {
    JsonType type;
    union {
        toon_bool bval;
        double nval;
        char *sval;
        JsonArray arr;
        JsonObject obj;
    } u;
};

/* ---- JSON Value API ---- */

JsonValue *json_new_null(void);
JsonValue *json_new_bool(toon_bool v);
JsonValue *json_new_number(double v);
JsonValue *json_new_string(const char *s);
JsonValue *json_new_string_len(const char *s, int len);
JsonValue *json_new_array(void);
JsonValue *json_new_object(void);

void json_array_push(JsonValue *arr, JsonValue *item);
void json_object_set(JsonValue *obj, const char *key, JsonValue *val);

void json_free(JsonValue *v);

/* ---- JSON Parser ---- */

JsonValue *json_parse(const char *input, const char **errout);

/* ---- JSON Emitter ---- */

/* Emit JSON text from a JsonValue. Caller must free() result. */
char *json_emit(const JsonValue *v);

/* ---- TOON Decoder ---- */

typedef struct {
    int indent;       /* spaces per level, default 2 */
    toon_bool strict; /* strict mode, default TRUE */
} ToonDecodeOpts;

JsonValue *toon_decode(const char *input, const ToonDecodeOpts *opts,
                       const char **errout);

/* ---- TOON Encoder ---- */

typedef enum {
    DELIM_COMMA = 0,
    DELIM_TAB,
    DELIM_PIPE
} ToonDelimiter;

typedef struct {
    int indent;            /* spaces per level, default 2 */
    ToonDelimiter delim;   /* document delimiter, default COMMA */
} ToonEncodeOpts;

/* Encode a JsonValue to TOON text. Caller must free() result. */
char *toon_encode(const JsonValue *v, const ToonEncodeOpts *opts);

/* ---- Utilities ---- */

char *toon_read_file(const char *path);
char *toon_read_stdin(void);
int toon_write_file(const char *path, const char *data);

/* Dynamic string buffer */
typedef struct {
    char *data;
    int len;
    int cap;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_append(StrBuf *sb, const char *s);
void sb_appendn(StrBuf *sb, const char *s, int n);
void sb_appendc(StrBuf *sb, char c);
void sb_append_indent(StrBuf *sb, int depth, int indent_size);
char *sb_detach(StrBuf *sb);
void sb_free(StrBuf *sb);

/* Number formatting - canonical form (no exponent, no trailing zeros) */
void format_number(double val, char *buf, int bufsize);

/* Check if string needs quoting for TOON */
toon_bool toon_needs_quote(const char *s, char delim);

/* Check if string is valid unquoted key */
toon_bool toon_valid_unquoted_key(const char *s);

#endif /* TOON_H */
