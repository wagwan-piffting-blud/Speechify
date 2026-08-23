/* Manifest parsing and the comparison against what is actually installed. */

#include "spfy_update.h"
#include "upd_internal.h"
#include "mini_json.h"
#include "voice_find.h"
#include "spfy/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define UPD_SEP '\\'
#else
#  define UPD_SEP '/'
#endif

int spfy_upd_version_is_dev(const char *v)
{
    if (!v || !*v) return 1;
    return strncmp(v, "dev", 3) == 0;
}

int spfy_upd_version_cmp(const char *a, const char *b)
{
    for (;;) {
        const char *ea, *eb;
        size_t la, lb;
        int a_num = 1, b_num = 1;
        size_t i;

        while (*a == '.') a++;
        while (*b == '.') b++;
        if (!*a && !*b) return 0;
        if (!*a) return -1;
        if (!*b) return  1;

        ea = strchr(a, '.');
        eb = strchr(b, '.');
        la = ea ? (size_t)(ea - a) : strlen(a);
        lb = eb ? (size_t)(eb - b) : strlen(b);

        for (i = 0; i < la; i++) if (a[i] < '0' || a[i] > '9') { a_num = 0; break; }
        for (i = 0; i < lb; i++) if (b[i] < '0' || b[i] > '9') { b_num = 0; break; }

        if (a_num && b_num) {
            /* Numeric, so 2026.9 < 2026.10 -- which strcmp gets backwards
             * the moment a version is not zero-padded. */
            long va = strtol(a, NULL, 10);
            long vb = strtol(b, NULL, 10);
            if (va != vb) return va < vb ? -1 : 1;
        } else {
            size_t n = la < lb ? la : lb;
            int c = memcmp(a, b, n);
            if (c) return c < 0 ? -1 : 1;
            if (la != lb) return la < lb ? -1 : 1;
        }
        a += la;
        b += lb;
    }
}

int spfy_upd_manifest_parse(const char *json, size_t n, spfy_upd_manifest *m)
{
    mj_node *nodes;
    mj_doc doc;
    int eng, arr, it, rc = 0;

    (void)n;
    memset(m, 0, sizeof *m);
    nodes = (mj_node *)malloc(sizeof(mj_node) * 8192);
    if (!nodes) return -1;
    if (mj_parse(json, nodes, 8192, &doc) != 0) { free(nodes); return -1; }

    m->schema = (int)mj_num(&doc, mj_obj_get(&doc, 0, "schema"), 0.0);
    /* Schema 1 is what this build understands. A future manifest that bumps
     * it is telling us it changed shape incompatibly, and the honest thing
     * is to stay quiet rather than misreport. */
    if (m->schema != 1) { free(nodes); return -2; }

    eng = mj_obj_get(&doc, 0, "engine");
    if (eng >= 0) {
        mj_str(&doc, mj_obj_get(&doc, eng, "version"),
               m->engine_version, sizeof m->engine_version);
        mj_str(&doc, mj_obj_get(&doc, eng, "url"),
               m->engine_url, sizeof m->engine_url);
        mj_str(&doc, mj_obj_get(&doc, eng, "message"),
               m->message, sizeof m->message);
        m->engine_notify = mj_bool(&doc, mj_obj_get(&doc, eng, "notify"), 1);
    }

    arr = mj_obj_get(&doc, 0, "voices");
    for (it = mj_arr_first(&doc, arr);
         it >= 0 && m->n_voices < SPFY_UPD_MAX_VOICES;
         it = mj_next(&doc, it)) {
        spfy_upd_voice *v = &m->voices[m->n_voices];
        int farr, fit;

        memset(v, 0, sizeof *v);
        mj_str(&doc, mj_obj_get(&doc, it, "id"),      v->id,      sizeof v->id);
        mj_str(&doc, mj_obj_get(&doc, it, "display"), v->display, sizeof v->display);
        mj_str(&doc, mj_obj_get(&doc, it, "lang"),    v->lang,    sizeof v->lang);
        mj_str(&doc, mj_obj_get(&doc, it, "version"), v->version, sizeof v->version);
        mj_str(&doc, mj_obj_get(&doc, it, "url"),     v->url,     sizeof v->url);
        v->zip_bytes = (long long)mj_num(&doc,
                            mj_obj_get(&doc, it, "zip_bytes"), 0.0);
        v->notify = mj_bool(&doc, mj_obj_get(&doc, it, "notify"), 1);

        farr = mj_obj_get(&doc, it, "files");
        for (fit = mj_arr_first(&doc, farr);
             fit >= 0 && v->n_files < SPFY_UPD_MAX_FILES;
             fit = mj_next(&doc, fit)) {
            spfy_upd_file *f = &v->files[v->n_files];
            memset(f, 0, sizeof *f);
            mj_str(&doc, mj_obj_get(&doc, fit, "name"), f->name, sizeof f->name);
            mj_str(&doc, mj_obj_get(&doc, fit, "sha256"),
                   f->sha256, sizeof f->sha256);
            f->bytes = (long long)mj_num(&doc,
                            mj_obj_get(&doc, fit, "bytes"), 0.0);
            if (f->name[0]) v->n_files++;
        }
        if (v->id[0]) m->n_voices++;
    }

    free(nodes);
    return rc;
}

static int ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return *a == *b;
}

