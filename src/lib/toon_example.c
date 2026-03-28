/*
 * toon_example.c - Example program using toon.library
 *
 * Demonstrates all major toon.library API functions.
 * Build: sc link toon_example.c LIB lib:sc.lib lib:amiga.lib
 *
 * Before running: copy toon.library LIBS:
 */

#include <stdio.h>
#include <proto/exec.h>
#include <proto/toon.h>

struct Library *ToonBase = NULL;

int main(void)
{
    char *result;
    JsonValue val;
    JsonValue arr;
    JsonValue obj;
    const char *err = NULL;
    int i;
    double num;
    double pi;

    /* Open the library */
    ToonBase = OpenLibrary(TOON_NAME, TOON_VERSION);
    if (!ToonBase) {
        printf("Cannot open %s v%d\n", TOON_NAME, TOON_VERSION);
        return 1;
    }

    printf("toon.library opened (spec version %d.%d)\n\n",
           ToonVersion() / 100, ToonVersion() % 100);

    /* ---- Example 1: Encode JSON to TOON ---- */
    printf("=== Encode JSON to TOON ===\n");
    result = ToonEncode(
        "{\"users\":[{\"id\":1,\"name\":\"Alice\"},{\"id\":2,\"name\":\"Bob\"}]}",
        2, TOON_DELIM_COMMA);
    if (result) {
        printf("%s\n", result);
        ToonFreeString(result);
    }

    /* ---- Example 2: Decode TOON to JSON ---- */
    printf("\n=== Decode TOON to JSON ===\n");
    result = ToonDecode("server:\n  host: localhost\n  port: 8080", 2, 0);
    if (result) {
        printf("%s\n", result);
        ToonFreeString(result);
    }

    /* ---- Example 3: Get a value by path ---- */
    printf("\n=== Get value by path ===\n");
    result = ToonGet(
        "users[2]{id,name}:\n  1,Alice\n  2,Bob",
        "users[1].name", TOON_FMT_AUTO);
    if (result) {
        printf("users[1].name = %s\n", result);
        ToonFreeString(result);
    }

    /* ---- Example 4: Set a value ---- */
    printf("\n=== Set a value ===\n");
    result = ToonSet(
        "server:\n  host: localhost\n  port: 8080",
        "server.port", "9090");
    if (result) {
        printf("%s\n", result);
        ToonFreeString(result);
    }

    /* ---- Example 5: Delete a value ---- */
    printf("\n=== Delete a value ===\n");
    result = ToonDel(
        "name: Alice\nage: 30\nrole: admin",
        "age");
    if (result) {
        printf("%s\n", result);
        ToonFreeString(result);
    }

    /* ---- Example 6: Build a JsonValue tree ---- */
    printf("\n=== Build JsonValue tree ===\n");
    obj = ToonNewObject();
    ToonObjectSet(obj, "name", ToonNewString("Widget"));
    pi = 3.14;
    ToonObjectSet(obj, "price", ToonNewNumber(&pi));
    ToonObjectSet(obj, "active", ToonNewBool(1));

    arr = ToonNewArray();
    ToonArrayPush(arr, ToonNewString("tag1"));
    ToonArrayPush(arr, ToonNewString("tag2"));
    ToonObjectSet(obj, "tags", arr);

    result = ToonEncodeValue(obj, 2, TOON_DELIM_COMMA);
    if (result) {
        printf("%s\n", result);
        ToonFreeString(result);
    }

    /* Query the tree */
    printf("\nQuerying tree:\n");
    printf("  type: %d (OBJECT=%d)\n", ToonGetType(obj), TOON_TYPE_OBJECT);
    printf("  fields: %d\n", ToonObjectCount(obj));
    for (i = 0; i < ToonObjectCount(obj); i++) {
        printf("  [%d] %s = ", i, ToonObjectKey(obj, i));
        {
            JsonValue item = ToonObjectItem(obj, i);
            int t = ToonGetType(item);
            if (t == TOON_TYPE_STRING)
                printf("\"%s\"", ToonGetString(item));
            else if (t == TOON_TYPE_NUMBER) {
                ToonGetNumber(item, &num);
                printf("%ld", (long)num);
            } else if (t == TOON_TYPE_BOOL)
                printf("%s", ToonGetBool(item) ? "true" : "false");
            else if (t == TOON_TYPE_ARRAY)
                printf("[%d items]", ToonArrayCount(item));
            else
                printf("(type %d)", t);
        }
        printf("\n");
    }

    ToonFreeValue(obj);

    /* ---- Example 7: Parse JSON, emit as TOON ---- */
    printf("\n=== JSON parse -> TOON encode ===\n");
    val = ToonParseJSON("{\"items\":[1,2,3],\"count\":3}", &err);
    if (val) {
        result = ToonEncodeValue(val, 2, TOON_DELIM_COMMA);
        if (result) {
            printf("%s\n", result);
            ToonFreeString(result);
        }
        ToonFreeValue(val);
    } else {
        printf("Parse error: %s\n", err ? err : "unknown");
    }

    /* Done */
    CloseLibrary(ToonBase);
    printf("\nDone.\n");
    return 0;
}
