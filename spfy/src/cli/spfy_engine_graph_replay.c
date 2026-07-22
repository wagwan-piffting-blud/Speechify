/* spfy_engine_graph_replay -- M3.4r Phase A validation harness.
 *
 * Reads a viterbi_dp_hook capture (per-utterance dump of the engine's
 * BuildGraph slot DAG + per-cand pre-DP costs + chosen path) and runs
 * our DAG Viterbi (spfy_viterbi_run_dag) over the engine's slot graph
 * using its `pre_dp` values as target costs and our same-rec / hash /
 * miss=1000 join formula. Compares the chosen-UID path against the
 * engine's reconstructed path.
 *
 * 100% match across the corpus validates that DP + join + cost stack
 * is bit-exact engine-faithful; the only remaining work to synthesize
 * from text alone is replicating BuildGraph + PostScoringAdj's
 * internal-slot scoring in C (Phase B).
 *
 * Usage:
 *   spfy_engine_graph_replay <voice.vin> <viterbi_dp.jsonl>
 *
 *   Single-file mode -- run on one captured trace at a time. Drive
 *   over the corpus from PowerShell.
 */

#include <spfy/spfy.h>

#include "../voice/voice.h"
#include "../voice/unit_table.h"
#include "../usel/hash.h"
#include "../usel/viterbi.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------- */
/* JSONL helpers (loose line-based extraction; matches the format our    */
/* viterbi_dp_hook.js emits exactly)                                     */
/* -------------------------------------------------------------------- */

static const char *find_lit(const char *p, const char *end, const char *lit)
{
    size_t lp = strlen(lit);
    if ((size_t)(end - p) < lp) return NULL;
    for (const char *q = p; q + lp <= end; ++q) {
        if (memcmp(q, lit, lp) == 0) return q;
    }
    return NULL;
}

static int eat_ws(const char **p, const char *end)
{
    while (*p < end && (**p == ' ' || **p == '\t' || **p == ','))
        ++(*p);
    return 0;
}

static int parse_u32_at(const char *p, const char *end, uint32_t *out,
                        const char **after)
{
    (void)end;
    char *e = NULL;
    /* strtoul handles full u32 range (up to 4294967295 on 32-bit long
     * platforms). strtol would clamp at 2^31-1 = INT32_MAX which
     * silently corrupted curve_offset_k = 4294967246 (= -50 as int32). */
    unsigned long v = strtoul(p, &e, 10);
    if (e == p) return -1;
    *out = (uint32_t)v;
    if (after) *after = e;
    return 0;
}

static int parse_f32_at(const char *p, const char *end, float *out,
                        const char **after)
{
    (void)end;
    char *e = NULL;
    double v = strtod(p, &e);
    if (e == p) return -1;
    *out = (float)v;
    if (after) *after = e;
    return 0;
}

/* Read a "key":value pair off `line` if present. Returns 0 on success. */
static int read_key_u32(const char *line, const char *end,
                        const char *key, uint32_t *out)
{
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = find_lit(line, end, needle);
    if (!p) return -1;
    p += strlen(needle);
    return parse_u32_at(p, end, out, NULL);
}

static int read_key_f32(const char *line, const char *end,
                        const char *key, float *out)
{
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = find_lit(line, end, needle);
    if (!p) return -1;
    p += strlen(needle);
    return parse_f32_at(p, end, out, NULL);
}

/* -------------------------------------------------------------------- */
/* Engine graph data structures                                          */
/* -------------------------------------------------------------------- */

typedef struct {
    uint32_t  slot_idx;
    uint32_t  slice_ptr;       /* heap address of the engine's slice obj */
    uint32_t  n_cands;
    uint32_t *cands;           /* uids, len n_cands (= cand+0xc) */
    uint32_t *join_keys;       /* len n_cands (= cand+0x10) */
    uint32_t *c68;             /* len n_cands; engine smooth-miss state */
    uint32_t *c78;             /* len n_cands */
    uint32_t *c6c;             /* len n_cands */
    float    *pre_dp;          /* engine pre-DP cost per cand */
    uint32_t *cand_ptrs;       /* heap addrs, for predec lookups in leave */
    uint32_t  n_preds;
    uint32_t *pred_slot_idx;   /* mapped from raw pred slice ptrs */
    int       has_error;
} eg_slot_t;

typedef struct {
    eg_slot_t *slots;
    uint32_t   n_slots;
    /* "leave" event derived chosen path (engine-truth) */
    int       best_slot;
    int       best_idx;
    uint32_t *chosen_path_slot;   /* len chosen_path_n */
    uint32_t *chosen_path_uid;
    uint32_t  chosen_path_n;
    /* per-cand "leave" predec ptr lookup (cand_ptr -> (slot, idx)) is
     * built as a flat flat scan when reconstructing the chain. */
    /* Raw bytes for the leave event so we can re-scan the cand objs to
     * pull predec ptrs. */
    char     *leave_buf;
    size_t    leave_n;
} eg_utt_t;

