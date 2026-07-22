/* spfy_viterbi_replay -- M3.4 Viterbi DP smoke test on real voice data.
 *
 * Loads a voice's VIN+VCF and reads a captured wsola_buffer JSONL trace
 * (the same file spfy_concat consumes -- the engine's chosen-UID sequence
 * per utterance). For each utterance:
 *
 *   1. Filter boundary markers (uid=0 and uid=last+1 silence sentinels)
 *      from the chosen sequence to leave only real halfphone slots.
 *
 *   2. For each chosen halfphone slot s, derive a triphone context_key
 *      from the slot's neighbors in the chosen sequence and look up the
 *      PRSL candidate pool. The encoding is:
 *
 *        context_key = hp_class(left) * 10000
 *                    + hp_class(this) *   100
 *                    + hp_class(right)
 *
 *      where hp_class = phone_center * 2 + is_first_half_inv. We also try
 *      a phone-only encoding as a fallback (some voices may key by phone).
 *
 *   3. For each candidate UID in the pool, compute target cost (D + F0 +
 *      SP + S) using the chosen unit's OWN features as the synthetic
 *      target. This is necessarily a smoke test, not a true 1:1 acceptance:
 *      the chosen unit always scores 0 on D + F0 + SP + S against itself,
 *      so its rank depends on join cost relative to the other candidates'
 *      D+F0+SP+S+join. Match% < 100% is expected and informative -- it
 *      tells us how much join cost alone would steer us off the engine's
 *      path. A real M3.4 acceptance harness needs the engine's actual
 *      target features (FE-emitted halfphone target struct) which would
 *      come from a new function-entry Frida hook (gap captured in
 *      ../docs/RESUME.md).
 *
 *   4. Run the pure Viterbi DP (src/usel/viterbi.c) with hash-based join
 *      cost. On hash miss the join cost falls back to JOIN_COST_OFFSET
 *      from the VCF (~0.2 for Tom).
 *
 *   5. Compare best-path UIDs to the oracle. Tally per-utterance and
 *      overall match counts.
 *
 * Usage:
 *   spfy_viterbi_replay <voice.vin> <voice.vcf> <wsola_buffer.jsonl>
 *                       [<prsl_slot.jsonl> [<cart_walks.jsonl>]]
 *
 * If a `prsl_slot.jsonl` is given (captured by the prsl_slot Frida hook on
 * `0x08E91DC0` = USelNetwork::AddUnit), the CLI bypasses the heuristic
 * context_key derivation above and uses the engine's actual per-slot
 * context tuple. The captured ctx[5] decomposes via
 *
 *     context_key = ctx[1] * 10000 + ctx[2] * 100 + ctx[3]
 *
 * (decoded empirically: ctx[1] = halfphone class at slot-2, ctx[2] at the
 * slot itself, ctx[3] at slot+2 -- the "same-side triphone" of halfphones
 * 2 positions apart). With this enabled the CLI also stops filtering
 * boundary uids (the engine emits PRSL slots for them too, with single-
 * element pools), so per-utterance slot counts match the wsola_buffer
 * `units` array length.
 *
 * If a `cart_walks.jsonl` is given (captured by the cart_walks hook),
 * the CLI uses the engine's per-slot durt/f0tr leaves as the real target
 * prosody for D + F0 scoring instead of the "chosen-as-target" proxy.
 * The first (utt, slot, durt) and (utt, slot, f0tr) walks per slot are
 * used. Slots without an f0tr walk (typically silence) get the
 * MISSING_F0_COST.
 *
 * In addition, when prsl_slot is enabled, the candidate pool at slot s>0
 * is augmented with `chosen[s-1] + 1` if it isn't already present, to
 * model the engine's "same-recording continuation" path: chosen UIDs
 * outside the PRSL pool come from the engine extending uid -> uid+1
 * across consecutive halfphone slots in the same recording (without
 * re-querying PRSL).
 *
 * Exit code: 0 if all utterances run to completion (regardless of match%),
 *            1 on a load error, 2 on bad args.
 */

#include <spfy/spfy.h>

#include "../voice/voice.h"
#include "../voice/unit_table.h"
#include "../voice/ccos.h"
#include "../voice/voice_runtime.h"
#include "../voice/vcf_matrix.h"
#include "../usel/cost.h"
#include "../usel/prsl.h"
#include "../usel/hash.h"
#include "../usel/viterbi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* VCF helper                                                          */
/* ------------------------------------------------------------------ */

static double get_vcf_f64(const spfy_vcf_t *vcf, const char *key, double dflt)
{
    for (const spfy_vcf_kv_t *kv = vcf->params; kv; kv = kv->next) {
        if (strcmp(kv->key, key) == 0) {
            char *e = NULL;
            double v = strtod(kv->value, &e);
            if (e != kv->value) return v;
        }
    }
    return dflt;
}

/* ------------------------------------------------------------------ */
/* Tiny JSONL field extractor for wsola_buffer lines                   */
/* ------------------------------------------------------------------ */

/* wsola_buffer line shape:
 *   {"type":"wsola_in","utt":1,"n_units":12,"units":[
 *     {"uid":0,"lp":1,"dl":0},
 *     {"uid":87072,"lp":1,"dl":0},
 *     ... ]}
 *
 * We only need the "units":[...] uid sequence. The lp/dl fields are
 * ignored here (they matter to spfy_concat, not to selection). */

typedef struct {
    uint32_t *uids;
    uint32_t  n;
    uint32_t  cap;
} uid_list_t;

static int uid_list_push(uid_list_t *l, uint32_t u)
{
    if (l->n == l->cap) {
        uint32_t  ncap = l->cap ? l->cap * 2u : 16u;
        uint32_t *nu   = (uint32_t *)realloc(l->uids, ncap * sizeof *nu);
        if (!nu) return -1;
        l->uids = nu;
        l->cap  = ncap;
    }
    l->uids[l->n++] = u;
    return 0;
}

static const char *find_lit(const char *p, const char *end, const char *lit)
{
    size_t n = strlen(lit);
    for (; p + n <= end; ++p) if (memcmp(p, lit, n) == 0) return p;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Captured prsl_slot data                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t  slot;
    uint32_t  ctx[5];
    uint32_t  n_cands;
    uint32_t *uids;        /* heap, owned */
} slot_capture_t;

typedef struct {
    slot_capture_t *slots;     /* heap, owned; n entries, ordered slot 0..n-1 */
    uint32_t        n;
    uint32_t        cap;
} utt_capture_t;

typedef struct {
    utt_capture_t *utts;       /* heap, owned */
    uint32_t       n;
    uint32_t       cap;
} prsl_capture_t;

static void slot_capture_free(slot_capture_t *s) { free(s->uids); }

static void utt_capture_free(utt_capture_t *u)
{
    for (uint32_t i = 0; i < u->n; ++i) slot_capture_free(&u->slots[i]);
    free(u->slots); u->slots = NULL; u->n = u->cap = 0;
}

static void prsl_capture_free(prsl_capture_t *p)
{
    for (uint32_t i = 0; i < p->n; ++i) utt_capture_free(&p->utts[i]);
    free(p->utts); p->utts = NULL; p->n = p->cap = 0;
}

static int utt_capture_push(utt_capture_t *u, slot_capture_t s)
{
    if (u->n == u->cap) {
        uint32_t        nc = u->cap ? u->cap * 2u : 8u;
        slot_capture_t *ns = realloc(u->slots, nc * sizeof *ns);
        if (!ns) return -1;
        u->slots = ns; u->cap = nc;
    }
    u->slots[u->n++] = s;
    return 0;
}

static int prsl_capture_push(prsl_capture_t *p, utt_capture_t u)
{
    if (p->n == p->cap) {
        uint32_t       nc = p->cap ? p->cap * 2u : 4u;
        utt_capture_t *nu = realloc(p->utts, nc * sizeof *nu);
        if (!nu) return -1;
        p->utts = nu; p->cap = nc;
    }
    p->utts[p->n++] = u;
    return 0;
}

/* Parse a single prsl_slot JSONL line into one slot_capture_t. The
 * format is:
 *   {"type":"prsl_slot","n":N,"slot":S,"this_ptr":T,"n_cands":C,
 *    "ctx":[c0,c1,c2,c3,c4],"uids":[u0,u1,...]}
 *
 * Note that hot-path failures emit truncated lines with no "uids" key;
 * we treat those as n_cands=0 and skip but don't error out. */
static int parse_prsl_slot_line(const char *line, size_t n,
                                slot_capture_t *out)
{
    memset(out, 0, sizeof *out);
    const char *end = line + n;

    const char *sp = find_lit(line, end, "\"slot\":");
    if (!sp) return -1;
    sp += strlen("\"slot\":");
    char *e = NULL;
    long sv = strtol(sp, &e, 10);
    if (e == sp || sv < 0) return -1;
    out->slot = (uint32_t)sv;

    const char *cxp = find_lit(line, end, "\"ctx\":[");
    if (!cxp) return -1;
    cxp += strlen("\"ctx\":[");
    for (int k = 0; k < 5; ++k) {
        while (cxp < end && (*cxp == ' ' || *cxp == ',')) ++cxp;
        long v = strtol(cxp, &e, 10);
        if (e == cxp) return -1;
        out->ctx[k] = (uint32_t)v;
        cxp = e;
    }

    const char *ncp = find_lit(line, end, "\"n_cands\":");
    if (!ncp) return -1;
    ncp += strlen("\"n_cands\":");
    long nc = strtol(ncp, &e, 10);
    if (e == ncp || nc < 0 || nc > 100000) return -1;
    out->n_cands = (uint32_t)nc;

    /* Optional uids array. */
    const char *up = find_lit(line, end, "\"uids\":[");
    if (!up || out->n_cands == 0) return 0;
    up += strlen("\"uids\":[");
    out->uids = (uint32_t *)calloc(out->n_cands, sizeof *out->uids);
    if (!out->uids) return -1;
    uint32_t got = 0;
    while (up < end && *up != ']' && got < out->n_cands) {
        while (up < end && (*up == ' ' || *up == ',')) ++up;
        if (up >= end || *up == ']') break;
        long v = strtol(up, &e, 10);
        if (e == up) break;
        out->uids[got++] = (uint32_t)v;
        up = e;
    }
    out->n_cands = got;       /* in case the line was truncated */
    return 0;
}

/* Load the prsl_slot JSONL file, splitting into utterances at slot=0
 * boundaries. Returns 0 on success and populates *out. */
