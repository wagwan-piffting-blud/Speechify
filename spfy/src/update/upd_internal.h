/* Shared plumbing between the update TUs. Not installed, not public --
 * spfy_update.h is the interface the rest of the tree sees. */

#ifndef SPFY_UPDATE_INTERNAL_H
#define SPFY_UPDATE_INTERNAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------- trigger lib (no net) */

int  spfy_upd_mkdir(const char *path);

/* Size and mtime (unix seconds) of a regular file. Returns 0 on success. */
int  spfy_upd_file_stat(const char *path, long long *bytes, long long *mtime);

/* <state dir>/<leaf>, creating the directory. Returns 0 on success. */
int  spfy_upd_state_path(char *buf, size_t buf_n, const char *leaf);

/* Truncating copy that always NUL-terminates. */
void spfy_upd_strlcpy(char *dst, const char *src, size_t dst_n);

/* Unix seconds, as a long long on every platform (32-bit time_t included). */
long long spfy_upd_now(void);

/* --------------------------------------------------------- core lib only */

/* Calver compare: -1 / 0 / +1 for a<b, a==b, a>b.
 *
 * Numeric field by field on '.', so 2026.9.1 sorts before 2026.10.1 -- a
 * plain strcmp gets that pair backwards, and the zero-padded YYYY.MM.DD the
 * CI emits is not the only string that reaches this (a hand-tagged release,
 * a voice version typed into voice.json). Non-numeric fields compare as
 * strings. */
int  spfy_upd_version_cmp(const char *a, const char *b);

/* A build with no release behind it ("dev", "dev-1a2b3c4d", ""). Such a
 * build never gets an engine notification. */
int  spfy_upd_version_is_dev(const char *v);

/* Cached SHA-256 of a file, keyed on (path, bytes, mtime) in
 * <state dir>/stamps.json. Returns 0 on success and fills out_hex. */
int  spfy_upd_hash_cached(const char *path, char out_hex[65]);

/* Locally recorded voice metadata, read from <voice dir>/voice.json. Absent
 * or malformed leaves *version empty and n_files at 0, which downgrades the
 * comparison to size-only -- a hand-built voice is not an error. */
typedef struct {
    char      version[32];
    char      display[96];
    int       n_files;
    struct {
        char      name[64];
        char      sha256[65];
        long long bytes;
    } files[8];
} spfy_upd_local_voice;

void spfy_upd_local_voice_read(const char *voice_dir, spfy_upd_local_voice *lv);

#ifdef __cplusplus
}
#endif

#endif
