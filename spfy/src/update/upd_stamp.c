/* Two small caches, both of them there to keep the check off the hot path.
 *
 *   stamps.json   SHA-256 keyed on (path, bytes, mtime). A 96 MB VDB hashes
 *                 in about half a second; doing that on every check, for
 *                 every installed voice, for a result that changes only when
 *                 the file does, would be the most expensive part of a
 *                 feature that is supposed to be invisible.
 *
 *   voice.json    what the voice packer recorded at build time. When its
 *                 sizes still match the files on disk we trust its hashes
 *                 and never read the audio at all -- the common case costs
 *                 three stat() calls per voice.
 *
 * A voice with no voice.json (hand-built, mid-experiment) is not an error.
 * It just downgrades to a size comparison, and hashes only if the sizes are
 * identical.
 */

#include "spfy_update.h"
#include "upd_internal.h"
#include "mini_json.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UPD_STAMP_MAX 128

typedef struct {
    char      path[512];
    long long bytes;
    long long mtime;
    char      sha256[65];
} stamp_t;

static int path_eq(const char *a, const char *b)
{
#ifdef _WIN32
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca == '/') ca = '\\';
        if (cb == '/') cb = '\\';
        if (ca != cb) return 0;
    }
    return *a == *b;
#else
    return strcmp(a, b) == 0;
#endif
}

static char *slurp(const char *path, long max_bytes)
{
    FILE *fp = fopen(path, "rb");
    long sz;
    char *buf;

    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    sz = ftell(fp);
    if (sz <= 0 || sz > max_bytes) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);
    buf[sz] = '\0';
    return buf;
}

static int stamps_load(stamp_t *out, int max)
{
    char path[1024];
    char *buf;
    mj_node *nodes;
    mj_doc doc;
    int n = 0, arr, it;

    if (spfy_upd_state_path(path, sizeof path, "stamps.json") != 0) return 0;
    buf = slurp(path, 1024L * 1024L);
    if (!buf) return 0;

    nodes = (mj_node *)malloc(sizeof(mj_node) * 2048);
    if (!nodes) { free(buf); return 0; }
    if (mj_parse(buf, nodes, 2048, &doc) != 0) {
        free(nodes); free(buf); return 0;
    }
    arr = mj_obj_get(&doc, 0, "stamps");
    for (it = mj_arr_first(&doc, arr); it >= 0 && n < max; it = mj_next(&doc, it)) {
        stamp_t *s = &out[n];
        memset(s, 0, sizeof *s);
        mj_str(&doc, mj_obj_get(&doc, it, "path"), s->path, sizeof s->path);
        mj_str(&doc, mj_obj_get(&doc, it, "sha256"), s->sha256, sizeof s->sha256);
        s->bytes = (long long)mj_num(&doc, mj_obj_get(&doc, it, "bytes"), 0.0);
        s->mtime = (long long)mj_num(&doc, mj_obj_get(&doc, it, "mtime"), 0.0);
        if (s->path[0] && s->sha256[0]) n++;
    }
    free(nodes);
    free(buf);
    return n;
}

static void json_escape_path(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', fp); fputc((int)c, fp); }
        else if (c >= 0x20)        { fputc((int)c, fp); }
    }
    fputc('"', fp);
}

static void stamps_save(const stamp_t *s, int n)
{
    char path[1024], tmp[1088];
    FILE *fp;
    int i;

    if (spfy_upd_state_path(path, sizeof path, "stamps.json") != 0) return;
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return;
    fp = fopen(tmp, "wb");
    if (!fp) return;
    fprintf(fp, "{\n  \"schema\": 1,\n  \"stamps\": [\n");
    for (i = 0; i < n; i++) {
        fprintf(fp, "    {\"path\": ");
        json_escape_path(fp, s[i].path);
        fprintf(fp, ", \"bytes\": %lld, \"mtime\": %lld, \"sha256\": \"%s\"}%s\n",
                s[i].bytes, s[i].mtime, s[i].sha256, i + 1 < n ? "," : "");
    }
    fprintf(fp, "  ]\n}\n");
    if (fclose(fp) != 0) return;
    remove(path);
    if (rename(tmp, path) != 0) remove(tmp);
}

int spfy_upd_hash_cached(const char *path, char out_hex[65])
{
    static stamp_t stamps[UPD_STAMP_MAX];
    static int loaded = 0, n = 0;
    long long bytes = 0, mtime = 0;
    int i;

    if (spfy_upd_file_stat(path, &bytes, &mtime) != 0) return -1;
    if (!loaded) { n = stamps_load(stamps, UPD_STAMP_MAX); loaded = 1; }

    for (i = 0; i < n; i++) {
        if (stamps[i].bytes == bytes && stamps[i].mtime == mtime &&
            path_eq(stamps[i].path, path)) {
            spfy_upd_strlcpy(out_hex, stamps[i].sha256, 65);
            return 0;
        }
    }
    if (spfy_sha256_file(path, out_hex) != 0) return -1;

    /* Replace an entry for the same path, else append. A full table drops
     * its oldest half rather than growing without bound -- this is a cache,
     * and re-hashing a file we forgot costs half a second once. */
    for (i = 0; i < n; i++) {
        if (path_eq(stamps[i].path, path)) break;
    }
    if (i == n) {
        if (n >= UPD_STAMP_MAX) {
            int keep = UPD_STAMP_MAX / 2;
            memmove(stamps, stamps + (UPD_STAMP_MAX - keep),
                    sizeof(stamp_t) * (size_t)keep);
            n = keep;
        }
        i = n++;
    }
    memset(&stamps[i], 0, sizeof stamps[i]);
    spfy_upd_strlcpy(stamps[i].path, path, sizeof stamps[i].path);
    stamps[i].bytes = bytes;
    stamps[i].mtime = mtime;
    spfy_upd_strlcpy(stamps[i].sha256, out_hex, sizeof stamps[i].sha256);
    stamps_save(stamps, n);
    return 0;
}

void spfy_upd_local_voice_read(const char *voice_dir, spfy_upd_local_voice *lv)
{
    char path[1024];
    char *buf;
    mj_node nodes[256];
    mj_doc doc;
    int arr, it;

    memset(lv, 0, sizeof *lv);
    if (snprintf(path, sizeof path, "%s%cvoice.json", voice_dir,
#ifdef _WIN32
                 '\\'
#else
                 '/'
#endif
                 ) >= (int)sizeof path)
        return;

    buf = slurp(path, 256L * 1024L);
    if (!buf) return;
    if (mj_parse(buf, nodes, (int)(sizeof nodes / sizeof nodes[0]), &doc) != 0) {
        free(buf);
        return;
    }
    mj_str(&doc, mj_obj_get(&doc, 0, "version"), lv->version, sizeof lv->version);
    mj_str(&doc, mj_obj_get(&doc, 0, "display"), lv->display, sizeof lv->display);

    arr = mj_obj_get(&doc, 0, "files");
    for (it = mj_arr_first(&doc, arr);
         it >= 0 && lv->n_files < (int)(sizeof lv->files / sizeof lv->files[0]);
         it = mj_next(&doc, it)) {
        int k = lv->n_files;
        mj_str(&doc, mj_obj_get(&doc, it, "name"),
               lv->files[k].name, sizeof lv->files[k].name);
        mj_str(&doc, mj_obj_get(&doc, it, "sha256"),
               lv->files[k].sha256, sizeof lv->files[k].sha256);
        lv->files[k].bytes =
            (long long)mj_num(&doc, mj_obj_get(&doc, it, "bytes"), 0.0);
        if (lv->files[k].name[0]) lv->n_files++;
    }
    free(buf);
}
