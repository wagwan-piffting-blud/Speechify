/* Voice catalog: list what can be downloaded, and install one.
 *
 * `spfy_synth --list-available` and `--install-voice NAME` exist so somebody
 * who cannot or will not navigate a GitHub releases page still gets a voice.
 * That is the whole feature: the engine already knows where voices live, the
 * updater already speaks HTTPS and JSON and SHA-256, and the release already
 * publishes a catalog. This joins them up.
 *
 * ⚠ THE CATALOG IS voices.json, THE SAME ONE pack_voices.py WRITES and the
 * Android app reads. Nothing here re-describes what a voice is.
 */

#include "spfy_update.h"
#include "upd_internal.h"
#include "mini_json.h"
#include "sha256.h"
#include "../common/env.h"
#include "../common/voice_find.h"
#include "../../third_party/miniz/miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#else
#  include <sys/types.h>
#  define MKDIR(p) mkdir((p), 0755)
#endif

#define VOICES_BASE \
    "https://github.com/wagwan-piffting-blud/Speechify/releases/download/voices/"

/* Generous: a voice zip is 66-232 MB and a slow line is a real thing. The
 * buffered manifest fetch keeps the short default. */
#define VOICE_TIMEOUT_S 1800

static void join(char *dst, size_t n, const char *a, const char *b)
{
#ifdef _WIN32
    snprintf(dst, n, "%s\\%s", a, b);
#else
    snprintf(dst, n, "%s/%s", a, b);
#endif
}

/* Every directory in the chain, so <root>/en-US/crsmara works from nothing. */
static int mkdirs(const char *path)
{
    char tmp[1024];
    size_t i, n = strlen(path);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = '\0';
            MKDIR(tmp);
            tmp[i] = c;
        }
    }
    MKDIR(tmp);
    /* MKDIR failing because the directory is already there is not a failure,
     * and telling that apart costs an errno.h include for nothing: every
     * caller's real question is whether the fopen that follows works. */
    return 0;
}

const char *spfy_voice_install_root(char *buf, size_t n)
{
    /* Exactly where --list-voices already looks, so a voice installed here is
     * one the very next command can use. SPFY_VOICE_DIR wins when set, which
     * is also what voice_find.c honours first. */
    const char *env = spfy_env("SPFY_VOICE_DIR");
    if (env && *env) {
        snprintf(buf, n, "%s", env);
        return buf;
    }
    {
        const char *home =
#ifdef _WIN32
            spfy_env("USERPROFILE");
#else
            spfy_env("HOME");
#endif
        if (!home || !*home) { snprintf(buf, n, "."); return buf; }
#ifdef _WIN32
        snprintf(buf, n, "%s\\Documents\\Speechify", home);
#else
        snprintf(buf, n, "%s/Documents/Speechify", home);
#endif
    }
    return buf;
}