static void eg_slot_free(eg_slot_t *s)
{
    free(s->cands);     s->cands = NULL;
    free(s->join_keys); s->join_keys = NULL;
    free(s->c68);       s->c68 = NULL;
    free(s->c78);       s->c78 = NULL;
    free(s->c6c);       s->c6c = NULL;
    free(s->pre_dp);    s->pre_dp = NULL;
    free(s->cand_ptrs); s->cand_ptrs = NULL;
    free(s->pred_slot_idx); s->pred_slot_idx = NULL;
}

static void eg_utt_free(eg_utt_t *u)
{
    if (!u) return;
    for (uint32_t i = 0; i < u->n_slots; ++i) eg_slot_free(&u->slots[i]);
    free(u->slots);     u->slots = NULL;
    free(u->chosen_path_slot); u->chosen_path_slot = NULL;
    free(u->chosen_path_uid);  u->chosen_path_uid  = NULL;
    free(u->leave_buf); u->leave_buf = NULL;
}

/* Skip past matching brackets/braces. Returns pointer just past the
 * close char, or end on parse error. */
static const char *skip_to_match(const char *p, const char *end,
                                 char open, char close)
{
    if (p >= end || *p != open) return end;
    int depth = 1;
    ++p;
    while (p < end && depth > 0) {
        if (*p == open)  ++depth;
        if (*p == close) --depth;
        ++p;
    }
    return p;
}

/* Parse a single slot object string {...}. p points at '{'. */
static int parse_slot_obj(const char *p, const char *end,
                          eg_slot_t *out)
{
    if (p >= end || *p != '{') return -1;
    const char *obj_end = skip_to_match(p, end, '{', '}');

    /* slot index */
    if (read_key_u32(p, obj_end, "slot", &out->slot_idx) != 0)
        return -1;

    /* slice_ptr */
    {
        uint32_t v = 0;
        if (read_key_u32(p, obj_end, "slice_ptr", &v) == 0)
            out->slice_ptr = v;
    }

    /* error? */
    if (find_lit(p, obj_end, "\"error\":") != NULL) {
        out->has_error = 1;
    }

    /* n_cands */
    {
        uint32_t v = 0;
        if (read_key_u32(p, obj_end, "n_cands", &v) == 0) {
            out->n_cands = v;
        }
    }
    /* n_preds */
    {
        uint32_t v = 0;
        if (read_key_u32(p, obj_end, "n_preds", &v) == 0)
            out->n_preds = v;
    }

    /* preds: [a,b,c,...] -- raw slice ptrs (heap addresses) */
    if (out->n_preds > 0) {
        out->pred_slot_idx = calloc(out->n_preds, sizeof *out->pred_slot_idx);
        if (!out->pred_slot_idx) return -1;
        const char *pp = find_lit(p, obj_end, "\"preds\":[");
        if (pp) {
            pp += strlen("\"preds\":[");
            for (uint32_t k = 0; k < out->n_preds; ++k) {
                eat_ws(&pp, obj_end);
                if (pp >= obj_end || *pp == ']') break;
                uint32_t v = 0;
                if (parse_u32_at(pp, obj_end, &v, &pp) != 0) break;
                /* Store raw pred slice ptr; we'll map to slot indices
                 * after all slots have been parsed. */
                out->pred_slot_idx[k] = v;
            }
        }
    }

    /* cands: [{...}, ...] */
    if (!out->has_error && out->n_cands > 0) {
        out->cands     = calloc(out->n_cands, sizeof *out->cands);
        out->join_keys = calloc(out->n_cands, sizeof *out->join_keys);
        out->c68       = calloc(out->n_cands, sizeof *out->c68);
        out->c78       = calloc(out->n_cands, sizeof *out->c78);
        out->c6c       = calloc(out->n_cands, sizeof *out->c6c);
        out->pre_dp    = calloc(out->n_cands, sizeof *out->pre_dp);
        out->cand_ptrs = calloc(out->n_cands, sizeof *out->cand_ptrs);
        if (!out->cands || !out->join_keys || !out->c68 || !out->c78 ||
            !out->c6c || !out->pre_dp || !out->cand_ptrs) return -1;
        const char *cp = find_lit(p, obj_end, "\"cands\":[");
        if (cp) {
            cp += strlen("\"cands\":[");
            uint32_t k = 0;
            while (cp < obj_end && k < out->n_cands) {
                eat_ws(&cp, obj_end);
                if (cp >= obj_end || *cp == ']') break;
                if (*cp != '{') break;
                const char *cend = skip_to_match(cp, obj_end, '{', '}');
                uint32_t uid = 0;
                float    pdp = 0.f;
                uint32_t cptr = 0;
                uint32_t jkey = 0;
                uint32_t c68 = 0, c78 = 0, c6c = 0;
                if (read_key_u32(cp, cend, "uid", &uid) != 0) break;
                if (read_key_u32(cp, cend, "cand_ptr", &cptr) == 0)
                    out->cand_ptrs[k] = cptr;
                /* join_key (cand+0x10) -- if absent (older trace),
                 * fall back to uid (halfphone-leaf semantics). */
                if (read_key_u32(cp, cend, "join_key", &jkey) != 0)
                    jkey = uid;
                /* Smooth-miss inputs (M3.4r Phase A.5). Default 0
                 * means halfphone behavior (no smooth contribution). */
                (void)read_key_u32(cp, cend, "c68", &c68);
                (void)read_key_u32(cp, cend, "c78", &c78);
                (void)read_key_u32(cp, cend, "c6c", &c6c);
                if (read_key_f32(cp, cend, "pre_dp", &pdp) != 0) break;
                out->cands[k]     = uid;
                out->join_keys[k] = jkey;
                out->c68[k]       = c68;
                out->c78[k]       = c78;
                out->c6c[k]       = c6c;
                out->pre_dp[k]    = pdp;
                ++k;
                cp = cend;
            }
            /* If we didn't fill all cands (e.g. parse hiccup), shrink. */
            if (k < out->n_cands) out->n_cands = k;
        }
    }
    return 0;
}

