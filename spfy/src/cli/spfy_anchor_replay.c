/* spfy_anchor_replay -- M3.4r Phase B4.4 / M5 anchor scorer validation.
 *
 * Replays captured engine traces through spfy_anchor_score() and reports
 * top-N match rate against engine's chosen cands. Mirrors v7's main loop
 * (anchor_predp_v7.py) but uses the C implementation.
 *
 * Usage:
 *   spfy_anchor_replay <voice.vin> <voice.vcf> <hpclass.bin> <traces_dir>
 *
 * traces_dir must contain:
 *   anchor_score/text_*.jsonl
 *   viterbi_dp/text_*.jsonl
 *   prsl_slot/text_*.jsonl
 *   inner_scorer/text_*.jsonl
 *   cart_walks/text_*.jsonl
 *
 * Reports total slots / matches across the corpus.
 */

#include <spfy/spfy.h>
#include "../usel/anchor_score.h"
#include "../voice/voice.h"
#include "../voice/unit_table.h"
#include "../voice/ccos.h"
#include "../voice/voice_runtime.h"
#include "../voice/vcf_matrix.h"
#include "../voice/chunk_table.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- minimal JSONL field extractor ----------------- */

static const char *find_lit(const char *p, const char *end, const char *lit)
{
    size_t lp = strlen(lit);
    if ((size_t)(end - p) < lp) return NULL;
    for (const char *q = p; q + lp <= end; ++q) {
        if (memcmp(q, lit, lp) == 0) return q;
    }
    return NULL;
}

static int read_key_i64(const char *s, const char *e, const char *key,
                        int64_t *out)
{
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = find_lit(s, e, needle);
    if (!p) return -1;
    p += strlen(needle);
    char *ep = NULL;
    long long v = strtoll(p, &ep, 10);
    if (ep == p) return -1;
    *out = (int64_t)v;
    return 0;
}

static int read_key_f64(const char *s, const char *e, const char *key,
                        double *out)
{
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = find_lit(s, e, needle);
    if (!p) return -1;
    p += strlen(needle);
    char *ep = NULL;
    double v = strtod(p, &ep);
    if (ep == p) return -1;
    *out = v;
    return 0;
}

/* Extract a JSON int array "key":[a,b,c,...] from a line. Writes up to
 * out_cap entries, returns count written. -1 on parse error. */
static int read_key_int_array(const char *s, const char *e, const char *key,
                              int64_t *out, int out_cap)
{
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\":[", key);
    const char *p = find_lit(s, e, needle);
    if (!p) return -1;
    p += strlen(needle);
    int n = 0;
    while (p < e && n < out_cap) {
        while (p < e && (*p == ' ' || *p == ',')) ++p;
        if (p >= e || *p == ']') break;
        char *ep = NULL;
        long long v = strtoll(p, &ep, 10);
        if (ep == p) break;
        out[n++] = v;
        p = ep;
    }
    return n;
}

/* ---------------- per-utt accumulators ----------------- */

#define MAX_HP 256
#define MAX_CANDS 200

typedef struct {
    int32_t  ctx[5];
    int      have;
} hp_ctx_t;

typedef struct {
    spfy_anchor_sp_target_t  sp;
    int                      have;
} hp_sp_t;

typedef struct {
    spfy_anchor_cart_t  cart;
} hp_cart_t;

typedef struct {
    /* Per HP slot indices (IS-slot idx). */
    hp_ctx_t   ctxs[MAX_HP];
    hp_sp_t    sps[MAX_HP];
    hp_cart_t  carts[MAX_HP];
    /* Voicing array (per-hp_class). */
    int32_t    voicing[256];
    int        voicing_n;
    /* Anchors: list of anchor_score events with first_hp/last_hp + uid. */
    struct {
        int32_t  type;
        int32_t  first_hp;
        int32_t  last_hp;
        int32_t  cand_uid;
        int32_t  cand_jk;
        int32_t  final_n_cands;       /* used to disambiguate same (uid,jk) */
        int64_t  syl_idx[MAX_HP];
        int      n_syl;
    } as[MAX_CANDS];
    int n_as;
    /* Engine kept cands per anchor slot. Indexed by viterbi slot_idx. */
    struct {
        int      is_anchor;
        int32_t  uid;
        int32_t  jk;
        int      n_cands;
        int32_t  cands_uid[16];
        int32_t  cands_jk[16];
    } vit[1024];
    int n_vit;
} per_utt_t;