static int load_prsl_capture(const char *path, prsl_capture_t *out)
{
    memset(out, 0, sizeof *out);
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    char buf[1 << 17];        /* 128 KiB lines: pools can be up to ~200 uids */
    utt_capture_t cur = {0};
    int prev_slot = -1;
    int rc = 0;

    while (fgets(buf, sizeof buf, fp)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
        if (n == 0) continue;

        slot_capture_t s = {0};
        if (parse_prsl_slot_line(buf, n, &s) != 0) continue;

        /* Utterance boundary: slot index reset to 0 (after seeing a higher
         * slot index in the previous record). */
        if ((int)s.slot == 0 && prev_slot > 0) {
            if (prsl_capture_push(out, cur) != 0) {
                slot_capture_free(&s);
                rc = -1;
                goto done;
            }
            memset(&cur, 0, sizeof cur);
        }
        if (utt_capture_push(&cur, s) != 0) {
            slot_capture_free(&s);
            rc = -1;
            goto done;
        }
        prev_slot = (int)s.slot;
    }
    if (cur.n > 0) {
        if (prsl_capture_push(out, cur) != 0) rc = -1;
    } else {
        utt_capture_free(&cur);
    }
done:
    fclose(fp);
    if (rc != 0) prsl_capture_free(out);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Captured inner_scorer data (per-slot SP target features)            */
/* ------------------------------------------------------------------ */

typedef struct {
    int      has;
    uint32_t sp[5];         /* 5 target SP row indices, in cost-formula order */
} sp_target_t;

/* Engine weights captured once from the inner_scorer trace. The engine
 * reads them from `slice+0x24` -> {weight_struct}, so they're effectively
 * per-voice constants for any given utterance. We just take the first
 * valid sample per file and assume it holds for the whole run. */
typedef struct {
    int    has;
    float  sp[5];
    float  d;
    float  f0;
    float  ccos;
    float  miss_f0;
    float  flag;       /* extra term coefficient (cand.byte+0x17 * flag * c) */
} engine_weights_t;

/* hp_class remap table (voice+0x608). Maps slice.ctx encoding [0..93] to
 * ccos forest index [0..93]. Most entries follow ctx -> ctx/2 + 47*(ctx%2)
 * but the engine swaps some phone pairs (9/10/11, 14/15 in Tom). Captured
 * from inner_scorer's hp_class_remap event. */
typedef struct {
    int      has;
    uint32_t map[256];   /* indexed by slice.ctx value (u8 range) */
    uint32_t n_entries;  /* typically 2 * n_labels = 94 */
} hp_remap_t;

/* S-cost target context remap (voice+0x604). Maps slice.ctx values to
 * label indices for ccos row lookup. Pattern: ctx -> ctx/2 with the
 * same phone-pair swaps as hp_remap (phones 9/10/11 and 14/15 in Tom).
 * Captured from inner_scorer's s_ctx_remap event. */
typedef struct {
    int      has;
    uint32_t map[256];   /* indexed by slice.ctx value */
    uint32_t n_entries;  /* typically 2 * n_labels = 94 */
} s_ctx_remap_t;

/* Voicing-flag gate for F0 cost. Captured from inner_scorer's
 * `join_consts.voice_5fc_per_uid` block (which dumps voice+0x5fc[0..93]
 * indexed by halfphone class). The engine's InnerScorer (FUN_08e88de0)
 * gates F0 evaluation:
 *
 *   if (voice+0x5fc[slice.ctx[2]] == 0 && weight+0x8c == 0):
 *       local_24 = 0   // skip f0tr walk; F0 cost = 0 for ALL cands
 *
 * Without this gate, our per-cand F0 path adds MISSING_F0_COST=5 for
 * silence cands (cand_byte+0xf == 0), but the engine zeroes the whole
 * F0 component. Confirmed for text_001 slot 11 uid 169578 (M3.4q). */
typedef struct {
    int      has;
    uint32_t by_hp_class[256]; /* index = slice.ctx[2]; 0 means unvoiced */
    uint32_t n_entries;        /* typically 94 (2 * n_labels) */
    uint32_t weight_8c;        /* second half of the engine gate */
} voicing_gate_t;

typedef struct {
    sp_target_t      *slots;     /* heap, owned; index = slot number */
    uint32_t          n;
    uint32_t          cap;
    engine_weights_t  weights;   /* may .has = 0 */
    hp_remap_t        hp_remap;  /* may .has = 0 */
    s_ctx_remap_t     s_remap;   /* may .has = 0 */
    voicing_gate_t    voicing;   /* may .has = 0 */
} utt_sp_targets_t;

typedef struct {
    utt_sp_targets_t *utts;
    uint32_t          n;
    uint32_t          cap;
} sp_capture_t;

static int utt_sp_ensure(utt_sp_targets_t *u, uint32_t need)
{
    if (need < u->cap) return 0;
    uint32_t nc = u->cap ? u->cap : 16u;
    while (nc <= need) nc *= 2u;
    sp_target_t *ns = realloc(u->slots, nc * sizeof *ns);
    if (!ns) return -1;
    for (uint32_t i = u->cap; i < nc; ++i) memset(&ns[i], 0, sizeof ns[i]);
    u->slots = ns; u->cap = nc;
    return 0;
}

static int sp_capture_push(sp_capture_t *c, utt_sp_targets_t u)
{
    if (c->n == c->cap) {
        uint32_t          nc = c->cap ? c->cap * 2u : 4u;
        utt_sp_targets_t *nu = realloc(c->utts, nc * sizeof *nu);
        if (!nu) return -1;
        c->utts = nu; c->cap = nc;
    }
    c->utts[c->n++] = u;
    return 0;
}

static void sp_capture_free(sp_capture_t *c)
{
    if (!c) return;
    for (uint32_t i = 0; i < c->n; ++i) free(c->utts[i].slots);
    free(c->utts);
    memset(c, 0, sizeof *c);
}

/* Parse one inner_scorer JSONL line. Format:
 *   {"type":"inner_scorer","n":N,"slot":S,"this_ptr":P,"net_ptr":Q,
 *    "sp_target":[a,b,c,d,e],
 *    "weights":{"sp":[a,b,c,d,e],"f0":F,"d":D,"flag":FL,"ccos":CC,
 *               "w_4c":W4C,"miss_f0":M}}  (weights optional)
 * Populates *out_slot, out_sp[]; on success populates *out_weights if
 * the weights block is present (out_weights->has set to 1). Returns 0
 * on success. */
static int parse_inner_scorer_line(const char *line, size_t n,
                                   uint32_t *out_slot,
                                   uint32_t out_sp[5],
                                   engine_weights_t *out_weights)
{
    const char *end = line + n;

    const char *sp = find_lit(line, end, "\"slot\":");
    if (!sp) return -1;
    sp += strlen("\"slot\":");
    char *e = NULL;
    long sv = strtol(sp, &e, 10);
    if (e == sp || sv < 0) return -1;
    *out_slot = (uint32_t)sv;

    const char *tp = find_lit(line, end, "\"sp_target\":[");
    if (!tp) return -1;
    tp += strlen("\"sp_target\":[");
    for (int k = 0; k < 5; ++k) {
        while (tp < end && (*tp == ' ' || *tp == ',')) ++tp;
        /* Each may be a number or `null` (sp field couldn't be read). */
        if (tp + 4 <= end && memcmp(tp, "null", 4) == 0) {
            out_sp[k] = 0xFFFFFFFFu;
            tp += 4;
            continue;
        }
        long v = strtol(tp, &e, 10);
        if (e == tp) return -1;
        out_sp[k] = (uint32_t)v;
        tp = e;
    }

    /* Optional weights block. */
    if (out_weights && !out_weights->has) {
        const char *wp = find_lit(line, end, "\"weights\":{");
        if (wp) {
            wp += strlen("\"weights\":{");
            const char *spw = find_lit(wp, end, "\"sp\":[");
            if (spw) {
                spw += strlen("\"sp\":[");
                for (int k = 0; k < 5; ++k) {
                    while (spw < end && (*spw == ' ' || *spw == ',')) ++spw;
                    double v = strtod(spw, &e);
                    if (e == spw) break;
                    out_weights->sp[k] = (float)v;
                    spw = e;
                }
            }
            const char *fp;
            #define READ_FIELD(name, dest) do {                            \
                fp = find_lit(wp, end, "\"" name "\":");                   \
                if (fp) {                                                  \
                    fp += strlen("\"" name "\":");                         \
                    double v = strtod(fp, &e);                             \
                    if (e != fp) (dest) = (float)v;                        \
                }                                                          \
            } while (0)
            READ_FIELD("f0",      out_weights->f0);
            READ_FIELD("d",       out_weights->d);
            READ_FIELD("flag",    out_weights->flag);
            READ_FIELD("ccos",    out_weights->ccos);
            READ_FIELD("miss_f0", out_weights->miss_f0);
            #undef READ_FIELD
            out_weights->has = 1;
        }
    }
    return 0;
}

/* Helper: parse a `"<key>":[a,b,c,...]` array of small ints into out_map
 * (size 256). Returns count parsed (0 if key not found / parse error). */
static uint32_t parse_u32_array(const char *buf, size_t n,
                                const char *key,
                                uint32_t out_map[256])
{
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\":[", key);
    const char *bp = find_lit(buf, buf + n, needle);
    if (!bp) return 0;
    bp += strlen(needle);
    uint32_t k;
    for (k = 0; k < 256u; ++k) {
        while (bp < buf + n && (*bp == ' ' || *bp == ',')) ++bp;
        if (bp >= buf + n || *bp == ']') break;
        char *e = NULL;
        long v = strtol(bp, &e, 10);
        if (e == bp) break;
        out_map[k] = (uint32_t)v;
        bp = e;
    }
    return k;
}

/* Try to extract voice-wide constants (voicing gate, hp_class_remap,
 * s_ctx_remap) from a (possibly different) inner_scorer JSONL. Used as
 * fallback because each of these events only fires on the very first
 * inner_scorer call of a Frida session — so only the FIRST trace file
 * in a corpus run carries them. The data is voice-wide (doesn't change
 * per utterance), so falling back to a sibling file is safe. Returns 0
 * if at least one of the three was loaded. */
static int load_voice_wide_only(const char *path,
                                voicing_gate_t *out_voicing,
                                hp_remap_t     *out_hp,
                                s_ctx_remap_t  *out_s)
{
    memset(out_voicing, 0, sizeof *out_voicing);
    memset(out_hp,      0, sizeof *out_hp);
    memset(out_s,       0, sizeof *out_s);
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    char buf[1 << 14];
    while (fgets(buf, sizeof buf, fp)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
            buf[--n] = 0;
        if (n == 0) continue;

        if (find_lit(buf, buf + n, "\"type\":\"join_consts\"")) {
            uint32_t cnt = parse_u32_array(buf, n, "by_hp_class",
                                           out_voicing->by_hp_class);
            if (cnt > 0) {
                out_voicing->n_entries = cnt;
                out_voicing->has       = 1;
            }
            const char *wp = find_lit(buf, buf + n, "\"weight_8c\":");
            if (wp) {
                wp += strlen("\"weight_8c\":");
                char *e = NULL;
                long v = strtol(wp, &e, 10);
                if (e != wp) out_voicing->weight_8c = (uint32_t)v;
            }
        }
        else if (find_lit(buf, buf + n, "\"type\":\"hp_class_remap\"")) {
            uint32_t cnt = parse_u32_array(buf, n, "remap",
                                           out_hp->map);
            if (cnt > 0) {
                out_hp->n_entries = cnt;
                out_hp->has       = 1;
            }
        }
        else if (find_lit(buf, buf + n, "\"type\":\"s_ctx_remap\"")) {
            uint32_t cnt = parse_u32_array(buf, n, "remap", out_s->map);
            if (cnt > 0) {
                out_s->n_entries = cnt;
                out_s->has       = 1;
            }
        }
        if (out_voicing->has && out_hp->has && out_s->has) break;
    }
    fclose(fp);
    return (out_voicing->has || out_hp->has || out_s->has) ? 0 : -1;
}

