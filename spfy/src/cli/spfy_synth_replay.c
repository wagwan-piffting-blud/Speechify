/* spfy_synth_replay -- end-to-end synth using FE traces (no FE port required).
 *
 * Pipeline:
 *   FE traces (prsl_slot, cart_walks, inner_scorer) ->
 *   per-slot cand pool ->
 *   per-cand cost via spfy_hp_innerscorer (engine-truth, 99% bit-exact) ->
 *   Viterbi DP with hash-based join cost ->
 *   chosen UIDs ->
 *   concat audio (same-rec / cross-rec pair detection from spfy_concat) ->
 *   WAV.
 *
 * Targets utt 0 of one text_id at a time. wsola_buffer trace (oracle) is
 * loaded only for the match% comparison; not required for synthesis.
 *
 *   spfy_synth_replay <voice.vin> <voice.vdb> <voice.vcf>
 *                     <hpclass.bin> <traces_dir> <text_id> <out.wav>
 */

#include <spfy/spfy.h>

#include "../voice/voice.h"
#include "../voice/unit_table.h"
#include "../voice/feat_table.h"
#include "../voice/vdb_lookup.h"
#include "../voice/ccos.h"
#include "../voice/voice_runtime.h"
#include "../voice/vcf_matrix.h"
#include "../voice/chunk_table.h"
#include "../usel/anchor_score.h"
#include "../usel/hash.h"
#include "../usel/viterbi.h"
#include "../wsola/ulaw.h"
#include "../wsola/wav.h"
#include "../wsola/wsola.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SILENCE_SENTINEL_UID 169578u
#define MAX_SLOTS            256
#define MAX_CANDS_PER_SLOT   512

/* ------------------------------------------------------------------ */
/* Tiny JSONL parsers (same shape as spfy_hp_score_test)               */
/* ------------------------------------------------------------------ */

static const char *find_lit(const char *p, const char *end, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(end - p) < n) return NULL;
    for (const char *q = p; q + n <= end; ++q)
        if (memcmp(q, lit, n) == 0) return q;
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

static int read_file(const char *path, char **buf, size_t *n)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)sz + 1);
    if (!b) { fclose(fp); return -1; }
    if (fread(b, 1, (size_t)sz, fp) != (size_t)sz) {
        free(b); fclose(fp); return -1;
    }
    b[sz] = 0;
    fclose(fp);
    *buf = b; *n = (size_t)sz;
    return 0;
}

