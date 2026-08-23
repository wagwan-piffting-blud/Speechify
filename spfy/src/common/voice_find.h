/* Resolve a bare voice NAME to its .vin/.vdb/.vcf triple.
 *
 * The three paths are always the same three paths, and typing them out is the
 * only reason a one-line synth is a three-line one. This walks the standard
 * layout instead:
 *
 *     <root>/<lang>/<voice>/<voice>.vin
 *                           <voice>8.vdb
 *                           <voice>.vcf
 *
 * A <lang> is any subdirectory shaped like a BCP-47 tag (`en-US`, `es-MX`,
 * `fr-CA`), matched by SHAPE and not from a list, so a new language needs no
 * code change here.
 *
 * Matching on <voice> is case-insensitive and the REAL casing is what gets
 * used to build the filenames -- `tom`, `Tom` and `TOM` all find `tom/tom.vin`.
 * That matters off Windows, where the filesystem will not paper over it.
 */

#ifndef SPFY_COMMON_VOICE_FIND_H
#define SPFY_COMMON_VOICE_FIND_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[128];     /* folder name, in its real casing        */
    char lang[32];      /* language directory, e.g. "en-US"       */
    char dir[1024];     /* the voice directory itself             */
    char vin[1024];
    char vdb[1024];
    char vcf[1024];
} spfy_voice_paths;

/* Does this token look like a bare voice name rather than a file path?
 *
 * A name has no separator, no drive letter and no recognised voice-file
 * extension. `tom` is a name; `tom.vin`, `en-US/tom/tom.vin` and `C:\x.vin`
 * are not. A DIRECTORY path is deliberately NOT a name but is still accepted
 * by spfy_voice_resolve(). */
int spfy_voice_is_name(const char *s);

/* Resolve `name` -- a bare voice name or a path to a voice DIRECTORY -- into
 * `out`. `argv0` may be NULL; when given, the executable's own directory and
 * its ancestors join the search.
 *
 * Returns 0 on success, -1 when no voice matched, -2 when the folder matched
 * but the triple is incomplete (and then out->dir names the folder and the
 * missing member's path is left empty, so the caller can say which). */
int spfy_voice_resolve(const char *name, const char *argv0,
                       spfy_voice_paths *out);

/* Every voice found under every search root, sorted by lang then name and
 * de-duplicated by lang/name (the first root wins). Returns the count
 * WRITTEN, which is capped at `max`. */
size_t spfy_voice_list(const char *argv0, spfy_voice_paths *out, size_t max);

/* The roots that were searched, newline-separated, for an error message.
 * Points at a static buffer refilled by each resolve/list call. */
const char *spfy_voice_search_path(void);

#ifdef __cplusplus
}
#endif

#endif