/* Load inner_scorer JSONL. Splits utterances at slot=0 boundaries. */
static int load_sp_capture(const char *path, sp_capture_t *out)
{
    memset(out, 0, sizeof *out);
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    char buf[1 << 14];
    utt_sp_targets_t cur = {0};
    int prev_slot = -1;
    int rc = 0;

    while (fgets(buf, sizeof buf, fp)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
        if (n == 0) continue;

        /* hp_class_remap event lines: a one-shot table dump at session
         * start. Capture into cur.hp_remap. */
        if (find_lit(buf, buf + n, "\"type\":\"hp_class_remap\"")) {
            const char *rp = find_lit(buf, buf + n, "\"remap\":[");
            if (rp) {
                rp += strlen("\"remap\":[");
                cur.hp_remap.n_entries = 0;
                for (uint32_t k = 0; k < 256u; ++k) {
                    while (rp < buf + n && (*rp == ' ' || *rp == ','))
                        ++rp;
                    if (rp >= buf + n || *rp == ']') break;
                    char *e = NULL;
                    long v = strtol(rp, &e, 10);
                    if (e == rp) break;
                    cur.hp_remap.map[k] = (uint32_t)v;
                    cur.hp_remap.n_entries++;
                    rp = e;
                }
                cur.hp_remap.has = 1;
            }
            continue;
        }
        /* join_consts: one-shot dump of the voicing-flag table
         * (voice+0x5fc, 94 entries indexed by halfphone class) and
         * weight+0x8c. Together they form the F0-evaluation gate. */
        if (find_lit(buf, buf + n, "\"type\":\"join_consts\"")) {
            const char *bp = find_lit(buf, buf + n, "\"by_hp_class\":[");
            if (bp) {
                bp += strlen("\"by_hp_class\":[");
                cur.voicing.n_entries = 0;
                for (uint32_t k = 0; k < 256u; ++k) {
                    while (bp < buf + n && (*bp == ' ' || *bp == ','))
                        ++bp;
                    if (bp >= buf + n || *bp == ']') break;
                    char *e = NULL;
                    long v = strtol(bp, &e, 10);
                    if (e == bp) break;
                    cur.voicing.by_hp_class[k] = (uint32_t)v;
                    cur.voicing.n_entries++;
                    bp = e;
                }
                cur.voicing.has = 1;
            }
            const char *wp = find_lit(buf, buf + n, "\"weight_8c\":");
            if (wp) {
                wp += strlen("\"weight_8c\":");
                char *e = NULL;
                long v = strtol(wp, &e, 10);
                if (e != wp) cur.voicing.weight_8c = (uint32_t)v;
            }
            continue;
        }
        /* s_ctx_remap (voice+0x604): target ctx -> label remap for
         * S-cost row lookup. Same parser as hp_class_remap. */
        if (find_lit(buf, buf + n, "\"type\":\"s_ctx_remap\"")) {
            const char *rp = find_lit(buf, buf + n, "\"remap\":[");
            if (rp) {
                rp += strlen("\"remap\":[");
                cur.s_remap.n_entries = 0;
                for (uint32_t k = 0; k < 256u; ++k) {
                    while (rp < buf + n && (*rp == ' ' || *rp == ','))
                        ++rp;
                    if (rp >= buf + n || *rp == ']') break;
                    char *e = NULL;
                    long v = strtol(rp, &e, 10);
                    if (e == rp) break;
                    cur.s_remap.map[k] = (uint32_t)v;
                    cur.s_remap.n_entries++;
                    rp = e;
                }
                cur.s_remap.has = 1;
            }
            continue;
        }

        uint32_t slot;
        uint32_t spv[5];
        if (parse_inner_scorer_line(buf, n, &slot, spv, &cur.weights) != 0)
            continue;

        if ((int)slot == 0 && prev_slot > 0 && cur.n > 0) {
            /* Save voice-wide constants (weights, remap tables) before
             * reset -- they only fire on the FIRST inner_scorer call
             * per file, but apply to every utterance in the file. */
            engine_weights_t saved_w  = cur.weights;
            hp_remap_t       saved_h  = cur.hp_remap;
            s_ctx_remap_t    saved_s  = cur.s_remap;
            voicing_gate_t   saved_v  = cur.voicing;
            if (sp_capture_push(out, cur) != 0) { rc = -1; goto done; }
            memset(&cur, 0, sizeof cur);
            cur.weights  = saved_w;
            cur.hp_remap = saved_h;
            cur.s_remap  = saved_s;
            cur.voicing  = saved_v;
        }
        if (utt_sp_ensure(&cur, slot) != 0) { rc = -1; goto done; }
        if (slot >= cur.n) cur.n = slot + 1;

        sp_target_t *t = &cur.slots[slot];
        if (!t->has) {
            for (int k = 0; k < 5; ++k) t->sp[k] = spv[k];
            t->has = 1;
        }
        prev_slot = (int)slot;
    }
    if (cur.n > 0) {
        if (sp_capture_push(out, cur) != 0) rc = -1;
    } else {
        free(cur.slots);
    }
done:
    fclose(fp);
    if (rc != 0) sp_capture_free(out);

    /* Fallback: voice-wide events (join_consts / hp_class_remap /
     * s_ctx_remap) are dumped by the Frida hook only on the FIRST
     * trace of a session — typically text_001.jsonl. Pull them from
     * the sibling so all corpus entries inherit the same voice-wide
     * tables. Apply each table to every utterance we just pushed
     * (only filling in tables that the current file didn't have). */
    if (rc == 0 && out->n > 0 &&
        (!out->utts[0].voicing.has  ||
         !out->utts[0].hp_remap.has ||
         !out->utts[0].s_remap.has)) {
        voicing_gate_t fb_v = {0};
        hp_remap_t     fb_h = {0};
        s_ctx_remap_t  fb_s = {0};
        char sibling[1024];
        const char *slash = strrchr(path, '/');
        const char *bslash = strrchr(path, '\\');
        const char *base = slash > bslash ? slash : bslash;
        if (base) {
            size_t dirlen = (size_t)(base - path) + 1u;
            if (dirlen < sizeof sibling) {
                memcpy(sibling, path, dirlen);
                snprintf(sibling + dirlen, sizeof sibling - dirlen,
                         "text_001.jsonl");
                if (load_voice_wide_only(sibling, &fb_v,
                                         &fb_h, &fb_s) == 0) {
                    for (uint32_t u = 0; u < out->n; ++u) {
                        if (!out->utts[u].voicing.has  && fb_v.has)
                            out->utts[u].voicing  = fb_v;
                        if (!out->utts[u].hp_remap.has && fb_h.has)
                            out->utts[u].hp_remap = fb_h;
                        if (!out->utts[u].s_remap.has  && fb_s.has)
                            out->utts[u].s_remap  = fb_s;
                    }
                }
            }
        }
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Captured cart_walks data                                            */
/* ------------------------------------------------------------------ */

/* Per-slot target prosody from the engine's CART traversals. The hook
 * emits one walk per (slot, tree) per call -- and InnerScorer calls CART
 * many times per slot (once per candidate scoring pass), so we keep only
 * the first occurrence per (utt, slot, tree). Same-tree repeats produce
 * identical leaves by construction (deterministic traversal of a fixed
 * tree against a fixed slot context). */

typedef struct {
    int   has_durt;
    float durt_mean;
    float durt_var;
    int   has_f0tr;
    float f0tr_mean;
    float f0tr_var;
} cart_targets_t;

typedef struct {
    cart_targets_t *slots;     /* heap, owned; index = slot number */
    uint32_t        n;
    uint32_t        cap;
} utt_targets_t;

typedef struct {
    utt_targets_t *utts;       /* heap, owned */
    uint32_t       n;
    uint32_t       cap;
} cart_capture_t;

static int utt_targets_ensure(utt_targets_t *u, uint32_t need)
{
    if (need < u->cap) return 0;
    uint32_t nc = u->cap ? u->cap : 16u;
    while (nc <= need) nc *= 2u;
    cart_targets_t *ns = realloc(u->slots, nc * sizeof *ns);
    if (!ns) return -1;
    /* Zero-init newly added range. */
    for (uint32_t i = u->cap; i < nc; ++i) memset(&ns[i], 0, sizeof ns[i]);
    u->slots = ns; u->cap = nc;
    return 0;
}

static int cart_capture_push(cart_capture_t *c, utt_targets_t u)
{
    if (c->n == c->cap) {
        uint32_t       nc = c->cap ? c->cap * 2u : 4u;
        utt_targets_t *nu = realloc(c->utts, nc * sizeof *nu);
        if (!nu) return -1;
        c->utts = nu; c->cap = nc;
    }
    c->utts[c->n++] = u;
    return 0;
}

static void cart_capture_free(cart_capture_t *c)
{
    if (!c) return;
    for (uint32_t i = 0; i < c->n; ++i) free(c->utts[i].slots);
    free(c->utts);
    memset(c, 0, sizeof *c);
}

/* Parse one cart_walks JSONL line. Returns 0 on success. Out-params:
 *   *out_slot: slot index
 *   *out_tree: 0 = durt, 1 = f0tr, -1 = unknown (skip)
 *   *out_mean, *out_var: leaf values */
static int parse_cart_walk_line(const char *line, size_t n,
                                uint32_t *out_slot, int *out_tree,
                                float *out_mean, float *out_var)
{
    const char *end = line + n;
    char tree_buf[16] = {0};

    const char *tp = find_lit(line, end, "\"tree\":\"");
    if (!tp) return -1;
    tp += strlen("\"tree\":\"");
    size_t i = 0;
    while (tp < end && *tp != '"' && i + 1 < sizeof tree_buf) {
        tree_buf[i++] = *tp++;
    }
    tree_buf[i] = 0;
    if      (strcmp(tree_buf, "durt") == 0) *out_tree = 0;
    else if (strcmp(tree_buf, "f0tr") == 0) *out_tree = 1;
    else                                    return -1;

    const char *sp = find_lit(line, end, "\"slot\":");
    if (!sp) return -1;
    sp += strlen("\"slot\":");
    char *e = NULL;
    long sv = strtol(sp, &e, 10);
    if (e == sp || sv < 0) return -1;
    *out_slot = (uint32_t)sv;

    const char *mp = find_lit(line, end, "\"leaf_mean\":");
    if (!mp) return -1;
    mp += strlen("\"leaf_mean\":");
    double mv = strtod(mp, &e);
    if (e == mp) return -1;
    *out_mean = (float)mv;

    const char *vp = find_lit(line, end, "\"leaf_var\":");
    if (!vp) return -1;
    vp += strlen("\"leaf_var\":");
    double vv = strtod(vp, &e);
    if (e == vp) return -1;
    *out_var = (float)vv;
    return 0;
}

/* Load cart_walks JSONL. Splits into utterances at slot=0 boundaries
 * (same trick as prsl_slot loader). For each utterance + slot, keeps
 * only the FIRST durt walk and FIRST f0tr walk. */
static int load_cart_capture(const char *path, cart_capture_t *out)
{
    memset(out, 0, sizeof *out);
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    char buf[1 << 16];
    utt_targets_t cur = {0};
    int prev_slot = -1;
    int rc = 0;

    while (fgets(buf, sizeof buf, fp)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
        if (n == 0) continue;

        uint32_t slot;
        int      tree;
        float    mean, var;
        if (parse_cart_walk_line(buf, n, &slot, &tree, &mean, &var) != 0)
            continue;

        /* Utterance boundary: slot reset to 0 after seeing higher.
         * cart_walks intermixes durt and f0tr, so a slot=0 walk after
         * earlier slot>0 walks marks a new utterance. */
        if ((int)slot == 0 && prev_slot > 0 && cur.n > 0) {
            int has_zero = (cur.n > 0 && (cur.slots[0].has_durt ||
                                          cur.slots[0].has_f0tr));
            /* Only flush if we already saw slot 0 in current utt. The
             * leading boundary slot in a new utt is always slot=0. */
            if (has_zero) {
                if (cart_capture_push(out, cur) != 0) {
                    rc = -1; goto done;
                }
                memset(&cur, 0, sizeof cur);
            }
        }

        if (utt_targets_ensure(&cur, slot) != 0) { rc = -1; goto done; }
        if (slot >= cur.n) cur.n = slot + 1;

        cart_targets_t *t = &cur.slots[slot];
        if (tree == 0 && !t->has_durt) {
            t->durt_mean = mean; t->durt_var = var; t->has_durt = 1;
        } else if (tree == 1 && !t->has_f0tr) {
            t->f0tr_mean = mean; t->f0tr_var = var; t->has_f0tr = 1;
        }
        prev_slot = (int)slot;
    }
    if (cur.n > 0) {
        if (cart_capture_push(out, cur) != 0) rc = -1;
    } else {
        free(cur.slots);
    }
done:
    fclose(fp);
    if (rc != 0) cart_capture_free(out);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Wsola buffer parser                                                 */
/* ------------------------------------------------------------------ */

/* Parse the "units":[ ... ] uid sequence into list. Returns 0 on success. */
static int parse_units(const char *line, size_t n, uid_list_t *out)
{
    out->n = 0;
    const char *end = line + n;
    const char *p   = find_lit(line, end, "\"units\":[");
    if (!p) return -1;
    p += strlen("\"units\":[");
    /* Walk objects { ... }, extract "uid": NUMBER from each, until ']'. */
    while (p < end && *p != ']') {
        const char *u = find_lit(p, end, "\"uid\":");
        if (!u || u >= end) break;
        u += strlen("\"uid\":");
        char *e = NULL;
        long v = strtol(u, &e, 10);
        if (e == u) break;
        if (uid_list_push(out, (uint32_t)v) < 0) return -1;
        p = e;
        /* Advance past this object's closing brace. */
        const char *brace = find_lit(p, end, "}");
        if (!brace) break;
        p = brace + 1;
        if (p < end && *p == ',') ++p;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Cost glue                                                           */
/* ------------------------------------------------------------------ */

/* Per-replay scoring context. Held by reference inside the join callback
 * via a small wrapper so the Viterbi DP stays generic. Cost weights can
 * be overridden per-utterance from the inner_scorer capture (where the
 * engine's real weight struct was read from slice+0x24); when no
 * capture is present we use the VCF/default values. */
typedef struct {
    const spfy_unit_table_t       *units;
    const spfy_voice_maps_t       *maps;
    const spfy_ccos_t             *ccos;
    const spfy_proscost_matrix_t  *pmats;     /* [SPFY_PROSCOST_N] */
    /* Cost weights. */
    float dur_w;
    float f0_w;
    float ccos_w;
    float miss_f0;
    float w_sp[5];
    /* Target features (chosen unit). */
    spfy_unit_record_t target;
} score_ctx_t;

/* Join callback. Engine formula (decoded from FUN_08e8b620 Viterbi):
 *
 *   if (curr_uid == prev_uid + 1 && curr.flag_b != 0):
 *       J = 0                           // same-rec adjacency: free!
 *   else if (hash hit):
 *       J = hash_value                  // RAW value, no weight, no offset
 *   else (hash miss):
 *       if (ccos gate conditions met):
 *           J = gate_weight * ccos_curve[idx] + miss_offset
 *       else:
 *           J = miss_base + miss_offset // global constants
 *
 * The same-rec adjacency rule is the explanation for the bimodal
 * match-rate distribution in M3.4h: utterances dominated by same-rec
 * runs got penalized when our previous code applied JOIN_COST_WEIGHT *
 * MISSING_JOIN_COST (~7000) to those transitions. */
typedef struct {
    const spfy_hash_t       *hash;
    const spfy_unit_table_t *units;     /* for flag_b lookup */
    float                    miss_default;  /* legacy fallback miss cost (curve == NULL path) */
    /* Engine-faithful F0-prob curve (VIN `hist` chunk + voice+0xc8) for
     * the CCOS-gate hash-miss path (FUN_08e8b620 @ 0x08e8b7f8). Same
     * shape as spfy_synth.c's join_ctx_t — see plan 02-02 §"THIRD scope
     * revision" in 02-DP-AUDIT.md. When curve == NULL the dag callback
     * falls back to miss_default for any hash miss (legacy behaviour). */
    const float             *curve;
    int32_t                  curve_max_idx;
    int32_t                  curve_sub_off;
    float                    f0_edge_change_weight;
    float                    missing_join_cost;
} join_ctx_t;

/* Parse VIN `hist` sub-chunks (head + data) and populate the curve params.
 * On any malformed-chunk error returns a "no curve" state (legacy miss).
 * Lifted verbatim from spfy_synth.c::load_f0_hist_curve (plan 02-02). */
static void load_f0_hist_curve(const spfy_vin_t *vin, join_ctx_t *jc)
{
    jc->curve = NULL;
    jc->curve_max_idx = 0;
    jc->curve_sub_off = 0;
    if (!vin || !vin->hist || vin->hist_n < 16) return;
    const uint8_t *p   = vin->hist;
    const uint8_t *end = vin->hist + vin->hist_n;
    while (p + 8 <= end) {
        uint32_t fcc = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                     | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        uint32_t sz  = (uint32_t)p[4] | ((uint32_t)p[5] << 8)
                     | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        const uint8_t *body = p + 8;
        if (body + sz > end) return;
        if (fcc == 0x64616568u /* 'head' LE */ && sz >= 8) {
            uint32_t mx, off;
            memcpy(&mx,  body,     4);
            memcpy(&off, body + 4, 4);
            jc->curve_max_idx = (int32_t)mx;
            jc->curve_sub_off = (int32_t)off;
        } else if (fcc == 0x61746164u /* 'data' LE */) {
            jc->curve = (const float *)body;
        }
        p = body + sz;
        if (sz & 1) ++p;
    }
}

/* Legacy linear join callback (kept for diagnostic env-flag fallback;
 * the default DP path now goes through dag_join_cb below).
 *
 * Engine formula (decoded from FUN_08e8b620 Viterbi):
 *   if (curr_uid == prev_uid + 1 && curr.flag_b != 0):  J = 0
 *   else if (hash hit):                                  J = hash_value
 *   else (hash miss):                                    J = miss_default
 *
 * NOTE: this lacks the engine's CCOS-gate hash-miss path — for that
 * use dag_join_cb. Kept here for SPFY_LEGACY_VIT=1 diagnostic mode. */
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
    int   rc   = spfy_hash_lookup(jc->hash, prev_uid, curr_uid, &cost);
    if (rc == SPFY_OK) return cost;
    return jc->miss_default;
}

/* DAG join callback — engine-exact FUN_08e8b620 hash-miss CCOS-gate.
 * Lifted from spfy_synth.c::dag_join_cb (plan 02-02 THIRD scope revision).
 *
 *   if (same-rec adjacent):              J = 0
 *   else if (hash hit):                  J = hash_value
 *   else (hash miss):
 *     if curr.c6c > 20 AND prev.c80 < 15 AND prev.c7c > 20:
 *       idx = clamp(curr.c6c - sub_off - prev.c7c, 0, max_idx-1)
 *       J = MISSING_JOIN_COST + F0_EDGE_CHANGE_WEIGHT * curve[idx]
 *     else:
 *       J = MISSING_JOIN_COST + 0    (CCOS_DEFAULT = 0 for Tom)
 *
 * If jc->curve == NULL falls back to miss_default for any miss. */
/* Diagnostic counters for the DAG-path join cost. Reset in main() and
 * printed at end. SPFY_DAG_JOIN_TRACE=1 dumps each call to stderr. */
static long g_dag_total = 0;
static long g_dag_same_rec = 0;
static long g_dag_hash_hit = 0;
static long g_dag_miss_total = 0;
static long g_dag_miss_gate_fire = 0;

static float dag_join_cb(uint32_t prev_uid_join_key, uint32_t curr_uid,
                         uint32_t prev_slot, uint32_t prev_idx,
                         uint32_t curr_slot, uint32_t curr_idx,
                         int32_t  prev_c7c,  int32_t  prev_c80,
                         uint32_t curr_c6c,  void    *user)
{
    (void)prev_slot; (void)prev_idx; (void)curr_slot; (void)curr_idx;
    const join_ctx_t *jc = (const join_ctx_t *)user;
    g_dag_total++;
    if (curr_uid == prev_uid_join_key + 1u && curr_uid > 0u) {
        spfy_unit_record_t r;
        if (spfy_unit_record_get(jc->units, curr_uid, &r) == SPFY_OK
            && r.flag_b) {
            g_dag_same_rec++;
            const char *e = getenv("SPFY_SAME_REC_COST");
            return e ? (float)atof(e) : 0.0f;
        }
    }
    float cost = 0.0f;
    int rc = spfy_hash_lookup(jc->hash, prev_uid_join_key, curr_uid, &cost);
    if (rc == SPFY_OK) { g_dag_hash_hit++; return cost; }
    g_dag_miss_total++;
    if (!jc->curve) return jc->miss_default;
    float curve_val = 0.0f;
    if ((int32_t)curr_c6c > 20 && prev_c80 < 15 && prev_c7c > 20) {
        g_dag_miss_gate_fire++;
        int32_t idx = (int32_t)curr_c6c - jc->curve_sub_off - prev_c7c;
        if (idx < 0) idx = 0;
        else if (idx >= jc->curve_max_idx) idx = jc->curve_max_idx - 1;
        curve_val = jc->f0_edge_change_weight * jc->curve[idx];
        if (getenv("SPFY_DAG_JOIN_TRACE")) {
            fprintf(stderr,
                    "  dag-gate fire prev=(uid=%u,c7c=%d,c80=%d) "
                    "curr=(uid=%u,c6c=%u) idx=%d curve=%.4f -> %.4f\n",
                    prev_uid_join_key, prev_c7c, prev_c80,
                    curr_uid, curr_c6c, idx,
                    (double)jc->curve[idx],
                    (double)(jc->missing_join_cost + curve_val));
        }
    }
    return jc->missing_join_cost + curve_val;
}

/* Per-component cost breakdown for diagnostic output. */
typedef struct {
    float D, F0, SP, S, FLAG;
} cost_breakdown_t;

/* Compute total target cost for one candidate.
 *
 * If `tgt` is non-NULL, use the engine's captured durt/f0tr leaves as the
 * D and F0 prediction means + scales. Otherwise fall back to the
 * "chosen-UID-as-target" proxy where D and F0 are predicted from the
 * chosen unit's own stored values with a fixed 0.1 scale.
 *
 * If `slice_ctx` is non-NULL it points to the engine's captured 5-element
 * halfphone context tuple (from prsl_slot at the same slot). The S-cost
 * uses ctx[0], ctx[1], ctx[3], ctx[4] (skipping ctx[2] = center) as the
 * 4-element target context, matching what FUN_08e88de0
 * (USelNetworkSlice::all_half_phone_costs) reads from `this+4`, `+8`,
 * `+0x10`, `+0x14` when computing the term `local_3c + ... + local_4c`.
 * When NULL, falls back to the chosen-UID-as-target proxy for ctx.
 *
 * If `out_bd` is non-NULL, populates per-component costs there.
 *
 * Returns f32 (long-double accumulator inside the four cost functions). */
static float score_candidate(const score_ctx_t        *sc,
                             const cart_targets_t     *tgt,
                             const uint32_t           *slice_ctx,
                             const sp_target_t        *sp_tgt,
                             const hp_remap_t         *hp_remap,
                             const s_ctx_remap_t      *s_remap,
                             const voicing_gate_t     *voicing,
                             const spfy_unit_record_t *cand,
                             cost_breakdown_t         *out_bd)
{
    float durt_mean, durt_scale;
    if (tgt && tgt->has_durt) {
        durt_mean  = tgt->durt_mean;
        durt_scale = tgt->durt_var;     /* engine uses var as 1/stddev */
    } else {
        durt_mean  = (float)sc->target.f0_context;
        durt_scale = 0.1f;
    }
    /* IMPORTANT (M3.4g): The engine's "D-cost" formula in
     * USelNetworkSlice::all_half_phone_costs reads cand byte at mem+0x12,
     * which the in-memory unit record layout probe (this session) showed
     * to be `f0_context`, NOT `dur_like`. Despite the engine's parameter
     * being named DUR_WEIGHT in the VCF, this term is scored against
     * f0_context. The naming mismatch is in the engine -- DUR_WEIGHT
     * mostly likely refers to a duration-sensitive prosody decision,
     * not a literal duration delta. Use f0_context here. */
    float D = spfy_cost_d((uint32_t)cand->f0_context,
                          durt_mean, durt_scale, sc->dur_w);

    /* Engine voicing-flag gate (M3.4q, FUN_08e88de0):
     *
     *   if (voice+0x5fc[slice.ctx[2]] == 0 && weight+0x8c == 0):
     *       local_24 = 0  // skip f0tr; F0 cost = 0 for ALL cands at slot
     *
     * The cart_walks trace can mis-attribute f0tr walks to a slot that
     * the engine actually skipped (the cart_walks_hook tags walks with
     * the LAST InnerScorer's slot param, but FUN_08e87e10 may also be
     * called from BuildGraph or PostScoringAdj for unrelated slots).
     * So `tgt->has_f0tr` is unreliable for this gate; use the engine's
     * own gate inputs. */
    int voicing_skip = 0;
    if (voicing && voicing->has && slice_ctx &&
        voicing->weight_8c == 0u) {
        uint32_t ctx2 = slice_ctx[2];
        if (ctx2 < voicing->n_entries &&
            voicing->by_hp_class[ctx2] == 0u) {
            voicing_skip = 1;
        }
    }

    float F0;
    if (voicing_skip) {
        F0 = 0.0f;
    } else if (tgt && tgt->has_f0tr) {
        F0 = spfy_cost_f0((uint32_t)cand->f0_start,
                          tgt->f0tr_mean, tgt->f0tr_var,
                          sc->f0_w, sc->miss_f0);
    } else if (tgt) {
        /* No engine voicing capture, no f0tr leaf for this slot. Most
         * commonly silence / unvoiced. F0 = 0 conservatively. */
        F0 = 0.0f;
    } else {
        F0 = spfy_cost_f0((uint32_t)cand->f0_start,
                          (float)sc->target.f0_start, 0.1f,
                          sc->f0_w, sc->miss_f0);
    }

    spfy_sp_matrix_t sp_views[5];
    for (int k = 0; k < 5; ++k) {
        sp_views[k].data   = sc->pmats[k].data;
        sp_views[k].n_rows = sc->pmats[k].n_rows;
        sp_views[k].n_cols = sc->pmats[k].n_cols;
    }
    /* Target SP indices: prefer engine capture, fall back to chosen-as-
     * target proxy. The capture order matches InnerScorer's cost-formula
     * order (matrix 0 row, ..., matrix 4 row), which is *not* the same as
     * our cost.h doc strings -- those used a sp_syl_in_phrase / sp_syl_type
     * / sp_word_in_phrase / sp_syl_in_word / phoneInSyl naming for the
     * candidate columns. The cost is symmetric in the sense that both
     * sides index the same matrix; the row/column slots just need to be
     * consistent. */
    uint32_t sp_tgt_idx[5];
    if (sp_tgt && sp_tgt->has) {
        for (int k = 0; k < 5; ++k) {
            sp_tgt_idx[k] = (sp_tgt->sp[k] == 0xFFFFFFFFu)
                          ? 0u : sp_tgt->sp[k];
        }
    } else {
        sp_tgt_idx[0] = sc->target.sp_syl_in_phrase;
        sp_tgt_idx[1] = sc->target.sp_syl_type;
        sp_tgt_idx[2] = sc->target.sp_word_in_phrase;
        sp_tgt_idx[3] = sc->target.sp_syl_in_word;
        sp_tgt_idx[4] = 6u;
    }
    uint32_t sp_cnd[5] = {
        cand->sp_syl_in_phrase, cand->sp_syl_type,
        cand->sp_word_in_phrase, cand->sp_syl_in_word,
        cand->sp_phone_in_syl   /* 6 unless the record is v100008 */
    };
    float SP = spfy_cost_sp(sp_views, sp_tgt_idx, sp_cnd, sc->w_sp);

    /* hp_class indexes the ccos forest. The ccos forest layout uses
     * "left-first-then-right" hp_class order (0..n_labels-1 are LEFT
     * halves, n_labels..2*n_labels-1 are RIGHT). The engine's
     * slice.ctx[2] is in "interleaved phone*2+side" order (with two
     * phone-pair swaps in Tom: phones 9/10/11 and 14/15).
     *
     * Prefer slice.ctx[2] (engine-truth halfphone class) over the
     * chosen-UID-derived computation. With the captured `hp_remap` we
     * use it directly; without it we fall through to our (Tom-swap-
     * patched) `maps->hp_class[]` which has the same semantics. */
    uint32_t hp_class;
    if (slice_ctx) {
        if (hp_remap && hp_remap->has &&
            slice_ctx[2] < hp_remap->n_entries) {
            hp_class = hp_remap->map[slice_ctx[2]];
        } else if (slice_ctx[2] < sc->maps->n_hp_entries) {
            hp_class = sc->maps->hp_class[slice_ctx[2]];
        } else {
            hp_class = 0;
        }
        if (hp_class >= sc->ccos->n_hp_classes) hp_class = 0;
    } else {
        uint32_t hp_class_idx = (uint32_t)sc->target.phone_center * 2u +
                                (sc->target.is_first_half ? 0u : 1u);
        hp_class = (hp_class_idx < sc->maps->n_hp_entries)
                   ? sc->maps->hp_class[hp_class_idx] : 0;
    }

    /* Target context. With slice_ctx we use the engine-captured 5-tuple
     * (ctx[0,1,3,4] -- skipping center ctx[2]). The slice values are
     * halfphone classes in [0..92] (Tom: 46 phones * 2 sides + 1 silence
     * sentinel). We need to convert each to a label index for ccos row
     * lookup. The engine uses voice+0x604 as a remap table; without that
     * capture we fall back to ctx >> 1 (paired-halfphone -> phone_id
     * for Tom). The remap version is more accurate -- it applies the
     * 4 phone-pair swaps Tom has (phones 9/10/11 and 14/15) so e.g.
     * `slice.ctx=31` maps to label 14 (engine) instead of 15 (>>1). */
    uint8_t target_ctx[4];
    if (slice_ctx && s_remap && s_remap->has &&
        slice_ctx[0] < s_remap->n_entries &&
        slice_ctx[1] < s_remap->n_entries &&
        slice_ctx[3] < s_remap->n_entries &&
        slice_ctx[4] < s_remap->n_entries) {
        target_ctx[0] = (uint8_t)(s_remap->map[slice_ctx[0]] & 0xFFu);
        target_ctx[1] = (uint8_t)(s_remap->map[slice_ctx[1]] & 0xFFu);
        target_ctx[2] = (uint8_t)(s_remap->map[slice_ctx[3]] & 0xFFu);
        target_ctx[3] = (uint8_t)(s_remap->map[slice_ctx[4]] & 0xFFu);
    } else if (slice_ctx) {
        target_ctx[0] = (uint8_t)((slice_ctx[0] >> 1) & 0xFFu);
        target_ctx[1] = (uint8_t)((slice_ctx[1] >> 1) & 0xFFu);
        target_ctx[2] = (uint8_t)((slice_ctx[3] >> 1) & 0xFFu);
        target_ctx[3] = (uint8_t)((slice_ctx[4] >> 1) & 0xFFu);
    } else {
        target_ctx[0] = sc->target.phone_ctx[0];
        target_ctx[1] = sc->target.phone_ctx[1];
        target_ctx[2] = sc->target.phone_ctx[2];
        target_ctx[3] = sc->target.phone_ctx[3];
    }
    float S = spfy_cost_s(sc->ccos->tables, sc->ccos->n_labels, hp_class,
                          sc->maps->L, sc->maps->n_labels,
                          target_ctx, cand->phone_ctx, sc->ccos_w);

    /* Flag term: engine adds `cand.byte+0x17 * weight_0x38 * c` where
     * weight_0x38 = 0.25 (captured at slice+0x24+0x38) and c = 0.01
     * (global _DAT_08e98580). cand.byte+0x17 = context_cost. Almost
     * always zero in Tom; included for completeness. */
    float FLAG = (float)cand->context_cost * 0.25f * 0.01f;

    if (out_bd) {
        out_bd->D    = D;
        out_bd->F0   = F0;
        out_bd->SP   = SP;
        out_bd->S    = S;
        out_bd->FLAG = FLAG;
    }
    return D + F0 + SP + S + FLAG;
}

/* ------------------------------------------------------------------ */
/* Slot building                                                       */
/* ------------------------------------------------------------------ */

/* Halfphone-class encoding: the engine's PRSL key uses phone_center*2 +
 * side (0 = first/left half, 1 = second/right half). This matches the
 * 0..92 range observed in dump_voice --prsl output. */
static uint32_t hp_class_of(const spfy_unit_record_t *u)
{
    return (uint32_t)u->phone_center * 2u + (u->is_first_half ? 0u : 1u);
}

/* Try to look up a PRSL pool for slot i. We try the halfphone-class
 * triphone encoding first; on miss we try phone-center triphone (some
 * voices key differently). On either hit, *out_cands / *out_n is set. */
static int prsl_try_lookup(const spfy_prsl_t *prsl,
                           const spfy_unit_record_t *l,
                           const spfy_unit_record_t *c,
                           const spfy_unit_record_t *r,
                           const uint32_t **out_cands,
                           uint32_t *out_n,
                           uint32_t *used_key)
{
    uint32_t key_hp = spfy_prsl_context_key(hp_class_of(l), hp_class_of(c),
                                            hp_class_of(r));
    int rc = spfy_prsl_lookup(prsl, key_hp, out_cands, out_n);
    if (rc == SPFY_OK) { *used_key = key_hp; return SPFY_OK; }

    uint32_t key_ph = spfy_prsl_context_key((uint32_t)l->phone_center,
                                            (uint32_t)c->phone_center,
                                            (uint32_t)r->phone_center);
    rc = spfy_prsl_lookup(prsl, key_ph, out_cands, out_n);
    if (rc == SPFY_OK) { *used_key = key_ph; return SPFY_OK; }

    *used_key = 0;
    return SPFY_E_OOB;
}

/* ------------------------------------------------------------------ */
/* One-utterance replay                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    long n_utts;
    long n_slots_total;
    long n_slots_prsl_hit;
    long n_slots_chosen_in_pool;
    long n_slots_matched;
    long n_utts_complete;             /* ran to end without DP error */
    long n_slots_pool_matches_capture; /* C-side pool == captured pool */
} stats_t;

static int replay_utterance(const uid_list_t              *chosen,
                            const utt_capture_t           *cap,   /* may be NULL */
                            const utt_targets_t           *tgts,  /* may be NULL */
                            const utt_sp_targets_t        *spts,  /* may be NULL */
                            const spfy_unit_table_t       *units,
                            const spfy_prsl_t             *prsl,
                            const spfy_hash_t             *hash,
                            const spfy_ccos_t             *ccos,
                            const spfy_voice_maps_t       *maps,
                            const spfy_proscost_matrix_t  *pmats,
                            const spfy_vin_t              *vin,    /* plan 02-02: F0-curve loader */
                            float dur_w, float f0_w, float ccos_w,
                            float miss_f0, const float w_sp[5],
                            float join_miss_default,
                            uint32_t utt_idx,
                            stats_t *st)
{
    (void)join_miss_default;
    /* Build the slot list. Two modes:
     *   - With a captured prsl_slot trace: keep ALL chosen UIDs as slots
     *     (the engine emits PRSL slots for boundary uids too).
     *   - Without: drop uid=0 / oob entries since we'd have no way to
     *     derive a meaningful triphone for them.
     * Also drop UIDs we cannot resolve in the unit table either way. */
    uint32_t *slot_uids = (uint32_t *)calloc(chosen->n, sizeof *slot_uids);
    spfy_unit_record_t *recs = (spfy_unit_record_t *)
        calloc(chosen->n, sizeof *recs);
    if (!slot_uids || !recs) { free(slot_uids); free(recs); return -1; }

    uint32_t n_slots = 0;
    for (uint32_t i = 0; i < chosen->n; ++i) {
        uint32_t u = chosen->uids[i];
        if (cap == NULL) {
            if (u >= units->n_units) continue;
            if (u == 0) continue;
        } else {
            /* With captures we keep boundary slots; we still skip OOB. */
            if (u >= units->n_units) {
                slot_uids[n_slots] = u;
                memset(&recs[n_slots], 0, sizeof recs[n_slots]);
                n_slots++;
                continue;
            }
        }
        spfy_unit_record_t r;
        if (spfy_unit_record_get(units, u, &r) != SPFY_OK) {
            if (cap == NULL) continue;
            memset(&r, 0, sizeof r);
        }
        slot_uids[n_slots] = u;
        recs[n_slots]      = r;
        n_slots++;
    }
    if (n_slots < 2) {           /* need at least 2 slots for a join test */
        free(slot_uids); free(recs);
        return 0;
    }
    /* If we have captures but counts disagree (rare -- e.g. truncated
     * trace), fall back to neighbor-derived ctx so the run still completes
     * with sensible numbers. */
    if (cap != NULL && cap->n != n_slots) {
        fprintf(stderr,
                "  utt %u: prsl_slot count %u != wsola slot count %u; "
                "falling back to heuristic ctx for this utterance\n",
                utt_idx, cap->n, n_slots);
        cap = NULL;
    }

    /* Build per-slot candidate pools and target-cost arrays. */
    spfy_viterbi_slot_t *slots = (spfy_viterbi_slot_t *)
        calloc(n_slots, sizeof *slots);
    /* Owning copies of cand arrays + target_cost (since PRSL pools alias
     * VIN buffer; we make a copy because we may need to inject the chosen
     * UID if it's missing). */
    uint32_t **cand_buf = (uint32_t **)calloc(n_slots, sizeof *cand_buf);
    float    **tc_buf   = (float    **)calloc(n_slots, sizeof *tc_buf);
    /* Plan 02-02 — per-cand engine F0-state bytes for the DAG join cost.
     * c68 = unit_mem+0x11 (f0_mid)   — gate's run-length reset threshold
     * c6c = unit_mem+0x10 (f0_end)   — passed as curr_c6c to dag_join_cb
     * c70 = unit_mem+0x0f (f0_start) — stored on cand+0x70
     * c78 = unit_mem+0x0f (f0_start) — gate's smooth-miss counter seed
     * (see spfy/src/usel/viterbi.h spfy_viterbi_dag_slot_t comments). */
    uint8_t  **cand_c68 = (uint8_t **)calloc(n_slots, sizeof *cand_c68);
    uint8_t  **cand_c6c = (uint8_t **)calloc(n_slots, sizeof *cand_c6c);
    uint8_t  **cand_c70 = (uint8_t **)calloc(n_slots, sizeof *cand_c70);
    uint8_t  **cand_c78 = (uint8_t **)calloc(n_slots, sizeof *cand_c78);
    if (!slots || !cand_buf || !tc_buf
        || !cand_c68 || !cand_c6c || !cand_c70 || !cand_c78) {
        free(slots); free(cand_buf); free(tc_buf);
        free(cand_c68); free(cand_c6c); free(cand_c70); free(cand_c78);
        free(slot_uids); free(recs);
        return -1;
    }

    long u_slots_prsl_hit = 0, u_slots_chosen_in_pool = 0;
    long u_slots_pool_matches_capture = 0;

    for (uint32_t s = 0; s < n_slots; ++s) {
        const uint32_t *pcands   = NULL;
        uint32_t        pn       = 0;
        uint32_t        used_key = 0;
        int             rc_p     = SPFY_E_OOB;

        if (cap != NULL) {
            /* Use the engine's captured ctx[5] from the prsl_slot trace.
             * The decoded encoding is:
             *     key = ctx[1] * 10000 + ctx[2] * 100 + ctx[3]
             * where ctx[1..3] are halfphone classes at positions slot-2,
             * slot, slot+2 (same-side triphone). */
            const slot_capture_t *cs = &cap->slots[s];
            used_key = spfy_prsl_context_key(cs->ctx[1], cs->ctx[2],
                                             cs->ctx[3]);
            rc_p = spfy_prsl_lookup(prsl, used_key, &pcands, &pn);
        } else {
            const spfy_unit_record_t *L = (s == 0)         ? &recs[s]
                                                           : &recs[s-1];
            const spfy_unit_record_t *C = &recs[s];
            const spfy_unit_record_t *R = (s+1 == n_slots) ? &recs[s]
                                                           : &recs[s+1];
            rc_p = prsl_try_lookup(prsl, L, C, R, &pcands, &pn, &used_key);
        }

        /* Decide candidate set. Always include the chosen UID (so the test
         * can run; if it isn't in PRSL output we'll log it). */
        int chosen_in_pool = 0;
        if (rc_p == SPFY_OK) {
            u_slots_prsl_hit++;
            for (uint32_t i = 0; i < pn; ++i)
                if (pcands[i] == slot_uids[s]) { chosen_in_pool = 1; break; }
            if (chosen_in_pool) u_slots_chosen_in_pool++;

            /* If we have a captured pool, sanity check: same UIDs (any
             * order, but engine emits in order so first-N comparison is
             * usually exact). */
            if (cap != NULL) {
                const slot_capture_t *cs = &cap->slots[s];
                if (cs->n_cands == pn) {
                    int eq = 1;
                    for (uint32_t i = 0; i < pn; ++i) {
                        if (cs->uids[i] != pcands[i]) { eq = 0; break; }
                    }
                    if (eq) u_slots_pool_matches_capture++;
                }
            }
        }

        /* Same-recording continuation: for s>0, the engine may select
         * `chosen[s-1] + 1` directly without going through PRSL. Augment
         * the pool with that uid (if not already present and in range). */
        uint32_t prev_plus_one = 0;
        int      add_prev_plus_one = 0;
        if (s > 0) {
            uint32_t pp = slot_uids[s-1] + 1u;
            if (pp < units->n_units) {
                int present = 0;
                if (rc_p == SPFY_OK) {
                    for (uint32_t i = 0; i < pn; ++i)
                        if (pcands[i] == pp) { present = 1; break; }
                }
                if (!present && pp != slot_uids[s]) {
                    prev_plus_one = pp;
                    add_prev_plus_one = 1;
                }
            }
        }

        uint32_t cap_n = pn
                      + (chosen_in_pool ? 0u : 1u)
                      + (add_prev_plus_one ? 1u : 0u);
        if (cap_n == 0) cap_n = 1;
        cand_buf[s] = (uint32_t *)calloc(cap_n, sizeof *cand_buf[s]);
        tc_buf[s]   = (float    *)calloc(cap_n, sizeof *tc_buf[s]);
        cand_c68[s] = (uint8_t  *)calloc(cap_n, sizeof *cand_c68[s]);
        cand_c6c[s] = (uint8_t  *)calloc(cap_n, sizeof *cand_c6c[s]);
        cand_c70[s] = (uint8_t  *)calloc(cap_n, sizeof *cand_c70[s]);
        cand_c78[s] = (uint8_t  *)calloc(cap_n, sizeof *cand_c78[s]);
        if (!cand_buf[s] || !tc_buf[s]
            || !cand_c68[s] || !cand_c6c[s] || !cand_c70[s] || !cand_c78[s])
            goto fail_inner;

        uint32_t k = 0;
        if (rc_p == SPFY_OK) {
            for (uint32_t i = 0; i < pn; ++i) cand_buf[s][k++] = pcands[i];
        }
        if (!chosen_in_pool)   cand_buf[s][k++] = slot_uids[s];
        if (add_prev_plus_one) cand_buf[s][k++] = prev_plus_one;

        /* Set up the scoring context. The 'target' field of score_ctx is
         * still recs[s] so SP and S costs use the chosen-UID-as-target
         * proxy (no engine capture for those features yet). D and F0 will
         * use the captured cart_walks leaves below if available. Weights
         * are overridden by the inner_scorer-captured engine values when
         * present (engine reads them from slice+0x24+offset). */
        score_ctx_t sc = {0};
        sc.units   = units;
        sc.maps    = maps;
        sc.ccos    = ccos;
        sc.pmats   = pmats;
        sc.dur_w   = (spts && spts->weights.has)
                     ? spts->weights.d : dur_w;
        sc.f0_w    = (spts && spts->weights.has)
                     ? spts->weights.f0 : f0_w;
        sc.ccos_w  = (spts && spts->weights.has)
                     ? spts->weights.ccos : ccos_w;
        sc.miss_f0 = (spts && spts->weights.has)
                     ? spts->weights.miss_f0 : miss_f0;
        for (int j = 0; j < 5; ++j) {
            sc.w_sp[j] = (spts && spts->weights.has)
                         ? spts->weights.sp[j] : w_sp[j];
        }
        sc.target  = recs[s];

        const cart_targets_t *tgt =
            (tgts && s < tgts->n) ? &tgts->slots[s] : NULL;
        const uint32_t *slice_ctx =
            (cap && s < cap->n) ? cap->slots[s].ctx : NULL;
        const sp_target_t *sp_tgt =
            (spts && s < spts->n && spts->slots[s].has)
                ? &spts->slots[s] : NULL;
        const hp_remap_t *hp_remap =
            (spts && spts->hp_remap.has) ? &spts->hp_remap : NULL;
        const s_ctx_remap_t *s_remap =
            (spts && spts->s_remap.has) ? &spts->s_remap : NULL;
        const voicing_gate_t *voicing =
            (spts && spts->voicing.has) ? &spts->voicing : NULL;

        for (uint32_t i = 0; i < k; ++i) {
            uint32_t cu = cand_buf[s][i];
            spfy_unit_record_t cr;
            if (spfy_unit_record_get(units, cu, &cr) != SPFY_OK) {
                tc_buf[s][i] = -1.0f;     /* forbidden */
                /* leave c68/c6c/c70/c78 = 0 (calloc) for forbidden cands */
                continue;
            }
            /* Plan 02-02: per-cand F0-state bytes for dag_join_cb.
             * Engine reads these from in-mem unit struct at cand+0x68 etc;
             * derived from disk fields per spfy_synth.c:1396-1401. */
            cand_c6c[s][i] = cr.f0_end;
            cand_c68[s][i] = cr.f0_mid;
            cand_c70[s][i] = cr.f0_start;
            cand_c78[s][i] = cr.f0_start;
            cost_breakdown_t bd;
            tc_buf[s][i] = score_candidate(&sc, tgt, slice_ctx, sp_tgt,
                                           hp_remap, s_remap, voicing,
                                           &cr, &bd);
            if (getenv("SPFY_DUMP_COMPONENTS")) {
                fprintf(stdout, "COMP utt=%u slot=%u uid=%u "
                        "D=%g F0=%g SP=%g S=%g FLAG=%g tc=%g\n",
                        utt_idx, s, cu,
                        (double)bd.D, (double)bd.F0, (double)bd.SP,
                        (double)bd.S, (double)bd.FLAG,
                        (double)tc_buf[s][i]);
            }
            /* SPFY_TRACE_CAND="utt:slot:uid" prints exhaustive per-cand
             * cost decomposition to stderr. */
            const char *trc = getenv("SPFY_TRACE_CAND");
            if (trc) {
                uint32_t tu, ts, tuid;
                if (sscanf(trc, "%u:%u:%u", &tu, &ts, &tuid) == 3 &&
                    tu == utt_idx && ts == s && tuid == cu) {
                    fprintf(stderr, "TRACE %u/%u uid=%u:\n"
                            "  cand: phone_center=%u is_first_half=%u "
                            "phone_ctx=[%u,%u,%u,%u] flag_b=%u ctx_cost=%u\n"
                            "        f0_start=%u f0_end=%u f0_mid=%u "
                            "f0_context=%u dur_like=%u\n"
                            "        sp=[%u,%u,%u,%u]\n"
                            "  slice.ctx = [%u,%u,%u,%u,%u]\n"
                            "  sp_target = [%u,%u,%u,%u,%u]\n"
                            "  cart_durt mean=%g var=%g  cart_f0tr "
                            "mean=%g var=%g\n"
                            "  weights: D=%g F0=%g CCOS=%g missF0=%g "
                            "SP=[%g,%g,%g,%g,%g]\n"
                            "  components: D=%g F0=%g SP=%g S=%g FLAG=%g\n"
                            "  TOTAL = %g\n",
                            utt_idx, s, cu,
                            cr.phone_center, cr.is_first_half,
                            cr.phone_ctx[0], cr.phone_ctx[1],
                            cr.phone_ctx[2], cr.phone_ctx[3],
                            cr.flag_b, cr.context_cost,
                            cr.f0_start, cr.f0_end, cr.f0_mid,
                            cr.f0_context, cr.dur_like,
                            cr.sp_syl_in_phrase, cr.sp_syl_type,
                            cr.sp_word_in_phrase, cr.sp_syl_in_word,
                            slice_ctx ? slice_ctx[0] : 0,
                            slice_ctx ? slice_ctx[1] : 0,
                            slice_ctx ? slice_ctx[2] : 0,
                            slice_ctx ? slice_ctx[3] : 0,
                            slice_ctx ? slice_ctx[4] : 0,
                            sp_tgt ? sp_tgt->sp[0] : 0,
                            sp_tgt ? sp_tgt->sp[1] : 0,
                            sp_tgt ? sp_tgt->sp[2] : 0,
                            sp_tgt ? sp_tgt->sp[3] : 0,
                            sp_tgt ? sp_tgt->sp[4] : 0,
                            tgt && tgt->has_durt ? (double)tgt->durt_mean : 0,
                            tgt && tgt->has_durt ? (double)tgt->durt_var  : 0,
                            tgt && tgt->has_f0tr ? (double)tgt->f0tr_mean : 0,
                            tgt && tgt->has_f0tr ? (double)tgt->f0tr_var  : 0,
                            (double)sc.dur_w, (double)sc.f0_w,
                            (double)sc.ccos_w, (double)sc.miss_f0,
                            (double)sc.w_sp[0], (double)sc.w_sp[1],
                            (double)sc.w_sp[2], (double)sc.w_sp[3],
                            (double)sc.w_sp[4],
                            (double)bd.D, (double)bd.F0, (double)bd.SP,
                            (double)bd.S, (double)bd.FLAG,
                            (double)tc_buf[s][i]);
                }
            }
        }
        slots[s].cands       = cand_buf[s];
        slots[s].target_cost = tc_buf[s];
        slots[s].n_cands     = k;
    }

    /* Run Viterbi. Plan 02-02 (THIRD scope revision) — switched from
     * the legacy 3-arg `spfy_viterbi_run` (flat miss_default) to the
     * engine-faithful DAG variant `spfy_viterbi_run_dag` with
     * `dag_join_cb` reproducing FUN_08e8b620's CCOS-gate hash-miss
     * formula. Closes the 81.4% DP-on-engine-inputs UID ceiling.
     *
     * Tom values per spfy_synth.c::main:
     *   F0_EDGE_CHANGE_WEIGHT = 0.6f  (VCF)
     *   MISSING_JOIN_COST     = 1000.0f (FE-init default param_3[0x21])
     *   curve / max_idx / sub_off : loaded from VIN `hist` chunk.
     *
     * Diagnostics:
     *   SPFY_JOIN_MISS=<f>   override miss_default (legacy fallback only)
     *   SPFY_NO_F0_CURVE=1   disable curve, fall back to legacy miss path
     *   SPFY_LEGACY_VIT=1    use 3-arg join_cb / spfy_viterbi_run (debug). */
    float miss_default = 1000.0f;
    {
        const char *e = getenv("SPFY_JOIN_MISS");
        if (e) miss_default = (float)atof(e);
    }
    join_ctx_t jc;
    jc.hash                  = hash;
    jc.units                 = units;
    jc.miss_default          = miss_default;
    load_f0_hist_curve(vin, &jc);
    jc.f0_edge_change_weight = 0.6f;
    jc.missing_join_cost     = 1000.0f;
    if (getenv("SPFY_NO_F0_CURVE")) jc.curve = NULL;

    uint32_t *path  = (uint32_t *)calloc(n_slots, sizeof *path);
    float    total  = 0.0f;
    int      rc_v;
    if (getenv("SPFY_LEGACY_VIT")) {
        /* Legacy linear DP for diagnostic A/B against the DAG path. */
        rc_v = spfy_viterbi_run(slots, n_slots, join_cb, &jc, path, &total);
    } else {
        /* DAG-with-CCOS-gate. Build a per-slot pred list of length 1
         * (= [s-1]) for s>0 and zero-length for s=0. The DAG variant
         * forwards per-cand c7c/c80 along the chosen pred path. */
        spfy_viterbi_dag_slot_t *dag_slots = (spfy_viterbi_dag_slot_t *)
            calloc(n_slots, sizeof *dag_slots);
        uint32_t *pred_cells = (uint32_t *)
            calloc(n_slots ? n_slots : 1, sizeof *pred_cells);
        if (!dag_slots || !pred_cells) {
            free(dag_slots); free(pred_cells); free(path);
            goto fail_inner;
        }
        for (uint32_t s = 0; s < n_slots; ++s) {
            pred_cells[s] = (s == 0) ? 0u : (s - 1u);
            dag_slots[s].cands       = cand_buf[s];
            dag_slots[s].join_keys   = cand_buf[s];   /* HP leaf: uid == join_key */
            dag_slots[s].target_cost = tc_buf[s];
            dag_slots[s].n_cands     = slots[s].n_cands;
            dag_slots[s].preds       = (s == 0) ? NULL : &pred_cells[s];
            dag_slots[s].n_preds     = (s == 0) ? 0u : 1u;
            dag_slots[s].c68         = cand_c68[s];
            dag_slots[s].c6c         = cand_c6c[s];
            dag_slots[s].c70         = cand_c70[s];
            dag_slots[s].c78         = cand_c78[s];
        }
        uint32_t *path_slots = (uint32_t *)calloc(n_slots, sizeof *path_slots);
        uint32_t  path_len   = 0;
        rc_v = spfy_viterbi_run_dag(dag_slots, n_slots, dag_join_cb, &jc,
                                     path_slots, path, &path_len, &total);
        /* All n_slots are HP leaves so path_len == n_slots on success. */
        if (rc_v == SPFY_OK && path_len != n_slots) {
            fprintf(stderr,
                    "  utt %u: dag path_len=%u != n_slots=%u; downstream "
                    "match counts may be off\n",
                    utt_idx, path_len, n_slots);
        }
        free(path_slots);
        free(dag_slots);
        free(pred_cells);
    }

    long u_matched = 0;
    if (rc_v == SPFY_OK) {
        for (uint32_t s = 0; s < n_slots; ++s)
            if (path[s] == slot_uids[s]) u_matched++;
        st->n_utts_complete++;
    }

    /* Per-cand totals dump (env: SPFY_DUMP_CAND_TOTALS=1). Emits one
     * line per (utt, slot, cand) to stdout for offline diff against the
     * engine's per-cand totals captured via the inner_scorer hook. */
    if (getenv("SPFY_DUMP_CAND_TOTALS")) {
        for (uint32_t s = 0; s < n_slots; ++s) {
            for (uint32_t i = 0; i < slots[s].n_cands; ++i) {
                fprintf(stdout, "CAND_TOTAL utt=%u slot=%u uid=%u tc=%g\n",
                        utt_idx, s, slots[s].cands[i],
                        (double)slots[s].target_cost[i]);
            }
        }
    }

    /* Per-slot mismatch dump (env: SPFY_DEBUG_MISMATCH=1 or =UTT_NUM).
     * For each slot where best_path != chosen, print:
     *   slot s: chosen=UID_C tc=TC_C [pool_idx=I_C]  best=UID_B tc=TC_B [pool_idx=I_B]
     *   diff target = TC_C - TC_B   (positive = chosen worse)
     * The DP-level total cost difference can include join contributions
     * which we don't break out here -- look at the per-slot target cost
     * gap to find systematic component scoring bugs. */
    const char *dbg_env = getenv("SPFY_DEBUG_MISMATCH");
    int dbg_enabled = (dbg_env != NULL) &&
                      (atoi(dbg_env) == 0 ||
                       (uint32_t)atoi(dbg_env) == utt_idx);
    if (rc_v == SPFY_OK && dbg_enabled) {
        for (uint32_t s = 0; s < n_slots; ++s) {
            if (path[s] == slot_uids[s]) continue;     /* match -- skip */
            /* Find pool indices for chosen and best. */
            int     ic = -1, ib = -1;
            for (uint32_t i = 0; i < slots[s].n_cands; ++i) {
                if (slots[s].cands[i] == slot_uids[s]) ic = (int)i;
                if (slots[s].cands[i] == path[s])      ib = (int)i;
            }
            float tc_c = (ic >= 0) ? slots[s].target_cost[ic] : 1e30f;
            float tc_b = (ib >= 0) ? slots[s].target_cost[ib] : 1e30f;
            spfy_unit_record_t rc_chosen, rc_best;
            (void)spfy_unit_record_get(units, slot_uids[s], &rc_chosen);
            (void)spfy_unit_record_get(units, path[s],      &rc_best);
            fprintf(stderr,
                "  utt %u slot %2u: chosen=%6u (ph=%u dur=%u f0c=%u sp=%u/%u/%u/%u) tc=%.3f"
                "  best=%6u (ph=%u dur=%u f0c=%u sp=%u/%u/%u/%u) tc=%.3f"
                "  diff=%+.3f  pool_n=%u\n",
                utt_idx, s,
                slot_uids[s], rc_chosen.phone_center, rc_chosen.dur_like,
                rc_chosen.f0_context,
                rc_chosen.sp_syl_in_phrase, rc_chosen.sp_syl_type,
                rc_chosen.sp_word_in_phrase, rc_chosen.sp_syl_in_word,
                (double)tc_c,
                path[s], rc_best.phone_center, rc_best.dur_like,
                rc_best.f0_context,
                rc_best.sp_syl_in_phrase, rc_best.sp_syl_type,
                rc_best.sp_word_in_phrase, rc_best.sp_syl_in_word,
                (double)tc_b,
                (double)(tc_c - tc_b),
                slots[s].n_cands);
        }
    }

    /* Per-utterance summary line. */
    fprintf(stdout,
        "utt %3u: slots=%2u  prsl=%2ld  chosen_in_pool=%2ld  "
        "pool_eq_cap=%2ld  matched=%2ld  rc=%s  cost=%.3f\n",
        utt_idx, n_slots, u_slots_prsl_hit, u_slots_chosen_in_pool,
        u_slots_pool_matches_capture, u_matched,
        rc_v == SPFY_OK ? "ok" : spfy_strerror(rc_v),
        (double)total);

    /* Per-phrase JSONL emit for plan 02-06 Task 2 / D-12 schema. Gated
     * on SPFY_VR_JSONL_OUT env var; the corpus driver sets SPFY_VR_ID
     * and SPFY_VR_MODE per phrase. n_uid_match is the canonical integer
     * field the corpus aggregate sums directly to avoid float round-trip
     * through uid_match_pct (per plan 02-06 Threat T-02-26). */
    {
        const char *jsonl_out = getenv("SPFY_VR_JSONL_OUT");
        if (jsonl_out && *jsonl_out) {
            const char *id   = getenv("SPFY_VR_ID");
            const char *mode = getenv("SPFY_VR_MODE");
            FILE *jp = fopen(jsonl_out, "ab");
            if (jp) {
                double pct = n_slots
                    ? 100.0 * (double)u_matched / (double)n_slots
                    : 0.0;
                fprintf(jp,
                    "{\"id\":\"%s\",\"mode\":\"%s\","
                    "\"byte_exact\":null,\"first_diff_byte\":null,"
                    "\"rms_s16\":null,\"sha256_native\":null,"
                    "\"sha256_oracle\":null,"
                    "\"uid_match_pct\":%.4f,"
                    "\"n_uid_match\":%ld,"
                    "\"slot_match_pct\":null,"
                    "\"n_slots\":%u,"
                    "\"stage_first_divergence\":null}\n",
                    id   ? id   : "?",
                    mode ? mode : "?",
                    pct, u_matched, n_slots);
                fclose(jp);
            } else {
                fprintf(stderr,
                    "warning: SPFY_VR_JSONL_OUT=%s open failed\n",
                    jsonl_out);
            }
        }
    }

    st->n_utts++;
    st->n_slots_total                 += (long)n_slots;
    st->n_slots_prsl_hit              += u_slots_prsl_hit;
    st->n_slots_chosen_in_pool        += u_slots_chosen_in_pool;
    st->n_slots_pool_matches_capture  += u_slots_pool_matches_capture;
    st->n_slots_matched               += u_matched;

    free(path);
    int ok = 0;
    goto cleanup;
fail_inner:
    ok = -1;
cleanup:
    for (uint32_t s = 0; s < n_slots; ++s) {
        free(cand_buf[s]); free(tc_buf[s]);
        free(cand_c68[s]); free(cand_c6c[s]);
        free(cand_c70[s]); free(cand_c78[s]);
    }
    free(cand_buf); free(tc_buf);
    free(cand_c68); free(cand_c6c); free(cand_c70); free(cand_c78);
    free(slots);
    free(slot_uids); free(recs);
    return ok;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc < 4 || argc > 7) {
        fprintf(stderr,
            "usage: %s <voice.vin> <voice.vcf> <wsola_buffer.jsonl> "
            "[<prsl_slot.jsonl> [<cart_walks.jsonl> "
            "[<inner_scorer.jsonl>]]]\n",
            argv[0]);
        return 2;
    }

    spfy_vin_t  vin   = {0};
    spfy_vcf_t  vcf   = {0};
    spfy_unit_table_t units = {0};
    spfy_ccos_t       ccos  = {0};
    spfy_voice_maps_t maps  = {0};
    spfy_prsl_t       prsl  = {0};
    spfy_hash_t       hash  = {0};
    spfy_proscost_matrix_t pmats[SPFY_PROSCOST_N];
    memset(pmats, 0, sizeof pmats);
    int rc = 0;

    if ((rc = spfy_vin_load(argv[1], &vin))           != SPFY_OK) goto fail;
    if ((rc = spfy_vcf_load(argv[2], &vcf))           != SPFY_OK) goto fail;
    if ((rc = spfy_unit_table_load(&vin, &units))     != SPFY_OK) goto fail;
    if ((rc = spfy_ccos_load(&vin, &ccos))            != SPFY_OK) goto fail;
    if ((rc = spfy_voice_maps_build(&ccos, &maps))    != SPFY_OK) goto fail;
    if ((rc = spfy_prsl_load(&vin, &prsl))            != SPFY_OK) goto fail;
    if ((rc = spfy_hash_load(&vin, &hash))            != SPFY_OK) goto fail;
    if ((rc = spfy_proscost_load(&vcf, pmats))        != SPFY_OK) goto fail;

    float dur_w   = (float)get_vcf_f64(&vcf, "tts.voiceCfg.DUR_WEIGHT",         0.3);
    float f0_w    = (float)get_vcf_f64(&vcf, "tts.voiceCfg.ABS_F0_WEIGHT",      0.2);
    float ccos_w  = (float)get_vcf_f64(&vcf, "tts.voiceCfg.CONTEXT_COST_WEIGHT",1.0);
    float miss_f0 = (float)get_vcf_f64(&vcf, "tts.voiceCfg.MISSING_F0_COST", 1000.0);
    float join_miss = (float)get_vcf_f64(&vcf, "tts.voiceCfg.JOIN_COST_OFFSET", 0.2);
    /* SP weights: Tom defaults; configurable in VCF too. */
    float w_sp[5] = {0.1f, 0.1f, 0.0f, 0.0f, 0.0f};

    fprintf(stdout, "loaded: %u units, %u prsl groups, %u hash rows / %u cells\n",
            units.n_units, prsl.n_groups, hash.n_rows, hash.n_cells);
    fprintf(stdout, "weights: D=%g F0=%g S=%g sp=[%g,%g,%g,%g,%g] missF0=%g "
            "join_miss=%g\n",
            dur_w, f0_w, ccos_w, w_sp[0], w_sp[1], w_sp[2], w_sp[3], w_sp[4],
            miss_f0, join_miss);

    /* Optional prsl_slot capture. */
    prsl_capture_t pcap = {0};
    int have_cap = 0;
    if (argc >= 5) {
        if (load_prsl_capture(argv[4], &pcap) != 0) {
            fprintf(stderr, "warning: could not load prsl_slot from %s; "
                    "falling back to heuristic context_key\n", argv[4]);
        } else {
            have_cap = 1;
            fprintf(stdout, "prsl_slot capture: %u utterances loaded\n",
                    pcap.n);
        }
    }

    /* Optional cart_walks capture for real per-slot D + F0 targets. */
    cart_capture_t ccap = {0};
    int have_tgts = 0;
    if (argc >= 6) {
        if (load_cart_capture(argv[5], &ccap) != 0) {
            fprintf(stderr, "warning: could not load cart_walks from %s; "
                    "falling back to chosen-UID-as-target proxy\n",
                    argv[5]);
        } else {
            have_tgts = 1;
            fprintf(stdout, "cart_walks capture: %u utterances loaded\n",
                    ccap.n);
        }
    }

    /* Optional inner_scorer capture for real per-slot SP target features. */
    sp_capture_t scap = {0};
    int have_sp = 0;
    if (argc >= 7) {
        if (load_sp_capture(argv[6], &scap) != 0) {
            fprintf(stderr, "warning: could not load inner_scorer from %s; "
                    "falling back to chosen-UID-as-target proxy for SP\n",
                    argv[6]);
        } else {
            have_sp = 1;
            fprintf(stdout, "inner_scorer capture: %u utterances loaded\n",
                    scap.n);
        }
    }

    FILE *fp = fopen(argv[3], "rb");
    if (!fp) { rc = SPFY_E_IO; goto fail; }

    char buf[16384];
    uid_list_t chosen = {0};
    stats_t st = {0};
    uint32_t utt = 0;

    while (fgets(buf, sizeof buf, fp)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
        if (n == 0) continue;
        chosen.n = 0;
        if (parse_units(buf, n, &chosen) != 0) continue;
        if (chosen.n == 0) continue;
        ++utt;
        const utt_capture_t *cap = (have_cap && (utt - 1) < pcap.n)
                                   ? &pcap.utts[utt - 1]
                                   : NULL;
        const utt_targets_t *tgts = (have_tgts && (utt - 1) < ccap.n)
                                    ? &ccap.utts[utt - 1]
                                    : NULL;
        const utt_sp_targets_t *spts = (have_sp && (utt - 1) < scap.n)
                                       ? &scap.utts[utt - 1]
                                       : NULL;
        replay_utterance(&chosen, cap, tgts, spts, &units, &prsl, &hash,
                         &ccos, &maps, pmats, &vin, dur_w, f0_w, ccos_w,
                         miss_f0, w_sp, join_miss, utt, &st);
    }
    fclose(fp);
    free(chosen.uids);
    prsl_capture_free(&pcap);
    cart_capture_free(&ccap);
    sp_capture_free(&scap);

    fprintf(stdout, "\n----- aggregate -----\n");
    fprintf(stdout, "utterances           : %ld  (complete: %ld)\n",
            st.n_utts, st.n_utts_complete);
    fprintf(stdout, "slots total          : %ld\n", st.n_slots_total);
    /* Percentages cast their long counters to double explicitly: on 64-bit
     * hosts `long` is 64-bit and GCC's -Wconversion flags the implicit
     * widening as possibly lossy. These are slot/call tallies that never
     * approach 2^53, so the cast is exact — it just states the intent. */
    fprintf(stdout, "slots PRSL hit       : %ld  (%.1f%%)\n",
            st.n_slots_prsl_hit,
            st.n_slots_total
            ? 100.0 * (double)st.n_slots_prsl_hit / (double)st.n_slots_total : 0);
    fprintf(stdout, "slots chosen-in-pool : %ld  (%.1f%% of slots, %.1f%% of PRSL hits)\n",
            st.n_slots_chosen_in_pool,
            st.n_slots_total
            ? 100.0 * (double)st.n_slots_chosen_in_pool / (double)st.n_slots_total : 0,
            st.n_slots_prsl_hit
            ? 100.0 * (double)st.n_slots_chosen_in_pool / (double)st.n_slots_prsl_hit : 0);
    if (have_cap) {
        fprintf(stdout, "slots pool=capture   : %ld  (%.1f%% of PRSL hits) "
                "[C-side pool exactly equals engine's captured pool]\n",
                st.n_slots_pool_matches_capture,
                st.n_slots_prsl_hit
                ? 100.0 * (double)st.n_slots_pool_matches_capture
                        / (double)st.n_slots_prsl_hit
                : 0);
    }
    fprintf(stdout, "slots matched        : %ld  (%.1f%%)\n",
            st.n_slots_matched,
            st.n_slots_total
            ? 100.0 * (double)st.n_slots_matched / (double)st.n_slots_total : 0);
    /* DAG-path join cost diagnostics (plan 02-02 THIRD scope revision). */
    if (g_dag_total > 0) {
        fprintf(stdout, "dag join calls       : %ld\n", g_dag_total);
        fprintf(stdout, "  same-rec adjacent  : %ld  (%.2f%%)\n",
                g_dag_same_rec,
                100.0 * (double)g_dag_same_rec / (double)g_dag_total);
        fprintf(stdout, "  hash hits          : %ld  (%.2f%%)\n",
                g_dag_hash_hit,
                100.0 * (double)g_dag_hash_hit / (double)g_dag_total);
        fprintf(stdout, "  hash misses        : %ld  (%.2f%%)\n",
                g_dag_miss_total,
                100.0 * (double)g_dag_miss_total / (double)g_dag_total);
        fprintf(stdout, "    of which gate fire: %ld  (%.2f%% of misses)\n",
                g_dag_miss_gate_fire,
                g_dag_miss_total
                  ? 100.0 * (double)g_dag_miss_gate_fire
                          / (double)g_dag_miss_total : 0);
    }

    if (st.n_slots_chosen_in_pool == 0 && st.n_slots_total > 0) {
        fprintf(stdout, "\nNOTE: chosen-in-pool is 0%% -- this means the "
                "context_key encoding I'm trying does\n"
                "not match what the engine uses for PRSL preselection. "
                "The 'matched' number above\n"
                "is therefore a DP-plumbing smoke test only (chosen UID was "
                "injected into each pool\n"
                "and selected against scoring/join). For a real M3.4 acceptance "
                "harness this CLI\n"
                "needs (a) the engine's actual per-slot context_key (probably "
                "via a function-entry\n"
                "Frida hook on the FE-emitted halfphone target struct) and "
                "(b) the engine's actual\n"
                "per-slot target prosody (cart_walks already captures durt+f0tr "
                "leaves -- align by\n"
                "utterance + slot index).\n");
    }

    rc = 0;
fail:
    spfy_proscost_free(pmats);
    spfy_hash_free(&hash);
    spfy_prsl_free(&prsl);
    spfy_voice_maps_free(&maps);
    spfy_ccos_free(&ccos);
    spfy_vcf_free(&vcf);
    spfy_vin_free(&vin);
    if (rc != 0) {
        fprintf(stderr, "error: %s\n", spfy_strerror(rc));
        return 1;
    }
    return 0;
}