static int read_file_lines(const char *path, char **out_buf, size_t *out_n)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return -1; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return -1;
    }
    buf[sz] = '\0';
    fclose(fp);
    *out_buf = buf;
    *out_n = (size_t)sz;
    return 0;
}

/* Iterate JSONL lines. Returns 1 if line found, 0 at EOF. */
static int next_line(const char **p, const char *end,
                     const char **line_start, const char **line_end)
{
    const char *s = *p;
    while (s < end && (*s == '\n' || *s == '\r')) ++s;
    if (s >= end) return 0;
    *line_start = s;
    while (s < end && *s != '\n') ++s;
    *line_end = s;
    *p = s;
    return 1;
}

/* Load a per-utt structure from the 5 trace files for one text_*.jsonl. */
static int load_utt(const char *traces_dir, const char *fname, int utt_idx,
                    per_utt_t *u)
{
    memset(u, 0, sizeof *u);
    char path[1024];

    /* anchor_score: list per anchor, with first_hp/last_hp/type/uid/jk/syl_idx. */
    snprintf(path, sizeof path, "%s/anchor_score/%s", traces_dir, fname);
    char *buf = NULL; size_t buf_n = 0;
    if (read_file_lines(path, &buf, &buf_n) == 0) {
        const char *p = buf, *end = buf + buf_n;
        const char *ls, *le;
        while (next_line(&p, end, &ls, &le)) {
            int64_t v;
            if (read_key_i64(ls, le, "first_hp", &v) != 0) continue;
            int32_t first_hp = (int32_t)v;
            if (read_key_i64(ls, le, "last_hp", &v) != 0) continue;
            int32_t last_hp = (int32_t)v;
            if (read_key_i64(ls, le, "type", &v) != 0) continue;
            int32_t type = (int32_t)v;
            int32_t cand_uid = -1, cand_jk = -1;
            if (read_key_i64(ls, le, "first_cand_uid", &v) == 0) cand_uid = (int32_t)v;
            if (read_key_i64(ls, le, "first_cand_jk", &v) == 0)  cand_jk  = (int32_t)v;
            if (cand_uid < 0 || cand_jk < 0) continue;
            if (u->n_as >= MAX_CANDS) break;
            int idx = u->n_as++;
            u->as[idx].first_hp = first_hp;
            u->as[idx].last_hp = last_hp;
            u->as[idx].type = type;
            u->as[idx].cand_uid = cand_uid;
            u->as[idx].cand_jk = cand_jk;
            int64_t fnc = -1;
            if (read_key_i64(ls, le, "final_n_cands", &fnc) == 0)
                u->as[idx].final_n_cands = (int32_t)fnc;
            else
                u->as[idx].final_n_cands = -1;
            int n = read_key_int_array(ls, le, "syl_idx_0_to_last",
                                        u->as[idx].syl_idx, MAX_HP);
            u->as[idx].n_syl = (n > 0) ? n : 0;
        }
        free(buf);
    }
    /* anchor_score has no utt boundary; assume single-utt files. */

    /* prsl_slot: per-HP ctx[5], grouped by slot=0 reset.
     * Multi-utt files (e.g. text_001 has 2 viterbi calls) share one
     * prsl_slot/inner_scorer/cart_walks file with slot=0 marking utt
     * boundaries. We must take ONLY events from the requested utt_idx,
     * else later utts overwrite earlier ones with conflicting ctx.
     * (Bug discovered while porting v7 anchor scorer to C: text_001 utt0
     * slot 2 ctx=[64,64,38,...] vs utt1's [64,64,82,...]; using utt1's
     * value when scoring utt0's slot 8 fails the hp_class boundary check
     * for the engine-chosen cand.) */
    snprintf(path, sizeof path, "%s/prsl_slot/%s", traces_dir, fname);
    if (read_file_lines(path, &buf, &buf_n) == 0) {
        const char *p = buf, *end = buf + buf_n;
        const char *ls, *le;
        int cur_utt = -1;        /* incremented to 0 on first slot==0 */
        while (next_line(&p, end, &ls, &le)) {
            if (!find_lit(ls, le, "\"type\":\"prsl_slot\"")) continue;
            int64_t slot_v;
            if (read_key_i64(ls, le, "slot", &slot_v) != 0) continue;
            int slot = (int)slot_v;
            if (slot == 0) ++cur_utt;
            if (cur_utt != utt_idx) continue;
            int64_t arr[8];
            int n = read_key_int_array(ls, le, "ctx", arr, 8);
            if (n != 5 || slot >= MAX_HP) continue;
            for (int i = 0; i < 5; ++i) u->ctxs[slot].ctx[i] = (int32_t)arr[i];
            u->ctxs[slot].have = 1;
        }
        free(buf);
    }

    /* inner_scorer: per-HP sp_target [5]; voicing from join_consts.
     * Per-utt grouped by slot=0 reset (same as prsl_slot). */
    snprintf(path, sizeof path, "%s/inner_scorer/%s", traces_dir, fname);
    if (read_file_lines(path, &buf, &buf_n) == 0) {
        const char *p = buf, *end = buf + buf_n;
        const char *ls, *le;
        int cur_utt = -1;
        while (next_line(&p, end, &ls, &le)) {
            if (find_lit(ls, le, "\"type\":\"join_consts\"")) {
                /* Look for "by_hp_class":[...]. */
                const char *bh = find_lit(ls, le,
                    "\"by_hp_class\":[");
                if (bh) {
                    bh += strlen("\"by_hp_class\":[");
                    int64_t arr[256];
                    int n = 0;
                    const char *q = bh;
                    while (q < le && n < 256) {
                        while (q < le && (*q == ' ' || *q == ',')) ++q;
                        if (q >= le || *q == ']') break;
                        char *ep = NULL;
                        long long v = strtoll(q, &ep, 10);
                        if (ep == q) break;
                        arr[n++] = v;
                        q = ep;
                    }
                    /* voicing array typically n_labels*2 entries; rest may be junk. */
                    int keep = (n < 256) ? n : 256;
                    for (int i = 0; i < keep; ++i)
                        u->voicing[i] = (int32_t)arr[i];
                    u->voicing_n = keep;
                }
                continue;
            }
            if (!find_lit(ls, le, "\"type\":\"inner_scorer\"")) continue;
            int64_t slot_v;
            if (read_key_i64(ls, le, "slot", &slot_v) != 0) continue;
            int slot = (int)slot_v;
            if (slot == 0) ++cur_utt;
            if (cur_utt != utt_idx) continue;
            if (slot >= MAX_HP) continue;
            int64_t arr[8];
            int n = read_key_int_array(ls, le, "sp_target", arr, 8);
            if (n != 5) continue;
            for (int i = 0; i < 5; ++i) u->sps[slot].sp.sp[i] = (uint32_t)arr[i];
            u->sps[slot].have = 1;
        }
        free(buf);
    }

    /* cart_walks: per-HP durt + f0tr leaves.
     * Per-utt grouped by slot=0 reset. */
    snprintf(path, sizeof path, "%s/cart_walks/%s", traces_dir, fname);
    if (read_file_lines(path, &buf, &buf_n) == 0) {
        const char *p = buf, *end = buf + buf_n;
        const char *ls, *le;
        int cur_utt = -1;
        while (next_line(&p, end, &ls, &le)) {
            if (!find_lit(ls, le, "\"type\":\"cart_walk\"")) continue;
            int64_t slot_v;
            if (read_key_i64(ls, le, "slot", &slot_v) != 0) continue;
            int slot = (int)slot_v;
            if (slot == 0) ++cur_utt;
            if (cur_utt != utt_idx) continue;
            if (slot >= MAX_HP) continue;
            double m, v;
            if (read_key_f64(ls, le, "leaf_mean", &m) != 0) continue;
            if (read_key_f64(ls, le, "leaf_var",  &v) != 0) continue;
            int is_durt = find_lit(ls, le, "\"tree\":\"durt\"") != NULL;
            int is_f0tr = find_lit(ls, le, "\"tree\":\"f0tr\"") != NULL;
            if (is_durt) {
                u->carts[slot].cart.durt_mean = (float)m;
                u->carts[slot].cart.durt_var  = (float)v;
                u->carts[slot].cart.durt_valid = 1;
            } else if (is_f0tr) {
                u->carts[slot].cart.f0tr_mean = (float)m;
                u->carts[slot].cart.f0tr_var  = (float)v;
                u->carts[slot].cart.f0tr_valid = 1;
            }
        }
        free(buf);
    }

    /* viterbi_dp: per-anchor-slot kept cands list.
     * Multi-utt files have one viterbi_enter per utt; pick the one
     * matching utt_idx. */
    snprintf(path, sizeof path, "%s/viterbi_dp/%s", traces_dir, fname);
    if (read_file_lines(path, &buf, &buf_n) == 0) {
        const char *p = buf, *end = buf + buf_n;
        const char *ls, *le;
        int enter_seen = -1;
        while (next_line(&p, end, &ls, &le)) {
            if (!find_lit(ls, le, "\"stage\":\"enter\"")) continue;
            ++enter_seen;
            if (enter_seen != utt_idx) continue;
            /* Walk through "slots":[...] manually. */
            const char *slots_start = find_lit(ls, le, "\"slots\":[");
            if (!slots_start) continue;
            const char *q = slots_start + strlen("\"slots\":[");
            int slot_idx = 0;
            int depth = 1;
            while (q < le && depth > 0) {
                if (*q == '{') {
                    /* Slot opener -- find matching close. */
                    const char *slot_open = q;
                    int sd = 1;
                    ++q;
                    while (q < le && sd > 0) {
                        if (*q == '{') ++sd;
                        else if (*q == '}') --sd;
                        ++q;
                    }
                    /* Slot content: slot_open..q. */
                    if (slot_idx >= 1024) { ++slot_idx; continue; }
                    /* Look for cands array. */
                    const char *cb = find_lit(slot_open, q, "\"cands\":[");
                    if (!cb) { u->vit[slot_idx].n_cands = 0; ++slot_idx; continue; }
                    const char *r = cb + strlen("\"cands\":[");
                    int n_cands = 0;
                    int first_uid = -1, first_jk = -1;
                    while (r < q && n_cands < 16) {
                        if (*r != '{') { ++r; continue; }
                        const char *cs = r;
                        int cd = 1; ++r;
                        while (r < q && cd > 0) {
                            if (*r == '{') ++cd;
                            else if (*r == '}') --cd;
                            ++r;
                        }
                        int64_t uid_v, jk_v;
                        if (read_key_i64(cs, r, "uid", &uid_v) == 0
                            && read_key_i64(cs, r, "join_key", &jk_v) == 0) {
                            if (n_cands == 0) {
                                first_uid = (int32_t)uid_v;
                                first_jk  = (int32_t)jk_v;
                            }
                            u->vit[slot_idx].cands_uid[n_cands] = (int32_t)uid_v;
                            u->vit[slot_idx].cands_jk[n_cands]  = (int32_t)jk_v;
                            ++n_cands;
                        }
                    }
                    u->vit[slot_idx].n_cands = n_cands;
                    if (n_cands > 0) {
                        u->vit[slot_idx].uid = first_uid;
                        u->vit[slot_idx].jk  = first_jk;
                        u->vit[slot_idx].is_anchor = (first_uid != first_jk);
                    }
                    ++slot_idx;
                } else if (*q == ']') {
                    --depth;
                    ++q;
                } else {
                    ++q;
                }
            }
            u->n_vit = slot_idx;
            break;   /* found the requested utt's enter; done */
        }
        free(buf);
    }

    return 0;
}