static int next_line(const char **p, const char *end,
                     const char **ls, const char **le)
{
    const char *s = *p;
    while (s < end && (*s == '\n' || *s == '\r')) ++s;
    if (s >= end) return 0;
    *ls = s;
    while (s < end && *s != '\n') ++s;
    *le = s;
    *p = s;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Per-slot inputs assembled from the captured traces (utt 0)          */
/* ------------------------------------------------------------------ */

typedef struct {
    int      have;
    spfy_anchor_ctx_t       ctx;
    spfy_anchor_sp_target_t sp;
    spfy_anchor_cart_t      cart;
    uint32_t                cands[MAX_CANDS_PER_SLOT];
    uint32_t                n_cands;
} slot_input_t;

/* Parse prsl_slot/<text>.jsonl utt 0 into per-slot ctx + cands.
 * Stops at the start of utt 1 (slot=0 reappearing). */
static int load_prsl_slot_utt0(const char *path, slot_input_t *slots,
                               uint32_t *n_slots_out)
{
    char *buf = NULL; size_t buf_n = 0;
    if (read_file(path, &buf, &buf_n) != 0) {
        fprintf(stderr, "cannot read prsl_slot %s\n", path);
        return -1;
    }
    const char *p = buf, *end = buf + buf_n;
    const char *ls, *le;
    int seen0 = 0;
    uint32_t max_slot = 0;
    while (next_line(&p, end, &ls, &le)) {
        if (!find_lit(ls, le, "\"type\":\"prsl_slot\"")) continue;
        int64_t slot;
        if (read_key_i64(ls, le, "slot", &slot) != 0) continue;
        if (slot < 0 || slot >= MAX_SLOTS) continue;
        if (slot == 0) {
            if (seen0) break;            /* utt 1 starts */
            seen0 = 1;
        }
        int64_t arr[8];
        int n = read_key_int_array(ls, le, "ctx", arr, 8);
        if (n != 5) continue;
        slot_input_t *sl = &slots[slot];
        for (int i = 0; i < 5; ++i) sl->ctx.ctx[i] = (int32_t)arr[i];
        sl->have = 1;
        if ((uint32_t)slot > max_slot) max_slot = (uint32_t)slot;

        /* Cands. */
        const char *up = find_lit(ls, le, "\"uids\":[");
        if (up) {
            up += strlen("\"uids\":[");
            uint32_t k = 0;
            while (up < le && k < MAX_CANDS_PER_SLOT) {
                while (up < le && (*up == ' ' || *up == ',')) ++up;
                if (up >= le || *up == ']') break;
                char *ep = NULL;
                long long v = strtoll(up, &ep, 10);
                if (ep == up) break;
                sl->cands[k++] = (uint32_t)v;
                up = ep;
            }
            sl->n_cands = k;
        }
    }
    *n_slots_out = max_slot + 1;
    free(buf);
    return 0;
}

/* Parse cart_walks/<text>.jsonl utt 0: per-slot durt + f0tr leaves.
 * Same utt-boundary detection (slot-drop) as spfy_hp_score_test. */
static int load_cart_walks_utt0(const char *path, slot_input_t *slots)
{
    char *buf = NULL; size_t buf_n = 0;
    if (read_file(path, &buf, &buf_n) != 0) return -1;
    const char *p = buf, *end = buf + buf_n;
    const char *ls, *le;
    int64_t max_slot = -1;
    int phase = 0;       /* 0=durt, 1=f0tr */
    while (next_line(&p, end, &ls, &le)) {
        if (!find_lit(ls, le, "\"type\":\"cart_walk\"")) continue;
        int64_t slot;
        if (read_key_i64(ls, le, "slot", &slot) != 0) continue;
        int is_durt = find_lit(ls, le, "\"tree\":\"durt\"") != NULL;
        int is_f0tr = find_lit(ls, le, "\"tree\":\"f0tr\"") != NULL;
        if (is_durt && phase == 1 && slot == 0) break;
        if (is_f0tr) phase = 1;
        else if (is_durt && phase == 0 && slot < max_slot) break;
        if (slot > max_slot) max_slot = slot;
        if (slot < 0 || slot >= MAX_SLOTS) continue;
        double m, v;
        if (read_key_f64(ls, le, "leaf_mean", &m) != 0) continue;
        if (read_key_f64(ls, le, "leaf_var", &v) != 0) continue;
        spfy_anchor_cart_t *c = &slots[slot].cart;
        if (is_durt && !c->durt_valid) {
            c->durt_mean = (float)m; c->durt_var = (float)v;
            c->durt_valid = 1;
        } else if (is_f0tr && !c->f0tr_valid) {
            c->f0tr_mean = (float)m; c->f0tr_var = (float)v;
            c->f0tr_valid = 1;
        }
    }
    free(buf);
    return 0;
}

/* Parse inner_scorer/<text>.jsonl utt 0: per-slot sp_target. */
static int load_inner_scorer_sp_utt0(const char *path, slot_input_t *slots)
{
    char *buf = NULL; size_t buf_n = 0;
    if (read_file(path, &buf, &buf_n) != 0) return -1;
    const char *p = buf, *end = buf + buf_n;
    const char *ls, *le;
    int seen0 = 0;
    while (next_line(&p, end, &ls, &le)) {
        if (!find_lit(ls, le, "\"type\":\"inner_scorer\"")) continue;
        int64_t slot;
        if (read_key_i64(ls, le, "slot", &slot) != 0) continue;
        if (slot < 0 || slot >= MAX_SLOTS) continue;
        if (slot == 0) {
            if (seen0) break;
            seen0 = 1;
        }
        int64_t sp[8];
        int n = read_key_int_array(ls, le, "sp_target", sp, 8);
        if (n != 5) continue;
        spfy_anchor_sp_target_t *t = &slots[slot].sp;
        for (int i = 0; i < 5; ++i) t->sp[i] = (uint32_t)sp[i];
    }
    free(buf);
    return 0;
}

/* Read voicing[] from inner_scorer/text_001.jsonl join_consts (singleton).
 * Voicing is voice-wide; only emitted on the first hook fire of the session. */
static int load_voicing(const char *traces_dir, uint32_t *voicing,
                        uint32_t *n_out)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/inner_scorer/text_001.jsonl", traces_dir);
    char *buf = NULL; size_t buf_n = 0;
    if (read_file(path, &buf, &buf_n) != 0) return -1;
    const char *p = buf, *end = buf + buf_n;
    const char *ls, *le;
    uint32_t k = 0;
    while (next_line(&p, end, &ls, &le)) {
        if (!find_lit(ls, le, "\"type\":\"join_consts\"")) continue;
        const char *bh = find_lit(ls, le, "\"by_hp_class\":[");
        if (!bh) break;
        bh += strlen("\"by_hp_class\":[");
        const char *q = bh;
        while (q < le && k < 256) {
            while (q < le && (*q == ' ' || *q == ',')) ++q;
            if (q >= le || *q == ']') break;
            char *ep = NULL;
            long long v = strtoll(q, &ep, 10);
            if (ep == q) break;
            voicing[k++] = (uint32_t)v;
            q = ep;
        }
        break;
    }
    *n_out = k;
    free(buf);
    return 0;
}