/* Parse a "viterbi_enter" line into slots[]. Returns 0 on success. */
static int parse_enter_line(const char *line, size_t n,
                            eg_utt_t *u)
{
    const char *end = line + n;
    if (!find_lit(line, end, "\"type\":\"viterbi_enter\"")) return -1;
    uint32_t n_slots = 0;
    if (read_key_u32(line, end, "n_slots", &n_slots) != 0) return -1;
    u->n_slots = n_slots;
    u->slots = calloc(n_slots ? n_slots : 1, sizeof *u->slots);
    if (!u->slots) return -1;

    const char *p = find_lit(line, end, "\"slots\":[");
    if (!p) return -1;
    p += strlen("\"slots\":[");

    uint32_t k = 0;
    while (p < end && k < n_slots) {
        eat_ws(&p, end);
        if (p >= end || *p == ']') break;
        if (*p != '{') break;
        const char *next_p = skip_to_match(p, end, '{', '}');
        if (parse_slot_obj(p, next_p, &u->slots[k]) != 0) {
            /* leave as-is, keep going */
        }
        ++k;
        p = next_p;
    }
    /* Sanity: slot indices should be 0..n-1 in array order. */
    for (uint32_t i = 0; i < u->n_slots; ++i) {
        if (u->slots[i].slot_idx != i) {
            fprintf(stderr, "warn: slot index gap at array pos %u "
                    "(slot_idx=%u)\n", i, u->slots[i].slot_idx);
        }
    }
    /* Map raw pred slice ptrs to slot indices via slice_ptr lookup. */
    for (uint32_t i = 0; i < u->n_slots; ++i) {
        eg_slot_t *s = &u->slots[i];
        for (uint32_t j = 0; j < s->n_preds; ++j) {
            uint32_t raw_ptr = s->pred_slot_idx[j];
            uint32_t mapped  = UINT32_MAX;
            for (uint32_t z = 0; z < u->n_slots; ++z) {
                if (u->slots[z].slice_ptr == raw_ptr) {
                    mapped = z; break;
                }
            }
            s->pred_slot_idx[j] = mapped;
        }
    }
    return 0;
}

/* Parse a "viterbi_leave" line: stash raw bytes, find best_slot/idx,
 * and reconstruct chosen path via predec walk. */
static int parse_leave_line(const char *line, size_t n, eg_utt_t *u)
{
    const char *end = line + n;
    if (!find_lit(line, end, "\"type\":\"viterbi_leave\"")) return -1;
    uint32_t v = 0;
    u->best_slot = -1;
    u->best_idx  = -1;
    if (read_key_u32(line, end, "best_slot", &v) == 0) u->best_slot = (int)v;
    if (read_key_u32(line, end, "best_idx",  &v) == 0) u->best_idx  = (int)v;

    /* Stash the leave buffer so we can re-scan it for predec walking. */
    u->leave_buf = (char *)malloc(n + 1);
    if (!u->leave_buf) return -1;
    memcpy(u->leave_buf, line, n);
    u->leave_buf[n] = 0;
    u->leave_n = n;
    return 0;
}

/* Walk one cand object in the leave buffer to find its (uid, predec).
 * Returns 0 on success.  `cand_ptr_target` selects which cand. */