/* "crstom@2026.08.22" present in the comma-separated dismissal list? */
static int is_dismissed(const char *list, const char *id, const char *version)
{
    char want[128];
    const char *p;
    size_t n;

    if (!list || !*list) return 0;
    if (snprintf(want, sizeof want, "%s@%s", id, version) >= (int)sizeof want)
        return 0;
    n = strlen(want);
    for (p = list; *p; ) {
        const char *comma = strchr(p, ',');
        size_t seg = comma ? (size_t)(comma - p) : strlen(p);
        if (seg == n && memcmp(p, want, n) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

/* The manifest's sha256 for a local file, when we can get one cheaply.
 * voice.json is trusted only while its recorded size still matches what is
 * on disk; otherwise the file is hashed (and cached). */
static int local_hash(const char *path, const char *name,
                      long long on_disk_bytes,
                      const spfy_upd_local_voice *lv, char out[65])
{
    int i;
    for (i = 0; i < lv->n_files; i++) {
        if (ci_eq(lv->files[i].name, name) &&
            lv->files[i].bytes == on_disk_bytes &&
            lv->files[i].sha256[0]) {
            spfy_upd_strlcpy(out, lv->files[i].sha256, 65);
            return 0;
        }
    }
    return spfy_upd_hash_cached(path, out);
}

static int compare_one(const spfy_voice_paths *lp, const spfy_upd_voice *rv,
                       const spfy_upd_local_voice *lv)
{
    int i;
    int hashed_any = 0;

    for (i = 0; i < rv->n_files; i++) {
        char path[1200];
        char hex[65];
        long long bytes = 0;

        if (snprintf(path, sizeof path, "%s%c%s", lp->dir, UPD_SEP,
                     rv->files[i].name) >= (int)sizeof path)
            continue;
        if (spfy_upd_file_stat(path, &bytes, NULL) != 0)
            return SPFY_UPD_R_MISSING;
        if (rv->files[i].bytes > 0 && bytes != rv->files[i].bytes)
            return SPFY_UPD_R_SIZE;
        if (!rv->files[i].sha256[0])
            continue;
        if (local_hash(path, rv->files[i].name, bytes, lv, hex) != 0)
            continue;
        hashed_any = 1;
        if (!ci_eq(hex, rv->files[i].sha256))
            return SPFY_UPD_R_HASH;
    }

    /* Nothing verifiable in the manifest: fall back to the version string,
     * which is all a hand-written entry may carry. */
    if (!hashed_any && rv->n_files == 0 && rv->version[0] && lv->version[0] &&
        spfy_upd_version_cmp(rv->version, lv->version) > 0)
        return SPFY_UPD_R_VERSION;

    return SPFY_UPD_R_NONE;
}

int spfy_upd_compare(const spfy_upd_manifest *m, const char *argv0,
                     const spfy_upd_state *st, spfy_upd_report *rep)
{
    static spfy_voice_paths local[SPFY_UPD_MAX_VOICES];
    size_t n_local, i;
    int j;

    memset(rep, 0, sizeof *rep);
    spfy_upd_strlcpy(rep->local_version, SPFY_VERSION, sizeof rep->local_version);
    spfy_upd_strlcpy(rep->remote_version, m->engine_version,
                     sizeof rep->remote_version);
    spfy_upd_strlcpy(rep->engine_url,
                     m->engine_url[0] ? m->engine_url : SPFY_RELEASES_URL,
                     sizeof rep->engine_url);
    spfy_upd_strlcpy(rep->message, m->message, sizeof rep->message);

    if (m->engine_notify && m->engine_version[0] &&
        !spfy_upd_version_is_dev(SPFY_VERSION) &&
        spfy_upd_version_cmp(m->engine_version, SPFY_VERSION) > 0 &&
        !(st && st->skip_engine[0] &&
          strcmp(st->skip_engine, m->engine_version) == 0))
        rep->engine_update = 1;

    n_local = spfy_voice_list(argv0, local, sizeof local / sizeof local[0]);
    for (i = 0; i < n_local && rep->n_voices < SPFY_UPD_MAX_VOICES; i++) {
        spfy_upd_local_voice lv;
        int reason;

        for (j = 0; j < m->n_voices; j++)
            if (ci_eq(m->voices[j].id, local[i].name)) break;
        if (j >= m->n_voices) continue;              /* not ours to update */
        if (!m->voices[j].notify) continue;

        spfy_upd_local_voice_read(local[i].dir, &lv);
        reason = compare_one(&local[i], &m->voices[j], &lv);
        if (reason == SPFY_UPD_R_NONE) continue;
        if (is_dismissed(st ? st->skip_voices : NULL,
                         m->voices[j].id, m->voices[j].version))
            continue;

        {
            spfy_upd_voice_result *r = &rep->voices[rep->n_voices++];
            memset(r, 0, sizeof *r);
            spfy_upd_strlcpy(r->id, m->voices[j].id, sizeof r->id);
            spfy_upd_strlcpy(r->display,
                             m->voices[j].display[0] ? m->voices[j].display
                                                     : local[i].name,
                             sizeof r->display);
            spfy_upd_strlcpy(r->lang, local[i].lang, sizeof r->lang);
            spfy_upd_strlcpy(r->local_version, lv.version,
                             sizeof r->local_version);
            spfy_upd_strlcpy(r->remote_version, m->voices[j].version,
                             sizeof r->remote_version);
            spfy_upd_strlcpy(r->url, m->voices[j].url, sizeof r->url);
            r->zip_bytes = m->voices[j].zip_bytes;
            r->reason    = reason;
        }
    }

    return rep->engine_update + rep->n_voices;
}