/* Parse wsola_buffer/<text>.jsonl utt 0 chosen UIDs (oracle for compare). */
static int load_oracle_utt0(const char *path, uint32_t *out, uint32_t *n_out,
                            uint32_t cap)
{
    char *buf = NULL; size_t buf_n = 0;
    if (read_file(path, &buf, &buf_n) != 0) return -1;
    const char *p = buf, *end = buf + buf_n;
    const char *ls, *le;
    uint32_t k = 0;
    if (next_line(&p, end, &ls, &le)) {
        const char *up = find_lit(ls, le, "\"units\":[");
        if (up) {
            up += strlen("\"units\":[");
            while (up < le && k < cap) {
                const char *u = find_lit(up, le, "\"uid\":");
                if (!u) break;
                u += strlen("\"uid\":");
                char *ep = NULL;
                long long v = strtoll(u, &ep, 10);
                if (ep == u) break;
                out[k++] = (uint32_t)v;
                up = ep;
                const char *brace = find_lit(up, le, "}");
                if (!brace) break;
                up = brace + 1;
            }
        }
    }
    *n_out = k;
    free(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Audio path (M4 WSOLA: streaming Hann OLA at every boundary,         */
/*             cross-correlation alignment for cross-rec joins)        */
/* ------------------------------------------------------------------ */

static int decode_unit_samples(uint32_t file_idx, uint32_t lp_ms,
                               uint32_t dur_ms,
                               const spfy_feat_table_t *feat,
                               const spfy_vdb_t        *vdb,
                               const spfy_vdb_lookup_t *lookup,
                               int16_t **out, size_t *out_n)
{
    *out = NULL; *out_n = 0;
    if (dur_ms == 0) return SPFY_OK;
    if (file_idx >= feat->n_entries) return SPFY_E_OOB;
    const spfy_feat_entry_t *fe = &feat->entries[file_idx];
    uint32_t rec_off = 0, rec_size = 0;
    int rc = spfy_vdb_lookup_by_name(lookup, fe->name, fe->name_len,
                                     &rec_off, &rec_size);
    if (rc != SPFY_OK) return rc;
    uint32_t off  = lp_ms * 8u;
    uint32_t blen = dur_ms * 8u;
    if (off >= rec_size) return SPFY_OK;
    if (off + blen > rec_size) blen = rec_size - off;
    if (blen == 0) return SPFY_OK;
    int16_t *buf = (int16_t *)malloc(blen * sizeof *buf);
    if (!buf) return SPFY_E_NOMEM;
    spfy_ulaw_decode(vdb->data + rec_off + off, blen, buf);
    *out   = buf;
    *out_n = blen;
    return SPFY_OK;
}

/* Push one decoded chunk into the streamer. align=1 when the chunk
 * starts a new recording (cross-rec or non-adjacent same-rec). */
static int push_decoded(spfy_wsola_streamer_t *ws, int16_t *buf, size_t n,
                        int align)
{
    return spfy_wsola_push_unit(ws, buf, n, align);
}

static int append_recording_span(spfy_wsola_streamer_t *ws,
                                 uint32_t file_idx, uint32_t lp, uint32_t dur,
                                 const spfy_feat_table_t *feat,
                                 const spfy_vdb_t        *vdb,
                                 const spfy_vdb_lookup_t *lookup,
                                 int align)
{
    int16_t *buf = NULL; size_t n = 0;
    int rc = decode_unit_samples(file_idx, lp, dur, feat, vdb, lookup,
                                 &buf, &n);
    if (rc != SPFY_OK) return rc;
    rc = push_decoded(ws, buf, n, align);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Viterbi join cost                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const spfy_hash_t       *hash;
    const spfy_unit_table_t *units;
    float                    miss_default;
} join_ctx_t;

static float join_cb(uint32_t prev_uid, uint32_t curr_uid, void *user)
{
    const join_ctx_t *jc = (const join_ctx_t *)user;
    if (curr_uid == prev_uid + 1u && curr_uid > 0u) {
        spfy_unit_record_t r;
        if (spfy_unit_record_get(jc->units, curr_uid, &r) == SPFY_OK
            && r.flag_b) {
            const char *e = getenv("SPFY_SAME_REC_COST");
            return e ? (float)atof(e) : 0.0f;
        }
    }
    float cost = 0.0f;
    int rc = spfy_hash_lookup(jc->hash, prev_uid, curr_uid, &cost);
    if (rc == SPFY_OK) return cost;
    return jc->miss_default;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc != 8) {
        fprintf(stderr,
            "usage: %s <voice.vin> <voice.vdb> <voice.vcf> "
            "<hpclass.bin> <traces_dir> <text_id> <out.wav>\n",
            argv[0]);
        return 2;
    }
    const char *vin_path    = argv[1];
    const char *vdb_path    = argv[2];
    const char *vcf_path    = argv[3];
    const char *hpc_path    = argv[4];
    const char *traces_dir  = argv[5];
    const char *text_id     = argv[6];
    const char *out_wav     = argv[7];

    spfy_vin_t vin = {0}; spfy_vdb_t vdb = {0}; spfy_vcf_t vcf = {0};
    spfy_unit_table_t units = {0};
    spfy_feat_table_t feat  = {0};
    spfy_vdb_lookup_t lookup = {0};
    spfy_ccos_t ccos = {0};
    spfy_voice_maps_t maps = {0};
    spfy_proscost_matrix_t pros[SPFY_PROSCOST_N] = {0};
    spfy_hash_t hash = {0};
    uint8_t *hpc = NULL; uint32_t hpc_n = 0;
    /* Declared here rather than at first use: every `goto fail` below
     * lands in the shared cleanup block, which frees these. Declaring
     * them further down means an early failure jumps over the
     * initialisation and cleanup then frees indeterminate pointers. */
    spfy_viterbi_slot_t *vslots = NULL;
    uint32_t **cbuf = NULL;
    float    **tbuf = NULL;
    uint32_t   n_slots = 0;
    int rc;

    if ((rc = spfy_vin_load(vin_path, &vin))            != SPFY_OK) goto fail;
    if ((rc = spfy_vdb_load(vdb_path, &vdb))            != SPFY_OK) goto fail;
    if ((rc = spfy_vdb_require_8k_mulaw(&vdb, vdb_path)) != SPFY_OK) goto fail;
    if ((rc = spfy_vcf_load(vcf_path, &vcf))            != SPFY_OK) goto fail;
    if ((rc = spfy_unit_table_load(&vin, &units))       != SPFY_OK) goto fail;
    if ((rc = spfy_feat_table_load(&vin, &feat))        != SPFY_OK) goto fail;
    if ((rc = spfy_vdb_lookup_build(&vdb, &lookup))     != SPFY_OK) goto fail;
    if ((rc = spfy_ccos_load(&vin, &ccos))              != SPFY_OK) goto fail;
    if ((rc = spfy_voice_maps_build(&ccos, &maps))      != SPFY_OK) goto fail;
    if ((rc = spfy_proscost_load(&vcf, pros))           != SPFY_OK) goto fail;
    if ((rc = spfy_hash_load(&vin, &hash))              != SPFY_OK) goto fail;
    if ((rc = spfy_anchor_hpclass_load(hpc_path, &hpc, &hpc_n)) != SPFY_OK)
        goto fail;

    spfy_anchor_voice_t av = {0};
    av.units = &units;
    av.ccos  = &ccos;
    av.maps  = &maps;
    av.proscost = pros;
    av.hpclass_table = hpc;
    av.hpclass_n     = hpc_n;
    spfy_anchor_voice_set_default_weights(&av);

    static uint32_t voicing[256];
    uint32_t voicing_n = 0;
    if (load_voicing(traces_dir, voicing, &voicing_n) == 0) {
        av.voicing   = voicing;
        av.voicing_n = voicing_n;
    }

    /* Load per-slot inputs from FE traces. */
    static slot_input_t slots[MAX_SLOTS];
    memset(slots, 0, sizeof slots);
    char path[1024];
    snprintf(path, sizeof path, "%s/prsl_slot/%s.jsonl", traces_dir, text_id);
    if (load_prsl_slot_utt0(path, slots, &n_slots) != 0) {
        rc = SPFY_E_IO; goto fail;
    }
    snprintf(path, sizeof path, "%s/cart_walks/%s.jsonl", traces_dir, text_id);
    load_cart_walks_utt0(path, slots);
    snprintf(path, sizeof path, "%s/inner_scorer/%s.jsonl", traces_dir, text_id);
    load_inner_scorer_sp_utt0(path, slots);

    /* Load oracle (chosen UIDs) for match% reporting. */
    static uint32_t oracle[MAX_SLOTS];
    uint32_t oracle_n = 0;
    snprintf(path, sizeof path, "%s/wsola_buffer/%s.jsonl", traces_dir, text_id);
    load_oracle_utt0(path, oracle, &oracle_n, MAX_SLOTS);

    fprintf(stdout, "loaded: %u units, %u slots from %s, %u oracle uids, "
                    "voicing_n=%u\n",
            units.n_units, n_slots, text_id, oracle_n, voicing_n);

    /* Build per-slot Viterbi inputs.
     *
     * For each slot s:
     *   - cand pool = prsl_slot uids (in capture order)
     *   - augment with chosen[s-1]+1 (same-rec continuation) when not present
     *   - score every cand via spfy_hp_innerscorer
     *   - boundary slots (where ctx[2] is silence sentinel) keep 1 cand
     *     with cost 0 (terminal) so the DP can still walk through.
     */
    vslots = (spfy_viterbi_slot_t *)calloc(n_slots, sizeof *vslots);
    cbuf   = (uint32_t **)calloc(n_slots, sizeof *cbuf);
    tbuf   = (float    **)calloc(n_slots, sizeof *tbuf);
    if (!vslots || !cbuf || !tbuf) { rc = SPFY_E_NOMEM; goto fail; }

    for (uint32_t s = 0; s < n_slots; ++s) {
        slot_input_t *sl = &slots[s];
        if (!sl->have) {
            /* Slot missing from capture (truncated trace). Inject the
             * oracle UID as a single-cand "free" slot so the DP can run. */
            uint32_t fb = (s < oracle_n) ? oracle[s] : 0u;
            cbuf[s] = (uint32_t *)calloc(1, sizeof **cbuf);
            tbuf[s] = (float    *)calloc(1, sizeof **tbuf);
            cbuf[s][0] = fb; tbuf[s][0] = 0.0f;
            vslots[s].cands = cbuf[s];
            vslots[s].target_cost = tbuf[s];
            vslots[s].n_cands = 1;
            continue;
        }

        /* Augment cand pool with prev-oracle+1 same-rec continuation. */
        uint32_t cap_n = sl->n_cands + 2u;
        cbuf[s] = (uint32_t *)calloc(cap_n, sizeof **cbuf);
        tbuf[s] = (float    *)calloc(cap_n, sizeof **tbuf);
        if (!cbuf[s] || !tbuf[s]) { rc = SPFY_E_NOMEM; goto fail; }

        uint32_t k = 0;
        for (uint32_t i = 0; i < sl->n_cands; ++i)
            cbuf[s][k++] = sl->cands[i];

        /* Same-rec continuation cand: oracle[s-1]+1 if not present. We
         * use the oracle predecessor here because we want the augmented
         * pool to MATCH what the engine actually had access to. */
        if (s > 0 && (s - 1) < oracle_n) {
            uint32_t pp = oracle[s-1] + 1u;
            if (pp < units.n_units) {
                int present = 0;
                for (uint32_t i = 0; i < k; ++i)
                    if (cbuf[s][i] == pp) { present = 1; break; }
                if (!present) cbuf[s][k++] = pp;
            }
        }
        /* Always include oracle[s] for diagnosis (no-op if already present). */
        if (s < oracle_n) {
            int present = 0;
            for (uint32_t i = 0; i < k; ++i)
                if (cbuf[s][i] == oracle[s]) { present = 1; break; }
            if (!present) cbuf[s][k++] = oracle[s];
        }

        /* Score each cand. */
        for (uint32_t i = 0; i < k; ++i) {
            float c = NAN;
            int rcs = spfy_hp_innerscorer(&av, &sl->ctx, &sl->sp,
                                          &sl->cart, cbuf[s][i], &c);
            if (rcs != SPFY_OK || isnan(c) || isinf(c)) {
                tbuf[s][i] = 1e9f;       /* effectively forbidden */
            } else {
                tbuf[s][i] = c;
            }
        }
        vslots[s].cands       = cbuf[s];
        vslots[s].target_cost = tbuf[s];
        vslots[s].n_cands     = k;
    }

    /* Run Viterbi. */
    join_ctx_t jc;
    jc.hash         = &hash;
    jc.units        = &units;
    jc.miss_default = 1000.0f;
    {
        const char *e = getenv("SPFY_JOIN_MISS");
        if (e) jc.miss_default = (float)atof(e);
    }
    uint32_t *path_uids = (uint32_t *)calloc(n_slots, sizeof *path_uids);
    float total = 0.0f;
    int rc_v = spfy_viterbi_run(vslots, n_slots, join_cb, &jc, path_uids, &total);
    if (rc_v != SPFY_OK) {
        fprintf(stderr, "viterbi failed: %s\n", spfy_strerror(rc_v));
        rc = rc_v; free(path_uids); goto fail;
    }

    /* Match% vs oracle. */
    uint32_t n_match = 0, n_cmp = 0;
    uint32_t cmp_n = oracle_n < n_slots ? oracle_n : n_slots;
    for (uint32_t s = 0; s < cmp_n; ++s) {
        ++n_cmp;
        if (path_uids[s] == oracle[s]) ++n_match;
    }
    fprintf(stdout, "viterbi total cost=%.3f, match vs oracle: %u/%u (%.1f%%)\n",
            (double)total, n_match, n_cmp,
            n_cmp ? 100.0 * n_match / n_cmp : 0.0);

    if (getenv("SPFY_DUMP_PATH")) {
        for (uint32_t s = 0; s < n_slots; ++s) {
            uint32_t orc = (s < oracle_n) ? oracle[s] : 0u;
            fprintf(stdout, "  slot %3u: chose=%6u  oracle=%6u  %s "
                            "(%u cands)\n",
                    s, path_uids[s], orc,
                    path_uids[s] == orc ? "ok" : "MISS",
                    vslots[s].n_cands);
        }
    }

    /* ----- Write WAV from chosen UIDs via streaming WSOLA ----- */
    spfy_wav_writer_t wav = {0};
    if ((rc = spfy_wav_open(&wav, out_wav, vdb.sample_rate)) != SPFY_OK) {
        free(path_uids); goto fail;
    }
    spfy_wsola_streamer_t ws;
    spfy_wsola_init(&ws, &wav);
    /* Track previous unit's recording for cross-rec detection (drives
     * the `align` flag passed to the streamer: 1 means run the
     * cross-correlation lag search to align pitch periods at the join,
     * 0 means trust the source audio is already continuous). */
    int      prev_have    = 0;
    uint16_t prev_file_idx = 0;
    uint16_t prev_local_pos = 0;
    uint16_t prev_dur_like  = 0;

    size_t played = 0, skipped = 0,
           paired_same = 0, paired_cross = 0;
    uint32_t s = 0;
    while (s < n_slots) {
        uint32_t u = path_uids[s];
        if (u == 0 || u == SILENCE_SENTINEL_UID || u >= units.n_units) {
            ++skipped; ++s;
            prev_have = 0;
            continue;
        }
        spfy_unit_record_t r1;
        if (spfy_unit_record_get(&units, u, &r1) != SPFY_OK) {
            ++skipped; ++s; prev_have = 0;
            continue;
        }
        /* Same-rec pair detection (still useful: lets us emit ONE
         * combined span instead of two halves with a join). The streamer
         * will then OLA against the previous unit's tail. */
        int paired = 0;
        if (s + 1 < n_slots) {
            uint32_t v = path_uids[s+1];
            spfy_unit_record_t r2;
            if (v != 0 && v != SILENCE_SENTINEL_UID && v < units.n_units
                && spfy_unit_record_get(&units, v, &r2) == SPFY_OK
                && r1.phone_center == r2.phone_center
                && r1.file_idx == r2.file_idx
                && r2.local_pos >= r1.local_pos
                && r2.local_pos <= r1.local_pos + r1.dur_like + 64u) {
                uint32_t span = (uint32_t)r2.local_pos
                              + (uint32_t)r2.dur_like
                              - (uint32_t)r1.local_pos;
                int align = !prev_have || prev_file_idx != r1.file_idx;
                rc = append_recording_span(&ws, r1.file_idx, r1.local_pos,
                                           span, &feat, &vdb, &lookup,
                                           align);
                if (rc != SPFY_OK) { free(path_uids); goto fail_wav; }
                ++paired_same;
                played += 2;
                paired = 1;
                prev_have = 1;
                prev_file_idx  = r2.file_idx;
                prev_local_pos = r2.local_pos;
                prev_dur_like  = r2.dur_like;
                s += 2;
            }
        }
        if (!paired) {
            /* Cross-rec / standalone: emit unit's natural span and let
             * the streamer Hann-OLA the boundary. align=1 whenever the
             * recording or position changes (i.e. not a continuation
             * within the same source). */
            int align = !prev_have || prev_file_idx != r1.file_idx
                     || (uint32_t)r1.local_pos
                        != (uint32_t)prev_local_pos + prev_dur_like;
            if (s + 1 < n_slots) {
                /* Sanity-only counter for cross-rec joins (vs standalone
                 * adjacents that share phone_center but different recs).
                 * The streamer doesn't care; this is for the summary
                 * line below. */
                spfy_unit_record_t r2;
                if (path_uids[s+1] < units.n_units
                    && spfy_unit_record_get(&units, path_uids[s+1], &r2)
                       == SPFY_OK
                    && r1.phone_center == r2.phone_center
                    && r1.file_idx != r2.file_idx) {
                    ++paired_cross;
                }
            }
            rc = append_recording_span(&ws, r1.file_idx, r1.local_pos,
                                       r1.dur_like, &feat, &vdb, &lookup,
                                       align);
            if (rc != SPFY_OK) { free(path_uids); goto fail_wav; }
            ++played;
            prev_have = 1;
            prev_file_idx  = r1.file_idx;
            prev_local_pos = r1.local_pos;
            prev_dur_like  = r1.dur_like;
            ++s;
        }
    }
    rc = spfy_wsola_flush(&ws);
    if (rc != SPFY_OK) { free(path_uids); goto fail_wav; }
    rc = spfy_wav_close(&wav);
    fprintf(stdout, "wrote %s: %u samples (%.2f s) "
                    "[%zu units, %zu same-rec pairs, %zu cross-rec pairs, "
                    "%zu silence skipped, wsola_aligned=%llu/%llu]\n",
            out_wav, wav.n_samples_written,
            (double)wav.n_samples_written / (double)vdb.sample_rate,
            played, paired_same, paired_cross, skipped,
            (unsigned long long)ws.n_aligned,
            (unsigned long long)ws.n_pushes);

    free(path_uids);
    rc = 0;
    goto cleanup;

fail_wav:
    spfy_wav_close(&wav);
fail:
    if (rc != 0) fprintf(stderr, "error: %s\n", spfy_strerror(rc));
cleanup:
    /* Guard on the arrays actually indexed below: vslots can allocate
     * while cbuf/tbuf fail, and n_slots is 0 on any early failure. */
    if (cbuf && tbuf) {
        for (uint32_t i = 0; i < n_slots; ++i) {
            free(cbuf[i]); free(tbuf[i]);
        }
    }
    free(vslots); free(cbuf); free(tbuf);
    spfy_anchor_hpclass_free(hpc);
    spfy_hash_free(&hash);
    spfy_proscost_free(pros);
    spfy_voice_maps_free(&maps);
    spfy_ccos_free(&ccos);
    spfy_vdb_lookup_free(&lookup);
    spfy_feat_table_free(&feat);
    spfy_vcf_free(&vcf);
    spfy_vdb_free(&vdb);
    spfy_vin_free(&vin);
    return rc == 0 ? 0 : 1;
}