static int leave_find_cand(const char *buf, size_t n, uint32_t cand_ptr_target,
                           uint32_t *out_slot, uint32_t *out_idx,
                           uint32_t *out_uid, uint32_t *out_predec)
{
    /* Scan all cand objects ({"i":...,"cand_ptr":CPP,"uid":U,...,"predec":P,...}).
     * Track the most-recent slot index we've seen: line is structured
     * as slots[].cands[]. */
    const char *p = buf;
    const char *end = buf + n;
    int      cur_slot = -1;
    while (p < end) {
        const char *slot_p = find_lit(p, end, "\"slot\":");
        const char *cand_p = find_lit(p, end, "\"cand_ptr\":");
        if (!slot_p && !cand_p) break;
        const char *next = NULL;
        if (slot_p && (!cand_p || slot_p < cand_p)) {
            next = slot_p + strlen("\"slot\":");
            uint32_t s = 0;
            if (parse_u32_at(next, end, &s, &next) == 0) cur_slot = (int)s;
            p = next;
            continue;
        }
        next = cand_p + strlen("\"cand_ptr\":");
        uint32_t cptr = 0;
        if (parse_u32_at(next, end, &cptr, &next) != 0) {
            p = next;
            continue;
        }
        if (cptr == cand_ptr_target) {
            uint32_t uid = 0, predec = 0;
            const char *uidp = find_lit(next, end, "\"uid\":");
            const char *prdp = find_lit(next, end, "\"predec\":");
            if (uidp) {
                uidp += strlen("\"uid\":");
                parse_u32_at(uidp, end, &uid, NULL);
            }
            if (prdp) {
                prdp += strlen("\"predec\":");
                parse_u32_at(prdp, end, &predec, NULL);
            }
            *out_slot   = (uint32_t)(cur_slot < 0 ? 0 : cur_slot);
            *out_idx    = 0;
            *out_uid    = uid;
            *out_predec = predec;
            return 0;
        }
        p = next;
    }
    return -1;
}

/* Reconstruct the engine chosen path by following cand+0x24 (predec)
 * pointers from (best_slot, best_idx) backwards. */
static int reconstruct_engine_path(eg_utt_t *u)
{
    if (u->best_slot < 0 || u->best_idx < 0) return -1;
    if ((uint32_t)u->best_slot >= u->n_slots) return -1;
    eg_slot_t *bs = &u->slots[u->best_slot];
    if ((uint32_t)u->best_idx >= bs->n_cands) return -1;

    /* Capacity: at most n_slots hops. */
    u->chosen_path_slot = calloc(u->n_slots, sizeof *u->chosen_path_slot);
    u->chosen_path_uid  = calloc(u->n_slots, sizeof *u->chosen_path_uid);
    if (!u->chosen_path_slot || !u->chosen_path_uid) return -1;

    /* Start cand_ptr from the enter's best_slot/idx (we have cand_ptrs
     * stored). */
    uint32_t cand_ptr = bs->cand_ptrs[u->best_idx];

    uint32_t hops = 0;
    while (hops < u->n_slots) {
        uint32_t s = 0, i = 0, uid = 0, predec = 0;
        if (leave_find_cand(u->leave_buf, u->leave_n,
                            cand_ptr, &s, &i, &uid, &predec) != 0)
            break;
        u->chosen_path_slot[hops] = s;
        u->chosen_path_uid[hops]  = uid;
        ++hops;
        if (!predec) break;
        cand_ptr = predec;
    }
    /* Reverse */
    for (uint32_t i = 0; i < hops / 2; ++i) {
        uint32_t a = u->chosen_path_slot[i];
        uint32_t b = u->chosen_path_uid[i];
        u->chosen_path_slot[i] = u->chosen_path_slot[hops - 1 - i];
        u->chosen_path_uid[i]  = u->chosen_path_uid[hops - 1 - i];
        u->chosen_path_slot[hops - 1 - i] = a;
        u->chosen_path_uid[hops - 1 - i]  = b;
    }
    u->chosen_path_n = hops;
    return 0;
}

/* -------------------------------------------------------------------- */
/* Gate curve loader (from viterbi_consts trace)                         */
/* -------------------------------------------------------------------- */

typedef struct {
    int       has;
    uint32_t  curve_n;          /* engine: 100 */
    int32_t   offset_k;         /* engine: -50 */
    float    *curve;            /* heap, len curve_n */
    float     gate_weight;      /* engine: 0.6 */
    float     miss_offset;      /* engine: 1000 */
    int       weight_94;        /* engine for Tom: 1 */
} gate_curve_t;

static int load_gate_curve(const char *path, gate_curve_t *out)
{
    memset(out, 0, sizeof *out);
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    char *buf = malloc(1 << 20);    /* 1 MiB scratch -- the line is big */
    if (!buf) { fclose(fp); return -1; }
    int rc = -1;
    while (fgets(buf, 1 << 20, fp)) {
        size_t n = strlen(buf);
        const char *end = buf + n;
        if (!find_lit(buf, end, "\"type\":\"viterbi_consts\"")) continue;
        uint32_t v = 0;
        float fv = 0;
        if (read_key_u32(buf, end, "curve_n",        &v)  == 0) out->curve_n = v;
        if (read_key_u32(buf, end, "curve_offset_k", &v)  == 0)
            out->offset_k = (int32_t)v;
        if (read_key_f32(buf, end, "gate_weight",    &fv) == 0) out->gate_weight = fv;
        if (read_key_f32(buf, end, "miss_offset",    &fv) == 0) out->miss_offset = fv;
        out->weight_94 = 1;       /* Tom: weight+0x94 = 1 (per probe) */

        const char *cp = find_lit(buf, end, "\"curve_data\":[");
        if (cp && out->curve_n > 0) {
            cp += strlen("\"curve_data\":[");
            out->curve = calloc(out->curve_n, sizeof *out->curve);
            if (!out->curve) break;
            for (uint32_t i = 0; i < out->curve_n; ++i) {
                while (cp < end && (*cp == ' ' || *cp == ','))
                    ++cp;
                if (cp >= end || *cp == ']') break;
                float val = 0;
                if (parse_f32_at(cp, end, &val, &cp) != 0) break;
                out->curve[i] = val;
            }
            out->has = 1;
            rc = 0;
        }
        break;
    }
    free(buf);
    fclose(fp);
    return rc;
}