/* Count the number of "stage":"enter" events in a viterbi_dp file. */
static int count_viterbi_utts(const char *traces_dir, const char *fname)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/viterbi_dp/%s", traces_dir, fname);
    char *buf = NULL; size_t buf_n = 0;
    if (read_file_lines(path, &buf, &buf_n) != 0) return 0;
    const char *p = buf, *end = buf + buf_n;
    const char *ls, *le;
    int n = 0;
    while (next_line(&p, end, &ls, &le)) {
        if (find_lit(ls, le, "\"stage\":\"enter\"")) ++n;
    }
    free(buf);
    return n;
}

/* Find the cklx posting list for a given (ss, se) anchor. We search both
 * groups for the matching span. Returns the (group, key index, n_postings,
 * postings array). */
static int find_cklx_postings(const spfy_chunk_tables_t *ct,
                               int anchor_type,
                               int32_t ss, int32_t se, int *out_grp,
                               const uint32_t **out_postings,
                               uint32_t *out_n)
{
    /* type=2 (Syl) -> _SYL_ group (idx 1).
     * type=4 (Word) -> _WORD_ group (idx 0). */
    int g = (anchor_type == 2) ? SPFY_CHUNK_GROUP_SYL : SPFY_CHUNK_GROUP_WORD;
    const spfy_ckls_group_t *cg = &ct->ckls[g];
    const spfy_cklx_group_t *xg = &ct->cklx[g];
    for (uint32_t pi = 0; pi < cg->n_postings; ++pi) {
        if (cg->span_start[pi] == (uint32_t)ss
            && cg->span_end[pi] == (uint32_t)se) {
            const char *tok = cg->token_text[pi];
            const uint32_t *posts = NULL;
            uint32_t n = 0;
            if (spfy_cklx_lookup(xg, tok, &posts, &n) > 0) {
                *out_grp = g;
                *out_postings = posts;
                *out_n = n;
                return 1;
            }
        }
    }
    return 0;
}