static int trio_present(const char *root, const char *lang, const char *id)
{
    char d[1024], f[1200];
    FILE *fp;
    int i;
    const char *suffix[3];
    char v[3][160];

    snprintf(v[0], sizeof v[0], "%s.vin", id);
    snprintf(v[1], sizeof v[1], "%s8.vdb", id);
    snprintf(v[2], sizeof v[2], "%s.vcf", id);
    suffix[0] = v[0]; suffix[1] = v[1]; suffix[2] = v[2];

    join(d, sizeof d, root, lang);
    {
        char d2[1024];
        join(d2, sizeof d2, d, id);
        for (i = 0; i < 3; i++) {
            join(f, sizeof f, d2, suffix[i]);
            fp = fopen(f, "rb");
            if (!fp) return 0;
            fclose(fp);
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */

int spfy_voice_catalog(spfy_voice_avail *out, size_t max)
{
    char *body = NULL;
    size_t body_n = 0;
    char url[512];
    char root[1024];
    int n = 0;

    snprintf(url, sizeof url, "%svoices.json", VOICES_BASE);
    if (spfy_upd_fetch(url, 30, &body, &body_n) != 0 || !body) return -1;
    spfy_voice_install_root(root, sizeof root);

    {
        mj_doc doc;
        mj_node *nodes = (mj_node *)malloc(8192 * sizeof *nodes);
        int arr, it;
        if (!nodes) { free(body); return -1; }
        if (mj_parse(body, nodes, 8192, &doc) != 0) {
            free(nodes); free(body); return -1;
        }
        arr = mj_obj_get(&doc, 0, "voices");
        for (it = mj_arr_first(&doc, arr);
             it >= 0 && (size_t)n < max;
             it = mj_next(&doc, it)) {
            spfy_voice_avail *o = &out[n];
            memset(o, 0, sizeof *o);
            mj_str(&doc, mj_obj_get(&doc, it, "id"), o->id, sizeof o->id);
            /* A catalog entry with no zip was described but never packed;
             * offering it produces a download that 404s. */
            mj_str(&doc, mj_obj_get(&doc, it, "zip"), o->zip, sizeof o->zip);
            if (!o->id[0] || !o->zip[0]) continue;
            mj_str(&doc, mj_obj_get(&doc, it, "display"), o->display, sizeof o->display);
            mj_str(&doc, mj_obj_get(&doc, it, "lang"), o->lang, sizeof o->lang);
            mj_str(&doc, mj_obj_get(&doc, it, "version"), o->version, sizeof o->version);
            mj_str(&doc, mj_obj_get(&doc, it, "zip_sha256"),
                   o->zip_sha256, sizeof o->zip_sha256);
            o->zip_bytes = (unsigned long long)mj_num(
                &doc, mj_obj_get(&doc, it, "zip_bytes"), 0.0);
            if (!o->display[0]) snprintf(o->display, sizeof o->display, "%s", o->id);
            if (!o->lang[0]) snprintf(o->lang, sizeof o->lang, "?");
            o->installed = trio_present(root, o->lang, o->id);
            n++;
        }
        free(nodes);
    }
    free(body);
    return n;
}

/* ------------------------------------------------------------------ */

/* Case-blind ASCII compare, so `--install-voice CRSMARA` works. Local rather
 * than strcasecmp/_stricmp, which live in different headers per platform. */
static int ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

static int progress_bar(void *ctx, unsigned long long done,
                        unsigned long long total)
{
    int *last = (int *)ctx;
    int pct = total ? (int)((done * 100ULL) / total) : -1;
    if (pct < 0) {
        /* No Content-Length: MB is the only honest thing to show. */
        fprintf(stderr, "\r  %.0f MB", (double)done / 1048576.0);
    } else if (pct != *last) {
        *last = pct;
        fprintf(stderr, "\r  %3d%%  (%.0f / %.0f MB)", pct,
                (double)done / 1048576.0, (double)total / 1048576.0);
    }
    fflush(stderr);
    return 0;
}

int spfy_voice_install(const char *id, char *err, size_t err_n)
{
    spfy_voice_avail list[64];
    int n, i, found = -1;
    char root[1024], langdir[1200], vdir[1400], zippath[1200], url[640];
    char got_hex[65];
    int last_pct = -1;
    mz_zip_archive zip;
    mz_uint fi, nfiles;
    int rc = -1;

#define FAIL(...) do { if (err) snprintf(err, err_n, __VA_ARGS__); goto out; } while (0)

    if (err && err_n) err[0] = '\0';

    n = spfy_voice_catalog(list, sizeof list / sizeof list[0]);
    if (n < 0) FAIL("could not reach the voice catalog (offline?)");
    for (i = 0; i < n; i++) {
        if (ci_eq(list[i].id, id)) { found = i; break; }
    }
    if (found < 0) FAIL("no voice named '%s' in the catalog", id);

    spfy_voice_install_root(root, sizeof root);
    join(langdir, sizeof langdir, root, list[found].lang);
    join(vdir, sizeof vdir, langdir, list[found].id);

    snprintf(url, sizeof url, "%s%s", VOICES_BASE, list[found].zip);
    join(zippath, sizeof zippath, root, list[found].zip);
    if (mkdirs(root) != 0) FAIL("cannot create %s", root);

    fprintf(stderr, "downloading %s (%s, %.0f MB)\n", list[found].display,
            list[found].lang, (double)list[found].zip_bytes / 1048576.0);
    if (spfy_upd_fetch_file(url, VOICE_TIMEOUT_S, zippath,
                            progress_bar, &last_pct) != 0)
        FAIL("download failed -- check the network, or fetch %s by hand", url);
    fprintf(stderr, "\n");

    /* ⚠ VERIFY BEFORE UNPACKING, NOT AFTER. A truncated archive unpacked
     * first leaves a partial voice directory that looks complete enough to
     * load, and the failure then surfaces from inside the engine rather than
     * from the download that actually caused it. */
    if (spfy_sha256_file(zippath, got_hex) != 0) FAIL("cannot read %s", zippath);
    if (list[found].zip_sha256[0] && !ci_eq(got_hex, list[found].zip_sha256))
        FAIL("checksum mismatch -- the download is corrupt, try again");

    memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_file(&zip, zippath, 0))
        FAIL("%s is not a readable zip", zippath);
    nfiles = mz_zip_reader_get_num_files(&zip);
    for (fi = 0; fi < nfiles; fi++) {
        mz_zip_archive_file_stat st;
        const char *leaf;
        char dest[1600];
        if (!mz_zip_reader_file_stat(&zip, fi, &st)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, fi)) continue;
        /* The archive is <lang>/<id>/<file>; it already carries the layout
         * the voice search expects, so honour it rather than flattening. */
        leaf = strrchr(st.m_filename, '/');
        leaf = leaf ? leaf + 1 : st.m_filename;
        if (!*leaf) continue;
        if (strstr(st.m_filename, "..")) {
            mz_zip_reader_end(&zip);
            FAIL("refused a suspicious archive entry: %s", st.m_filename);
        }
        mkdirs(vdir);
        join(dest, sizeof dest, vdir, leaf);
        if (!mz_zip_reader_extract_to_file(&zip, fi, dest, 0)) {
            mz_zip_reader_end(&zip);
            FAIL("could not write %s", dest);
        }
    }
    mz_zip_reader_end(&zip);
    remove(zippath);

    printf("installed %s (%s) to %s\n", list[found].display,
           list[found].lang, vdir);
    rc = 0;
out:
    if (rc != 0) remove(zippath);
    return rc;
#undef FAIL
}