static void gate_curve_free(gate_curve_t *g)
{
    if (!g) return;
    free(g->curve); g->curve = NULL;
    memset(g, 0, sizeof *g);
}

/* -------------------------------------------------------------------- */
/* Per-utterance: run our DAG DP, compare to engine                      */
/* -------------------------------------------------------------------- */

typedef struct {
    int      utt_run;
    int      utt_match;
    int      slot_total;
    int      slot_match;
} eg_stats_t;

static void replay_utt(const char *entry_id, int utt_idx,
                       eg_utt_t *u,
                       const spfy_hash_t *hash,
                       const spfy_unit_table_t *units,
                       const gate_curve_t *gate,
                       eg_stats_t *st,
                       int verbose)
{
    /* Engine DAG DP with smooth-miss state tracking. We run this
     * inline rather than via spfy_viterbi_run_dag because the engine's
     * miss formula depends on per-cand DP-state fields (cand+0x7c,
     * cand+0x80) that have to be updated as we pick best preds. */
    uint32_t n = u->n_slots;
    if (n == 0) return;
    /* Per-cand DP state: long double fwd, predec slot/idx, valid,
     * c7c_state, c80_state. Use flat per-slot allocations indexed by
     * slot-and-cand. */
    long double **fwd       = calloc(n, sizeof *fwd);
    uint32_t    **predslot  = calloc(n, sizeof *predslot);
    uint32_t    **predcidx  = calloc(n, sizeof *predcidx);
    uint8_t     **valid     = calloc(n, sizeof *valid);
    uint32_t    **c7c_state = calloc(n, sizeof *c7c_state);
    uint32_t    **c80_state = calloc(n, sizeof *c80_state);
    if (!fwd || !predslot || !predcidx || !valid ||
        !c7c_state || !c80_state) goto cleanup_arrays;
    for (uint32_t s = 0; s < n; ++s) {
        uint32_t nc = u->slots[s].n_cands;
        if (nc == 0) continue;
        fwd[s]       = calloc(nc, sizeof **fwd);
        predslot[s]  = calloc(nc, sizeof **predslot);
        predcidx[s]  = calloc(nc, sizeof **predcidx);
        valid[s]     = calloc(nc, sizeof **valid);
        c7c_state[s] = calloc(nc, sizeof **c7c_state);
        c80_state[s] = calloc(nc, sizeof **c80_state);
        if (!fwd[s] || !predslot[s] || !predcidx[s] || !valid[s] ||
            !c7c_state[s] || !c80_state[s]) goto cleanup_arrays;
    }

    /* Forward DP. */
    for (uint32_t s = 0; s < n; ++s) {
        eg_slot_t *sc = &u->slots[s];
        uint32_t nc = sc->n_cands;
        if (nc == 0) continue;

        /* Source slot? (no live predecessors) */
        int has_live_pred = 0;
        for (uint32_t pi = 0; pi < sc->n_preds; ++pi) {
            uint32_t p = sc->pred_slot_idx[pi];
            if (p >= s) continue;
            if (u->slots[p].n_cands > 0) { has_live_pred = 1; break; }
        }
        if (!has_live_pred) {
            for (uint32_t c = 0; c < nc; ++c) {
                fwd[s][c]   = (long double)sc->pre_dp[c];
                predslot[s][c] = s;
                predcidx[s][c] = 0;
                valid[s][c] = 1;
                /* Init c7c/c80: engine's slot-0 init sets both 0. */
                if (sc->c68[c] < 0x15u) {
                    c7c_state[s][c] = 0;        /* no live pred */
                    c80_state[s][c] = (gate && gate->weight_94)
                                      ? 100u : 1u;
                } else {
                    c7c_state[s][c] = sc->c68[c];
                    c80_state[s][c] = (gate && gate->weight_94)
                                      ? 0u : sc->c78[c];
                }
            }
            continue;
        }

        for (uint32_t c = 0; c < nc; ++c) {
            uint32_t cuid = sc->cands[c];
            uint32_t curr_c6c = sc->c6c[c];
            long double best  = 0.0L;
            uint32_t bestp_s  = 0;
            uint32_t bestp_c  = 0;
            int      have     = 0;
            float    best_pre_dp = sc->pre_dp[c];
            (void)best_pre_dp;

            for (uint32_t pi = 0; pi < sc->n_preds; ++pi) {
                uint32_t p = sc->pred_slot_idx[pi];
                if (p >= s) continue;
                eg_slot_t *sp = &u->slots[p];
                if (sp->n_cands == 0) continue;
                for (uint32_t cp = 0; cp < sp->n_cands; ++cp) {
                    if (!valid[p][cp]) continue;
                    uint32_t pkey = sp->join_keys[cp];

                    /* Engine same-rec adjacency: J = 0 if curr_uid =
                     * pkey + 1 AND curr.flag_b != 0. */
                    int same_rec = 0;
                    if (cuid == pkey + 1u && cuid > 0u) {
                        spfy_unit_record_t r;
                        if (spfy_unit_record_get(units, cuid, &r)
                            == SPFY_OK && r.flag_b)
                            same_rec = 1;
                    }
                    float j;
                    if (same_rec) {
                        j = 0.0f;
                    } else {
                        float hit_cost = 0;
                        int hr = spfy_hash_lookup(hash, pkey, cuid,
                                                  &hit_cost);
                        if (hr == SPFY_OK) {
                            j = hit_cost;
                        } else {
                            float smooth = 0.0f;
                            uint32_t pred_c80 = c80_state[p][cp];
                            uint32_t pred_c7c = c7c_state[p][cp];
                            if (gate && gate->has &&
                                curr_c6c > 0x14u &&
                                pred_c80 < 0x0fu &&
                                pred_c7c > 0x14u) {
                                int64_t i64 = (int64_t)curr_c6c
                                            - (int64_t)gate->offset_k
                                            - (int64_t)pred_c7c;
                                if (i64 < 0) i64 = 0;
                                if (i64 >= (int64_t)gate->curve_n)
                                    i64 = (int64_t)gate->curve_n - 1;
                                smooth = gate->gate_weight *
                                         gate->curve[i64];
                            }
                            j = (gate ? gate->miss_offset : 1000.0f)
                                + smooth;
                        }
                    }

                    long double cand = fwd[p][cp] + (long double)j;
                    if (!have || cand < best) {
                        best    = cand;
                        bestp_s = p;
                        bestp_c = cp;
                        have    = 1;
                    }
                }
            }
            if (!have) { valid[s][c] = 0; continue; }
            fwd[s][c]      = best + (long double)sc->pre_dp[c];
            predslot[s][c] = bestp_s;
            predcidx[s][c] = bestp_c;
            valid[s][c]    = 1;
            /* Update c7c/c80 state for this cand based on chosen pred. */
            if (sc->c68[c] < 0x15u) {
                c7c_state[s][c] = c7c_state[bestp_s][bestp_c];
                c80_state[s][c] = (gate && gate->weight_94)
                                  ? 100u
                                  : (c80_state[bestp_s][bestp_c] + 1u);
            } else {
                c7c_state[s][c] = sc->c68[c];
                c80_state[s][c] = (gate && gate->weight_94)
                                  ? 0u : sc->c78[c];
            }
        }
    }

    /* Pick best end at last non-empty slot. */
    int32_t last = -1;
    for (int32_t s = (int32_t)n - 1; s >= 0; --s) {
        if (u->slots[s].n_cands == 0) continue;
        for (uint32_t c = 0; c < u->slots[s].n_cands; ++c) {
            if (valid[s][c]) { last = s; break; }
        }
        if (last >= 0) break;
    }
    if (last < 0) goto cleanup_arrays;
    long double end_best = 0.0L;
    uint32_t    end_idx  = 0;
    int         end_have = 0;
    for (uint32_t c = 0; c < u->slots[last].n_cands; ++c) {
        if (!valid[last][c]) continue;
        long double v = fwd[last][c];
        if (!end_have || v < end_best) {
            end_best = v;
            end_idx  = c;
            end_have = 1;
        }
    }
    if (!end_have) goto cleanup_arrays;

    /* Backtrack. */
    uint32_t *out_path_slot = calloc(n, sizeof *out_path_slot);
    uint32_t *out_path_uid  = calloc(n, sizeof *out_path_uid);
    if (!out_path_slot || !out_path_uid) {
        free(out_path_slot); free(out_path_uid);
        goto cleanup_arrays;
    }
    uint32_t *tmp_s = malloc(n * sizeof *tmp_s);
    uint32_t *tmp_u = malloc(n * sizeof *tmp_u);
    uint32_t hops = 0;
    {
        uint32_t cur_s = (uint32_t)last;
        uint32_t cur_c = end_idx;
        for (;;) {
            tmp_s[hops] = cur_s;
            tmp_u[hops] = u->slots[cur_s].cands[cur_c];
            ++hops;
            if (hops >= n) break;
            uint32_t ps = predslot[cur_s][cur_c];
            uint32_t pc = predcidx[cur_s][cur_c];
            if (ps == cur_s) break;
            cur_s = ps; cur_c = pc;
        }
    }
    for (uint32_t i = 0; i < hops; ++i) {
        out_path_slot[i] = tmp_s[hops - 1 - i];
        out_path_uid [i] = tmp_u[hops - 1 - i];
    }
    uint32_t out_len = hops;
    free(tmp_s); free(tmp_u);

    st->utt_run++;

    /* Compare to engine chosen path. */
    int slot_total = 0, slot_match = 0, full_match = 1;
    /* Build slot->uid map for engine. */
    uint32_t *eng_by_slot = calloc(u->n_slots, sizeof *eng_by_slot);
    uint8_t  *eng_seen    = calloc(u->n_slots, sizeof *eng_seen);
    for (uint32_t i = 0; i < u->chosen_path_n; ++i) {
        uint32_t s = u->chosen_path_slot[i];
        if (s < u->n_slots) {
            eng_by_slot[s] = u->chosen_path_uid[i];
            eng_seen[s] = 1;
        }
    }
    /* Build slot->uid map for ours. */
    uint32_t *our_by_slot = calloc(u->n_slots, sizeof *our_by_slot);
    uint8_t  *our_seen    = calloc(u->n_slots, sizeof *our_seen);
    for (uint32_t i = 0; i < out_len; ++i) {
        uint32_t s = out_path_slot[i];
        if (s < u->n_slots) {
            our_by_slot[s] = out_path_uid[i];
            our_seen[s] = 1;
        }
    }
    for (uint32_t s = 0; s < u->n_slots; ++s) {
        if (!eng_seen[s] && !our_seen[s]) continue;
        slot_total++;
        if (eng_seen[s] && our_seen[s] && eng_by_slot[s] == our_by_slot[s]) {
            slot_match++;
        } else {
            full_match = 0;
        }
    }
    if (full_match) st->utt_match++;
    st->slot_total += slot_total;
    st->slot_match += slot_match;

    printf("  %-12s utt %d: engine_path_len=%u  our_path_len=%u  "
           "slots %d/%d  %s\n",
           entry_id, utt_idx, u->chosen_path_n, out_len,
           slot_match, slot_total,
           full_match ? "MATCH" : "DIFF");

    if (!full_match && verbose) {
        printf("    diff slots:");
        for (uint32_t s = 0; s < u->n_slots; ++s) {
            if (eng_seen[s] && our_seen[s] && eng_by_slot[s] != our_by_slot[s]) {
                printf("  %u(eng=%u/ours=%u)",
                       s, eng_by_slot[s], our_by_slot[s]);
            } else if (eng_seen[s] && !our_seen[s]) {
                printf("  %u(eng=%u/--)", s, eng_by_slot[s]);
            } else if (!eng_seen[s] && our_seen[s]) {
                printf("  %u(--/ours=%u)", s, our_by_slot[s]);
            }
        }
        printf("\n");
    }

    free(eng_by_slot); free(eng_seen);
    free(our_by_slot); free(our_seen);
    free(out_path_slot); free(out_path_uid);
cleanup_arrays:
    if (fwd)       { for (uint32_t s=0; s<n; ++s) free(fwd[s]);      free(fwd);       }
    if (predslot)  { for (uint32_t s=0; s<n; ++s) free(predslot[s]); free(predslot);  }
    if (predcidx)  { for (uint32_t s=0; s<n; ++s) free(predcidx[s]); free(predcidx);  }
    if (valid)     { for (uint32_t s=0; s<n; ++s) free(valid[s]);    free(valid);     }
    if (c7c_state) { for (uint32_t s=0; s<n; ++s) free(c7c_state[s]); free(c7c_state); }
    if (c80_state) { for (uint32_t s=0; s<n; ++s) free(c80_state[s]); free(c80_state); }
}