/* List directory entries matching prefix "text_" and ending in ".jsonl". */
#include <dirent.h>
static int list_text_files(const char *dir, char ***out, int *out_n)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    char **list = NULL;
    int cap = 0, n = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        if (strncmp(nm, "text_", 5) != 0) continue;
        size_t l = strlen(nm);
        if (l < 6 || strcmp(nm + l - 6, ".jsonl") != 0) continue;
        if (n >= cap) {
            cap = cap ? cap * 2 : 16;
            list = (char **)realloc(list, (size_t)cap * sizeof *list);
        }
        list[n++] = strdup(nm);
    }
    closedir(d);
    /* Sort alphabetically. */
    for (int i = 0; i < n - 1; ++i)
        for (int j = i + 1; j < n; ++j)
            if (strcmp(list[i], list[j]) > 0) {
                char *t = list[i]; list[i] = list[j]; list[j] = t;
            }
    *out = list;
    *out_n = n;
    return 0;
}

/* ---------------- main ----------------- */

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr,
            "usage: %s <voice.vin> <voice.vcf> <hpclass.bin> <traces_dir>\n",
            argv[0]);
        return 2;
    }
    const char *vin_path = argv[1];
    const char *vcf_path = argv[2];
    const char *hpclass_path = argv[3];
    const char *traces_dir = argv[4];

    spfy_vin_t vin = {0};
    spfy_vcf_t vcf = {0};
    if (spfy_vin_load(vin_path, &vin) != SPFY_OK) {
        fprintf(stderr, "VIN load failed\n"); return 3;
    }
    if (spfy_vcf_load(vcf_path, &vcf) != SPFY_OK) {
        fprintf(stderr, "VCF load failed\n"); return 3;
    }
    spfy_unit_table_t units = {0};
    spfy_ccos_t ccos = {0};
    spfy_voice_maps_t maps = {0};
    spfy_proscost_matrix_t proscost[SPFY_PROSCOST_N] = {0};
    spfy_chunk_tables_t ct = {0};
    if (spfy_unit_table_load(&vin, &units) != SPFY_OK) {
        fprintf(stderr, "unit_table load failed\n"); return 3;
    }
    if (spfy_ccos_load(&vin, &ccos) != SPFY_OK) {
        fprintf(stderr, "ccos load failed\n"); return 3;
    }
    if (spfy_voice_maps_build(&ccos, &maps) != SPFY_OK) {
        fprintf(stderr, "voice_maps build failed\n"); return 3;
    }
    if (spfy_proscost_load(&vcf, proscost) != SPFY_OK) {
        fprintf(stderr, "proscost load failed\n"); return 3;
    }
    if (spfy_chunk_tables_load(&vin, &ct) != SPFY_OK) {
        fprintf(stderr, "chunk_tables load failed\n"); return 3;
    }
    uint8_t *hpclass = NULL;
    uint32_t hpclass_n = 0;
    if (spfy_anchor_hpclass_load(hpclass_path, &hpclass, &hpclass_n) != SPFY_OK) {
        fprintf(stderr, "hpclass load failed: %s\n", hpclass_path);
        return 3;
    }

    spfy_anchor_voice_t av = {0};
    av.units = &units;
    av.ccos = &ccos;
    av.maps = &maps;
    av.proscost = proscost;
    av.hpclass_table = hpclass;
    av.hpclass_n = hpclass_n;
    spfy_anchor_voice_set_default_weights(&av);

    /* Iterate corpus. */
    char **files = NULL;
    int n_files = 0;
    char as_dir[1024];
    snprintf(as_dir, sizeof as_dir, "%s/anchor_score", traces_dir);
    if (list_text_files(as_dir, &files, &n_files) != 0 || n_files == 0) {
        fprintf(stderr, "no text_*.jsonl in %s\n", as_dir);
        return 3;
    }

    int total_anchors = 0, total_match = 0;
    const char *only_text = getenv("SPFY_ONLY_TEXT");

    for (int fi = 0; fi < n_files; ++fi) {
        if (only_text && strstr(files[fi], only_text) == NULL) continue;
        int n_utts = count_viterbi_utts(traces_dir, files[fi]);
        if (n_utts <= 0) n_utts = 1;
      for (int utt_iter = 0; utt_iter < n_utts; ++utt_iter) {
        per_utt_t u;
        if (load_utt(traces_dir, files[fi], utt_iter, &u) != 0) continue;

        /* For each viterbi anchor slot (uid != jk), look up the matching
         * anchor_score entry by (uid, jk), pull first_hp/last_hp,
         * find cklx postings, run anchor_score, compare top-N. */
        av.voicing = (const uint32_t *)u.voicing;
        av.voicing_n = (uint32_t)u.voicing_n;

        for (int vi = 0; vi < u.n_vit; ++vi) {
            if (!u.vit[vi].is_anchor || u.vit[vi].n_cands == 0) continue;
            int32_t want_uid = u.vit[vi].uid;
            int32_t want_jk  = u.vit[vi].jk;

            /* Find anchor_score entry matching (uid, jk).
             *
             * The same (uid, jk) can be first_cand for BOTH a SYL anchor
             * and a WORD anchor (e.g. "Eight." text_023). In that case,
             * pick the as_ev whose final_n_cands matches this Viterbi
             * slot's n_cands. v7 hits this on 1/179 slots; without it the
             * scorer picks the wrong anchor type and gets the slot wrong.
             * See project_b44_anchor_gap.md. */
            int as_idx = -1;
            int n_cands_here = u.vit[vi].n_cands;
            for (int j = 0; j < u.n_as; ++j) {
                if (u.as[j].cand_uid == want_uid
                    && u.as[j].cand_jk == want_jk
                    && u.as[j].final_n_cands == n_cands_here) {
                    as_idx = j;
                    break;
                }
            }
            if (as_idx < 0) {
                /* Fall back to first match if no n_cands disambiguator hit
                 * (single-anchor case). */
                for (int j = 0; j < u.n_as; ++j) {
                    if (u.as[j].cand_uid == want_uid
                        && u.as[j].cand_jk == want_jk) {
                        as_idx = j;
                        break;
                    }
                }
            }
            if (as_idx < 0) continue;

            int32_t first_hp = u.as[as_idx].first_hp;
            int32_t last_hp  = u.as[as_idx].last_hp;
            if (first_hp < 0 || last_hp < first_hp || last_hp >= MAX_HP)
                continue;
            if (!u.ctxs[first_hp].have || !u.ctxs[last_hp].have) continue;

            /* Build per-HP arrays for [first_hp..last_hp]. */
            int n_hp = last_hp - first_hp + 1;
            spfy_anchor_cart_t *cart_arr = (spfy_anchor_cart_t *)
                calloc((size_t)n_hp, sizeof *cart_arr);
            spfy_anchor_sp_target_t *sp_arr = (spfy_anchor_sp_target_t *)
                calloc((size_t)n_hp, sizeof *sp_arr);
            int32_t *syl_arr = NULL;
            if (u.as[as_idx].n_syl > 0) {
                syl_arr = (int32_t *)calloc((size_t)n_hp, sizeof *syl_arr);
                for (int k = 0; k < n_hp; ++k) {
                    if (first_hp + k < u.as[as_idx].n_syl)
                        syl_arr[k] = (int32_t)u.as[as_idx].syl_idx[first_hp + k];
                }
            }
            for (int k = 0; k < n_hp; ++k) {
                int hp = first_hp + k;
                cart_arr[k] = u.carts[hp].cart;
                if (u.sps[hp].have) sp_arr[k] = u.sps[hp].sp;
            }

            spfy_anchor_slot_input_t in = {0};
            in.first_hp = first_hp;
            in.last_hp  = last_hp;
            for (int k = 0; k < 5; ++k) {
                in.first_ctx.ctx[k] = u.ctxs[first_hp].ctx[k];
                in.last_ctx.ctx[k]  = u.ctxs[last_hp].ctx[k];
            }
            in.anchor_type = u.as[as_idx].type;
            in.cart_per_hp = cart_arr;
            in.sp_per_hp = sp_arr;
            /* Pass syl_arr indexed [first_hp..last_hp] -- but the C API
             * expects per-HP arrays, with syl_idx_per_hp[target_idx] where
             * target_idx is into hp_seq. Here hp_seq = [first_hp..last_hp]
             * so we need syl_arr indexed 0..n_hp-1. We built it that way. */
            in.syl_idx_per_hp = syl_arr;

            /* Find cklx postings for this anchor. */
            int grp = -1;
            const uint32_t *postings = NULL;
            uint32_t n_postings = 0;
            if (getenv("SPFY_DEBUG_POSTINGS") && first_hp == 42) {
                /* Dump postings for v_er anchor (slot 59 type=2). */
                int g = SPFY_CHUNK_GROUP_SYL;
                const spfy_ckls_group_t *cg = &ct.ckls[g];
                for (uint32_t pi = 0; pi < cg->n_postings; ++pi) {
                    if (cg->span_start[pi] == (uint32_t)want_uid
                        && cg->span_end[pi] == (uint32_t)want_jk) {
                        printf("  ckls[_SYL_][%u]: (%u,%u) tok=%s\n",
                               pi, cg->span_start[pi], cg->span_end[pi],
                               cg->token_text[pi]);
                    }
                }
                const spfy_cklx_group_t *xg = &ct.cklx[g];
                const uint32_t *posts = NULL;
                uint32_t n = 0;
                spfy_cklx_lookup(xg, "v_er", &posts, &n);
                printf("  cklx[v_er]: n=%u\n", n);
                int dup_cnt = 0;
                for (uint32_t i = 0; i < n; ++i) {
                    uint32_t pid = posts[i];
                    if (pid >= cg->n_postings) continue;
                    for (uint32_t j = 0; j < i; ++j) {
                        if (posts[j] == pid) { dup_cnt++; break; }
                    }
                }
                printf("  cklx duplicates in posts list: %d\n", dup_cnt);
                int span_dup = 0;
                for (uint32_t i = 0; i < n; ++i) {
                    if (posts[i] >= cg->n_postings) continue;
                    uint32_t ss_i = cg->span_start[posts[i]];
                    uint32_t se_i = cg->span_end[posts[i]];
                    for (uint32_t j = 0; j < i; ++j) {
                        if (posts[j] >= cg->n_postings) continue;
                        if (cg->span_start[posts[j]] == ss_i
                            && cg->span_end[posts[j]] == se_i) {
                            span_dup++; break;
                        }
                    }
                }
                printf("  cklx span duplicates: %d\n", span_dup);
            }
            if (!find_cklx_postings(&ct, u.as[as_idx].type,
                                     want_uid, want_jk,
                                     &grp, &postings, &n_postings)) {
                free(cart_arr); free(sp_arr); free(syl_arr);
                continue;
            }

            /* Run anchor_score. */
            spfy_anchor_cand_t out_cands[200];
            uint32_t out_n = 0;
            if (spfy_anchor_score(&av, &in, postings, n_postings,
                                    &ct.ckls[grp],
                                    out_cands, 200, &out_n) != SPFY_OK) {
                free(cart_arr); free(sp_arr); free(syl_arr);
                continue;
            }

            /* Compare top-N (N = engine kept count) by membership. */
            int n_actual = u.vit[vi].n_cands;
            int matched_all = 1;
            for (int j = 0; j < n_actual; ++j) {
                int found = 0;
                for (uint32_t i = 0; i < out_n && (int)i < n_actual; ++i) {
                    if (out_cands[i].ss == (uint32_t)u.vit[vi].cands_uid[j]
                        && out_cands[i].se == (uint32_t)u.vit[vi].cands_jk[j]) {
                        found = 1;
                        break;
                    }
                }
                if (!found) { matched_all = 0; break; }
            }
            ++total_anchors;
            if (matched_all) ++total_match;
            else if (getenv("SPFY_DEBUG_MISMATCH")) {
                printf("MISMATCH file=%s slot=%d type=%d hp=[%d..%d] "
                       "actual=", files[fi], vi, in.anchor_type,
                       first_hp, last_hp);
                for (int j = 0; j < n_actual; ++j)
                    printf("(%d,%d) ", u.vit[vi].cands_uid[j],
                           u.vit[vi].cands_jk[j]);
                printf(" top=");
                int show = (int)out_n < n_actual + 2 ? (int)out_n : n_actual + 2;
                for (int j = 0; j < show; ++j)
                    printf("(%u,%u,%.3f) ", out_cands[j].ss,
                           out_cands[j].se, out_cands[j].pre_dp);
                printf(" surviving=%u\n", out_n);
            }

            free(cart_arr); free(sp_arr); free(syl_arr);
        }
      }   /* end per-utt loop */

        free(files[fi]);
    }
    free(files);

    printf("anchor slots: %d\n", total_anchors);
    printf("top-N match:  %d/%d  (%.2f%%)\n",
           total_match, total_anchors,
           total_anchors ? 100.0 * total_match / total_anchors : 0.0);

    spfy_anchor_hpclass_free(hpclass);
    spfy_chunk_tables_free(&ct);
    spfy_proscost_free(proscost);
    spfy_voice_maps_free(&maps);
    spfy_ccos_free(&ccos);
    spfy_vin_free(&vin);
    spfy_vcf_free(&vcf);
    return 0;
}
