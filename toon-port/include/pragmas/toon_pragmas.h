/*
 * pragmas/toon_pragmas.h - SAS/C pragmas for toon.library
 *
 * Auto-generated from fd/toon_lib.fd
 * Do not edit manually.
 *
 * Pragma format: #pragma libcall Base Function Offset RegisterMask
 *
 * RegisterMask: rightmost hex digit = (return_reg << 4) | arg_count
 *   Register encoding: D0=0 D1=1 D2=2 ... D7=7 A0=8 A1=9 A2=A ... A5=D
 *   Digits read right-to-left after the count nibble = arg registers
 *   Return in D0 for integers/pointers
 *
 * Offsets: bias 30 (0x1E), each vector +6 bytes
 */

#ifndef PRAGMAS_TOON_PRAGMAS_H
#define PRAGMAS_TOON_PRAGMAS_H

/*                                         Offset  Mask          */

/* Core encode/decode (text to text) */
/* char *ToonEncode(char *json/A0, int indent/D0, int delim/D1)  */
#pragma libcall ToonBase ToonEncode       1E 10803
/* char *ToonDecode(char *toon/A0, int indent/D0, int strict/D1) */
#pragma libcall ToonBase ToonDecode       24 10803

/* Core encode/decode (with JsonValue) */
/* JsonValue *ToonDecodeValue(char *toon/A0, int indent/D0, int strict/D1, char **err/A1) */
#pragma libcall ToonBase ToonDecodeValue  2A 910804
/* char *ToonEncodeValue(JsonValue *val/A0, int indent/D0, int delim/D1) */
#pragma libcall ToonBase ToonEncodeValue  30 10803

/* Path operations */
/* char *ToonGet(char *toon/A0, char *path/A1, int format/D0)    */
#pragma libcall ToonBase ToonGet          36 09803
/* char *ToonSet(char *toon/A0, char *path/A1, char *value/A2)   */
#pragma libcall ToonBase ToonSet          3C A9803
/* char *ToonDel(char *toon/A0, char *path/A1)                   */
#pragma libcall ToonBase ToonDel          42 9802

/* JsonValue construction */
#pragma libcall ToonBase ToonNewNull      48 0
#pragma libcall ToonBase ToonNewBool      4E 001
#pragma libcall ToonBase ToonNewNumber    54 801
#pragma libcall ToonBase ToonNewString    5A 801
#pragma libcall ToonBase ToonNewArray     60 0
#pragma libcall ToonBase ToonNewObject    66 0

/* JsonValue manipulation */
/* void ToonArrayPush(JsonValue *arr/A0, JsonValue *item/A1)     */
#pragma libcall ToonBase ToonArrayPush    6C 9802
/* void ToonObjectSet(JsonValue *obj/A0, char *key/A1, JsonValue *val/A2) */
#pragma libcall ToonBase ToonObjectSet    72 A9803
/* void ToonFreeValue(JsonValue *val/A0)                         */
#pragma libcall ToonBase ToonFreeValue    78 801

/* JsonValue query */
#pragma libcall ToonBase ToonGetType      7E 801
#pragma libcall ToonBase ToonGetString    84 801
/* int ToonGetNumber(JsonValue *val/A0, double *result/A1)       */
#pragma libcall ToonBase ToonGetNumber    8A 9802
#pragma libcall ToonBase ToonGetBool      90 801
#pragma libcall ToonBase ToonArrayCount   96 801
/* JsonValue *ToonArrayItem(JsonValue *val/A0, int index/D0)     */
#pragma libcall ToonBase ToonArrayItem    9C 0802
#pragma libcall ToonBase ToonObjectCount  A2 801
/* char *ToonObjectKey(JsonValue *val/A0, int index/D0)          */
#pragma libcall ToonBase ToonObjectKey    A8 0802
/* JsonValue *ToonObjectItem(JsonValue *val/A0, int index/D0)    */
#pragma libcall ToonBase ToonObjectItem   AE 0802

/* JSON parse/emit */
/* JsonValue *ToonParseJSON(char *json/A0, char **err/A1)        */
#pragma libcall ToonBase ToonParseJSON    B4 9802
/* char *ToonEmitJSON(JsonValue *val/A0)                         */
#pragma libcall ToonBase ToonEmitJSON     BA 801

/* Utility */
/* void ToonFreeString(char *s/A0)                               */
#pragma libcall ToonBase ToonFreeString   C0 801
/* int ToonVersion(void)                                         */
#pragma libcall ToonBase ToonVersion      C6 0

#endif /* PRAGMAS_TOON_PRAGMAS_H */