/* -------------------------------------------------------------------- */
/* Main                                                                  */
/* -------------------------------------------------------------------- */

static int process_file(const char *vit_path,
                        const spfy_hash_t *hash,
                        const spfy_unit_table_t *units,
                        const gate_curve_t *gate,
                        eg_stats_t *st,
                        int verbose)
{
    FILE *fp = fopen(vit_path, "rb");
    if (!fp) {
        fprintf(stderr, "could not open %s\n", vit_path);
        return -1;
    }
    /* Extract entry_id = basename without extension. */
    const char *base = vit_path;
    for (const char *q = vit_path; *q; ++q)
        if (*q == '/' || *q == '\\') base = q + 1;
    char entry_id[128] = {0};
    snprintf(entry_id, sizeof entry_id, "%s", base);
    char *dot = strrchr(entry_id, '.');
    if (dot) *dot = 0;

    char buf[1 << 18];   /* 256k per line should be enough */
    int utt_idx = 0;
    eg_utt_t pending = {0};
    int have_enter = 0;
    while (fgets(buf, sizeof buf, fp)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
            buf[--n] = 0;
        if (n == 0) continue;
        if (find_lit(buf, buf + n, "\"type\":\"viterbi_enter\"")) {
            if (have_enter) eg_utt_free(&pending);
            memset(&pending, 0, sizeof pending);
            if (parse_enter_line(buf, n, &pending) == 0) have_enter = 1;
        } else if (find_lit(buf, buf + n, "\"type\":\"viterbi_leave\"")
                   && have_enter) {
            parse_leave_line(buf, n, &pending);
            reconstruct_engine_path(&pending);
            ++utt_idx;
            replay_utt(entry_id, utt_idx, &pending, hash, units,
                       gate, st, verbose);
            eg_utt_free(&pending);
            memset(&pending, 0, sizeof pending);
            have_enter = 0;
        }
    }
    if (have_enter) eg_utt_free(&pending);
    fclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: spfy_engine_graph_replay <voice.vin> "
                "<viterbi_dp.jsonl> [<viterbi_dp.jsonl> ...] "
                "[--verbose]\n");
        return 2;
    }
    int verbose = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
    }
    const char *vin_path = argv[1];

    spfy_vin_t vin;
    int rc = spfy_vin_load(vin_path, &vin);
    if (rc != SPFY_OK) {
        fprintf(stderr, "vin open failed: %s\n", spfy_strerror(rc));
        return 1;
    }
    spfy_hash_t hash = {0};
    rc = spfy_hash_load(&vin, &hash);
    if (rc != SPFY_OK) {
        fprintf(stderr, "hash load failed: %s\n", spfy_strerror(rc));
        spfy_vin_free(&vin);
        return 1;
    }
    spfy_unit_table_t units = {0};
    rc = spfy_unit_table_load(&vin, &units);
    if (rc != SPFY_OK) {
        fprintf(stderr, "unit table load failed: %s\n", spfy_strerror(rc));
        spfy_hash_free(&hash);
        spfy_vin_free(&vin);
        return 1;
    }

    /* SPFY_EG_HASH_SCAN="prev_lo:prev_hi:curr" -- print hash result
     * for (prev, curr) for all prev in [prev_lo..prev_hi]. */
    const char *scan = getenv("SPFY_EG_HASH_SCAN");
    if (scan) {
        uint32_t lo = 0, hi = 0, cc = 0;
        if (sscanf(scan, "%u:%u:%u", &lo, &hi, &cc) == 3) {
            uint32_t row_off = (cc < hash.n_rows) ? spfy_hash_row(&hash, cc) : 0;
            fprintf(stderr,
                    "scan curr=%u rows[%u]=%u\n", cc, cc, row_off);
            for (uint32_t p = lo; p <= hi; ++p) {
                float c = 0;
                int hr = spfy_hash_lookup(&hash, p, cc, &c);
                uint64_t idx = (uint64_t)row_off + p;
                uint32_t cellA = (idx < hash.n_cells)
                                 ? spfy_hash_cell_a(&hash, idx) : 0xFFFFFFFFu;
                fprintf(stderr,
                        "  prev=%u idx=%llu cellA=%u rc=%d cost=%g %s\n",
                        p, (unsigned long long)idx, cellA, hr, (double)c,
                        hr == SPFY_OK ? "HIT" : "MISS");
            }
        }
        spfy_hash_free(&hash);
        spfy_vin_free(&vin);
        return 0;
    }

    /* Try to load gate curve from sibling viterbi_consts trace.
     * Falls back to no-smooth (plain miss=1000) if not available. */
    gate_curve_t gate = {0};
    {
        char gate_path[1024];
        /* Find any vit-dp file we'll process and derive sibling. */
        for (int i = 2; i < argc; ++i) {
            if (argv[i][0] == '-' && argv[i][1] == '-') continue;
            const char *p = argv[i];
            /* Replace "viterbi_dp/<name>.jsonl" with
             * "viterbi_consts/text_001.jsonl" sibling. */
            const char *vd = strstr(p, "viterbi_dp");
            if (!vd) continue;
            size_t pre_n = (size_t)(vd - p);
            if (pre_n + strlen("viterbi_consts/text_001.jsonl") + 1
                >= sizeof gate_path) continue;
            memcpy(gate_path, p, pre_n);
            snprintf(gate_path + pre_n, sizeof gate_path - pre_n,
                     "viterbi_consts/text_001.jsonl");
            if (load_gate_curve(gate_path, &gate) == 0) {
                fprintf(stderr,
                        "loaded gate curve from %s "
                        "(n=%u, miss_offset=%g, gate_weight=%g, "
                        "offset_k=%d)\n",
                        gate_path, gate.curve_n,
                        (double)gate.miss_offset,
                        (double)gate.gate_weight, gate.offset_k);
            } else {
                fprintf(stderr, "warn: no gate curve at %s "
                        "(smooth miss disabled)\n", gate_path);
            }
            break;
        }
    }

    eg_stats_t st = {0};
    for (int i = 2; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] == '-') continue;
        process_file(argv[i], &hash, &units, &gate, &st, verbose);
    }
    printf("\n----- aggregate -----\n");
    printf("utterances: %d / %d  (%.2f%%)\n",
           st.utt_match, st.utt_run,
           100.0 * st.utt_match / (st.utt_run > 0 ? st.utt_run : 1));
    printf("slot-level: %d / %d  (%.2f%%)\n",
           st.slot_match, st.slot_total,
           100.0 * st.slot_match / (st.slot_total > 0 ? st.slot_total : 1));

    /* unit_table aliases into VIN; no separate free. */
    (void)units;
    gate_curve_free(&gate);
    spfy_hash_free(&hash);
    spfy_vin_free(&vin);
    return 0;
}
