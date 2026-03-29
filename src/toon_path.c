/*
 * toon_path.c - Path operations on JsonValue trees
 *
 * Shared by the toon CLI (main.c) and toon.library (toon_lib.c).
 * Supports JS-style dot/bracket notation with shell-friendly
 * numeric alternatives.
 */

#include "toon.h"
#include <ctype.h>

/* ---- Path segment (internal) ---- */

typedef struct {
    int type;       /* 1 = key, 2 = array index */
    char key[256];
    int idx;
} PathSeg;

/* ---- Root path check ---- */

toon_bool toon_is_root_path(const char *path)
{
    if (!path) return TRUE;
    if (path[0] == '.' && path[1] == '\0') return TRUE;
    if (path[0] == '\0') return TRUE;
    return FALSE;
}

/* ---- Path parser ---- */

static int parse_path_segments(const char *path, PathSeg *segs, int maxsegs)
{
    const char *p = path;
    int nseg = 0;

    while (*p && nseg < maxsegs) {
        if (*p == '.') p++;

        if (*p == '[') {
            p++;
            if (*p == '"') {
                int ki = 0;
                p++;
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
                segs[nseg].type = 1;
                nseg++;
            } else if (isdigit((unsigned char)*p)) {
                int idx = 0;
                while (isdigit((unsigned char)*p)) {
                    idx = idx * 10 + (*p - '0');
                    p++;
                }
                if (*p == ']') p++;
                segs[nseg].type = 2;
                segs[nseg].idx = idx;
                nseg++;
            } else {
                while (*p && *p != ']') p++;
                if (*p == ']') p++;
            }
        } else {
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

/* ---- Helpers ---- */

static toon_bool next_is_index(PathSeg *segs, int nseg, int i)
{
    return (i + 1 < nseg && segs[i + 1].type == 2) ? TRUE : FALSE;
}

static int obj_find_key(const JsonValue *obj, const char *key)
{
    int j;
    for (j = 0; j < obj->u.obj.count; j++) {
        if (strcmp(obj->u.obj.pairs[j].key, key) == 0)
            return j;
    }
    return -1;
}

/* ---- Get ---- */

const JsonValue *toon_path_get(const JsonValue *root, const char *path)
{
    PathSeg segs[64];
    int nseg, i;
    const JsonValue *cur = root;

    if (toon_is_root_path(path)) return root;

    nseg = parse_path_segments(path, segs, 64);

    for (i = 0; i < nseg; i++) {
        int j;
        if (!cur) return NULL;

        if (segs[i].type == 1) {
            if (cur->type != JSON_OBJECT) return NULL;
            j = obj_find_key(cur, segs[i].key);
            if (j < 0) return NULL;
            cur = cur->u.obj.pairs[j].value;
        } else if (segs[i].type == 2) {
            if (cur->type == JSON_ARRAY) {
                if (segs[i].idx < 0 || segs[i].idx >= cur->u.arr.count)
                    return NULL;
                cur = cur->u.arr.items[segs[i].idx];
            } else if (cur->type == JSON_OBJECT) {
                j = obj_find_key(cur, segs[i].key);
                if (j < 0) return NULL;
                cur = cur->u.obj.pairs[j].value;
            } else {
                return NULL;
            }
        }
    }

    return cur;
}

/* ---- Navigate to parent, creating intermediates ---- */

static JsonValue *walk_to_parent(JsonValue *root, PathSeg *segs, int nseg)
{
    JsonValue *cur = root;
    int i;

    for (i = 0; i < nseg - 1; i++) {
        int j;
        if (!cur) return NULL;

        if (segs[i].type == 1) {
            if (cur->type != JSON_OBJECT) return NULL;
            j = obj_find_key(cur, segs[i].key);
            if (j >= 0) {
                cur = cur->u.obj.pairs[j].value;
            } else {
                JsonValue *mid;
                if (next_is_index(segs, nseg, i))
                    mid = json_new_array();
                else
                    mid = json_new_object();
                json_object_set(cur, segs[i].key, mid);
                cur = mid;
            }
        } else if (segs[i].type == 2) {
            if (cur->type == JSON_ARRAY) {
                while (cur->u.arr.count <= segs[i].idx)
                    json_array_push(cur, json_new_null());
                if (cur->u.arr.items[segs[i].idx]->type == JSON_NULL) {
                    JsonValue *mid;
                    if (next_is_index(segs, nseg, i))
                        mid = json_new_array();
                    else
                        mid = json_new_object();
                    json_free(cur->u.arr.items[segs[i].idx]);
                    cur->u.arr.items[segs[i].idx] = mid;
                }
                cur = cur->u.arr.items[segs[i].idx];
            } else if (cur->type == JSON_OBJECT) {
                j = obj_find_key(cur, segs[i].key);
                if (j >= 0) {
                    cur = cur->u.obj.pairs[j].value;
                } else {
                    JsonValue *mid;
                    if (next_is_index(segs, nseg, i))
                        mid = json_new_array();
                    else
                        mid = json_new_object();
                    json_object_set(cur, segs[i].key, mid);
                    cur = mid;
                }
            } else {
                return NULL;
            }
        }
    }

    return cur;
}

/* ---- Set ---- */

toon_bool toon_path_set(JsonValue *root, const char *path, JsonValue *new_val)
{
    PathSeg segs[64];
    int nseg, j;
    JsonValue *cur;

    if (toon_is_root_path(path)) return FALSE; /* caller handles root */

    nseg = parse_path_segments(path, segs, 64);
    if (nseg == 0) return FALSE;

    cur = walk_to_parent(root, segs, nseg);
    if (!cur) return FALSE;

    /* Set the final segment */
    {
        PathSeg *last = &segs[nseg - 1];

        if (last->type == 1) {
            if (cur->type != JSON_OBJECT) return FALSE;
            j = obj_find_key(cur, last->key);
            if (j >= 0) {
                json_free(cur->u.obj.pairs[j].value);
                cur->u.obj.pairs[j].value = new_val;
            } else {
                json_object_set(cur, last->key, new_val);
            }
            return TRUE;
        } else if (last->type == 2) {
            if (cur->type == JSON_ARRAY) {
                while (cur->u.arr.count <= last->idx)
                    json_array_push(cur, json_new_null());
                json_free(cur->u.arr.items[last->idx]);
                cur->u.arr.items[last->idx] = new_val;
                return TRUE;
            } else if (cur->type == JSON_OBJECT) {
                j = obj_find_key(cur, last->key);
                if (j >= 0) {
                    json_free(cur->u.obj.pairs[j].value);
                    cur->u.obj.pairs[j].value = new_val;
                } else {
                    json_object_set(cur, last->key, new_val);
                }
                return TRUE;
            }
        }
    }

    return FALSE;
}

/* ---- Delete ---- */

toon_bool toon_path_del(JsonValue *root, const char *path)
{
    PathSeg segs[64];
    int nseg, i, j;
    JsonValue *cur = root;

    if (toon_is_root_path(path)) return FALSE; /* can't delete root */

    nseg = parse_path_segments(path, segs, 64);
    if (nseg == 0) return FALSE;

    /* Navigate to parent (don't create intermediates for delete) */
    for (i = 0; i < nseg - 1; i++) {
        if (!cur) return FALSE;

        if (segs[i].type == 1) {
            if (cur->type != JSON_OBJECT) return FALSE;
            j = obj_find_key(cur, segs[i].key);
            if (j < 0) return FALSE;
            cur = cur->u.obj.pairs[j].value;
        } else if (segs[i].type == 2) {
            if (cur->type == JSON_ARRAY) {
                if (segs[i].idx < 0 || segs[i].idx >= cur->u.arr.count)
                    return FALSE;
                cur = cur->u.arr.items[segs[i].idx];
            } else if (cur->type == JSON_OBJECT) {
                j = obj_find_key(cur, segs[i].key);
                if (j < 0) return FALSE;
                cur = cur->u.obj.pairs[j].value;
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
            j = obj_find_key(cur, last->key);
            if (j < 0) return FALSE;
            free(cur->u.obj.pairs[j].key);
            json_free(cur->u.obj.pairs[j].value);
            {
                int k;
                for (k = j; k < cur->u.obj.count - 1; k++)
                    cur->u.obj.pairs[k] = cur->u.obj.pairs[k + 1];
            }
            cur->u.obj.count--;
            return TRUE;
        } else if (last->type == 2) {
            if (cur->type == JSON_ARRAY) {
                if (last->idx < 0 || last->idx >= cur->u.arr.count)
                    return FALSE;
                json_free(cur->u.arr.items[last->idx]);
                {
                    int k;
                    for (k = last->idx; k < cur->u.arr.count - 1; k++)
                        cur->u.arr.items[k] = cur->u.arr.items[k + 1];
                }
                cur->u.arr.count--;
                return TRUE;
            } else if (cur->type == JSON_OBJECT) {
                j = obj_find_key(cur, last->key);
                if (j < 0) return FALSE;
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
    }

    return FALSE;
}

/* ---- Append ---- */

toon_bool toon_path_append(JsonValue *root, const char *path, JsonValue *new_val)
{
    /* Root append */
    if (toon_is_root_path(path)) {
        if (root->type != JSON_ARRAY) return FALSE;
        json_array_push(root, new_val);
        return TRUE;
    }

    /* Navigate the full path, creating arrays as needed */
    {
        PathSeg segs[64];
        int nseg, i, j;
        JsonValue *cur = root;

        nseg = parse_path_segments(path, segs, 64);
        if (nseg == 0) return FALSE;

        for (i = 0; i < nseg; i++) {
            if (!cur) return FALSE;

            if (segs[i].type == 1) {
                if (cur->type != JSON_OBJECT) return FALSE;
                j = obj_find_key(cur, segs[i].key);
                if (j >= 0) {
                    cur = cur->u.obj.pairs[j].value;
                } else {
                    JsonValue *arr = json_new_array();
                    json_object_set(cur, segs[i].key, arr);
                    cur = arr;
                }
            } else if (segs[i].type == 2) {
                if (cur->type == JSON_ARRAY) {
                    if (segs[i].idx < cur->u.arr.count)
                        cur = cur->u.arr.items[segs[i].idx];
                    else
                        return FALSE;
                } else if (cur->type == JSON_OBJECT) {
                    j = obj_find_key(cur, segs[i].key);
                    if (j >= 0) {
                        cur = cur->u.obj.pairs[j].value;
                    } else {
                        JsonValue *arr = json_new_array();
                        json_object_set(cur, segs[i].key, arr);
                        cur = arr;
                    }
                } else {
                    return FALSE;
                }
            }
        }

        if (cur->type != JSON_ARRAY) return FALSE;
        json_array_push(cur, new_val);
        return TRUE;
    }
}
