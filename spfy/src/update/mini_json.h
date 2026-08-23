/* A small, allocation-free JSON reader.
 *
 * Enough to read the update manifest and the state file, and nothing more:
 * the parser builds a flat node array over the caller's buffer (no copies,
 * no strdup) and the accessors below walk it. Unknown members are simply
 * never asked for, which is what lets the manifest grow new keys without
 * breaking a checker that shipped a year earlier.
 *
 * The source buffer must be NUL-terminated and must outlive the mj_doc --
 * every node points into it.
 *
 * Object keys are compared as raw bytes, so a key written with escapes
 * ("version") will not match. Ours are plain ASCII and the generator is
 * ours too.
 */

#ifndef SPFY_UPDATE_MINI_JSON_H
#define SPFY_UPDATE_MINI_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MJ_NULL = 0,
    MJ_BOOL,
    MJ_NUM,
    MJ_STR,
    MJ_ARR,
    MJ_OBJ
} mj_type;

typedef struct {
    mj_type     type;
    int         first;      /* first child, -1 when none        */
    int         next;       /* next sibling, -1 when last       */
    const char *key;        /* member name, NULL outside objects*/
    int         key_n;
    const char *raw;        /* string body (between the quotes) */
    int         raw_n;
    double      num;
    int         bval;
} mj_node;

typedef struct {
    mj_node *n;
    int      cap;
    int      count;
} mj_doc;

/* Returns 0 on success (root is node 0), -1 on malformed input, -2 when the
 * node pool ran out. Trailing bytes after the root value are ignored. */
int mj_parse(const char *src, mj_node *nodes, int cap, mj_doc *doc);

/* -1 when absent, when `obj` is not an object, or when obj < 0 -- so lookups
 * chain without a null check at every step. */
int mj_obj_get(const mj_doc *d, int obj, const char *key);

int mj_arr_first(const mj_doc *d, int arr);
int mj_next(const mj_doc *d, int idx);

/* Copy an unescaped string into `out` (always NUL-terminated). Returns the
 * number of bytes written, or 0 when the node is missing or not a string.
 * \uXXXX becomes UTF-8; a lone surrogate becomes U+FFFD. */
size_t mj_str(const mj_doc *d, int idx, char *out, size_t out_n);

double mj_num(const mj_doc *d, int idx, double dflt);

/* Accepts true/false and, for the state file written by hand, 0/1. */
int mj_bool(const mj_doc *d, int idx, int dflt);

#ifdef __cplusplus
}
#endif

#endif
