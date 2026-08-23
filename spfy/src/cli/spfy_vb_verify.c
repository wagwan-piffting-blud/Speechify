/* spfy_vb_verify -- structural and cross-chunk verification of a VIN/VDB pair.
 *
 * ⚠ RUN IT ON A VENDOR FIRST. Every check here must pass on tom and jill; one
 * that does not is a wrong check, not a broken voice, and would otherwise send
 * the next session chasing a defect that never existed. `--expect-clean` makes
 * that explicit in scripts.
 *
 * `hash` CONTENTS are deliberately NOT checked here -- spfy_hash_roundtrip is
 * their gate, and the DOMAIN is an open question rather than a correctness
 * one. Its OCCUPANCY is checked: a hash table that does not cover this voice's
 * units is a build failure, not a modelling choice. See the block by `cnts`.
 *
 *   spfy_vb_verify --vin V --vdb D [--src-dir DIR] [--expect-clean]
 */

#include "../vb/vb_io.h"
#include "../vb/vb_chunk.h"
#include "../cart/cart.h"
#include "../voice/ccos.h"
#include "../voice/feat_table.h"
#include "../voice/unit_table.h"
#include "../voice/vdb_lookup.h"
#include "../voice/voice.h"
#include "../usel/prsl.h"
#include "../../include/spfy/spfy.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_pass = 0, g_warn = 0;

static void ck(int ok, const char *what, const char *fmt, ...)
{
    va_list ap;
    char det[512];
    va_start(ap, fmt);
    vsnprintf(det, sizeof det, fmt, ap);
    va_end(ap);
    if (ok) { ++g_pass; printf("  PASS  %-34s %s\n", what, det); }
    else    { ++g_fail; printf("  ⛔FAIL %-34s %s\n", what, det); }
}

static void warn(const char *what, const char *fmt, ...)
{
    va_list ap;
    char det[512];
    va_start(ap, fmt);
    vsnprintf(det, sizeof det, fmt, ap);
    va_end(ap);
    ++g_warn;
    printf("  ⚠ WARN %-34s %s\n", what, det);
}

/* ====================================================================== */
/* ⭐ FIELD CENSUS -- the check for attributes the BUILDER never sets.
 *
 * This whole class of defect is invisible to every other check here. A field
 * the build leaves alone does not produce a malformed container, an
 * out-of-range value or a failed load: it produces a CONSTANT, and a constant
 * is structurally perfect. phoneInSyl sat at 6 for every unit of every build
 * while jill's VCF priced it at .3; durt q5 and q9 routed every unit down one
 * branch. Nothing failed. The voice just had the wrong attributes.
 *
 * ⚠ THE CONTROL IS THE VENDOR, NOT A THRESHOLD. "Constant" is not by itself
 * wrong -- voice_const is legitimately one value. What is wrong is a field
 * that VARIES in a shipped voice and is FROZEN in ours, and only a reference
 * can tell those apart. Absent one this prints the census and asserts
 * nothing, because a rule invented here would be the same guess it is meant
 * to catch. */

typedef struct {
    const char *name;
    uint32_t    distinct;
    uint32_t    mode_val;
    double      mode_pct;
    /* ⚠ An SP byte is a COLUMN INDEX into a proscost matrix. anchor_score
     * DOES bound it (`col >= m->n_cols` -> continue), so an out-of-range
     * value is not a bad read -- it is a FREE PASS: that whole cost dimension
     * silently stops applying to that unit. jill's widest SP matrix is 10
     * columns, so a max above 9 is worth seeing next to the reference's. */
    uint32_t    max_val;
} field_stat_t;

/* Only fields the ENGINE reads. Disk 0x08 (`u08`) and 0x16 (`voice_const`)
 * are deliberately absent: neither appears in spfy_unit_record_t, so the
 * engine never decodes them and leaving them at 0 cannot change a render. */
#define CENSUS_N_FIELDS 14

static const char *CENSUS_NAMES[CENSUS_N_FIELDS] = {
    "dur_like", "sp_syl_in_phrase", "sp_syl_type", "sp_word_in_phrase",
    "sp_syl_in_word", "sp_phone_in_syl", "f0_start", "f0_mid",
    "f0_context", "phone_ctx[1]", "phone_ctx[2]", "is_first_half",
    "flag_b", "context_cost"
};

static uint32_t census_value(const spfy_unit_record_t *r, int k)
{
    switch (k) {
        case 0:  return r->dur_like;
        case 1:  return r->sp_syl_in_phrase;
        case 2:  return r->sp_syl_type;
        case 3:  return r->sp_word_in_phrase;
        case 4:  return r->sp_syl_in_word;
        case 5:  return r->sp_phone_in_syl;
        case 6:  return r->f0_start;
        case 7:  return r->f0_mid;
        case 8:  return r->f0_context;
        case 9:  return r->phone_ctx[1];
        case 10: return r->phone_ctx[2];
        case 11: return r->is_first_half;
        case 12: return r->flag_b;
        default: return r->context_cost;
    }
}

/* Histogram over 16 bits; dur_like and u08 are wider, so they are folded.
 * Folding can only MERGE distinct values, never invent them, so a field this
 * reports as constant is constant. */
static int census_run(const spfy_unit_table_t *ut, field_stat_t *out)
{
    uint32_t *h = (uint32_t *)calloc(65536u, sizeof *h);
    if (!h) return -1;
    for (int k = 0; k < CENSUS_N_FIELDS; ++k) {
        memset(h, 0, 65536u * sizeof *h);
        uint32_t n = 0;
        for (uint32_t u = 0; u < ut->n_units; ++u) {
            spfy_unit_record_t r;
            if (spfy_unit_record_get((spfy_unit_table_t *)ut, u, &r) != SPFY_OK)
                continue;
            ++h[census_value(&r, k) & 0xFFFFu];
            ++n;
        }
        uint32_t d = 0, mv = 0, mc = 0, mx = 0;
        for (uint32_t v = 0; v < 65536u; ++v) {
            if (!h[v]) continue;
            ++d;
            mx = v;
            if (h[v] > mc) { mc = h[v]; mv = v; }
        }
        out[k].name     = CENSUS_NAMES[k];
        out[k].distinct = d;
        out[k].mode_val = mv;
        out[k].max_val  = mx;
        out[k].mode_pct = n ? 100.0 * (double)mc / (double)n : 0.0;
    }
    free(h);
    return 0;
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

int main(int argc, char **argv)
{
    const char *vin_path = NULL, *vdb_path = NULL, *ref_vin = NULL;
    const char *allow_frozen = NULL;
    int expect_clean = 0;
    for (int i = 1; i < argc; ++i) {
        int has = (i + 1 < argc);
        if (!strcmp(argv[i], "--vin") && has) { vin_path = argv[++i]; continue; }
        if (!strcmp(argv[i], "--vdb") && has) { vdb_path = argv[++i]; continue; }
        if (!strcmp(argv[i], "--ref-vin") && has) { ref_vin = argv[++i]; continue; }
        if (!strcmp(argv[i], "--allow-frozen") && has) { allow_frozen = argv[++i]; continue; }
        if (!strcmp(argv[i], "--expect-clean")) { expect_clean = 1; continue; }
        fprintf(stderr, "unknown option %s\n", argv[i]);
        return 2;
    }
    if (!vin_path || !vdb_path) {
        fprintf(stderr, "usage: %s --vin V --vdb D [--ref-vin R] "
                "[--allow-frozen A,B] [--expect-clean]\n"
                "  --ref-vin R  a SHIPPED voice to census against. A field\n"
                "               that varies in R and is constant here was\n"
                "               never set by the builder -- the one defect\n"
                "               class every other check is blind to, because\n"
                "               a constant is structurally perfect.\n"
                "  --allow-frozen L   comma list of fields this build has\n"
                "               DECIDED to leave constant; they warn instead\n"
                "               of failing. Naming them on the command line\n"
                "               keeps the accepted set arguable rather than\n"
                "               silently downgraded.\n",
                argv[0]);
        return 2;
    }
    if (allow_frozen && !ref_vin) {
        fprintf(stderr, "refusing: --allow-frozen needs --ref-vin; without a "
                "reference nothing is asserted for it to waive\n");
        return 2;
    }

    spfy_vb_riff rvin, rvdb;
    if (spfy_vb_riff_load(vin_path, &rvin) != SPFY_OK) {
        fprintf(stderr, "cannot load %s\n", vin_path); return 1;
    }
    if (spfy_vb_riff_load(vdb_path, &rvdb) != SPFY_OK) {
        fprintf(stderr, "cannot load %s\n", vdb_path); return 1;
    }
    spfy_vin_t vin = {0};
    spfy_vdb_t vdb = {0};
    if (spfy_vin_load(vin_path, &vin) != SPFY_OK
        || spfy_vdb_load(vdb_path, &vdb) != SPFY_OK) {
        fprintf(stderr, "engine loaders rejected the pair\n"); return 1;
    }
    printf("VIN %s\nVDB %s\n\n", vin_path, vdb_path);

    /* ---------------- container ---------------- */
    printf("== container ==\n");
    static const char *NEED[] = { "feat","unit","cnts","mean","prsl","ckls",
                                  "cklx","durt","f0tr","ccos","hist","vers",
                                  "hash","LIST" };
    size_t missing = 0;
    for (size_t i = 0; i < sizeof NEED / sizeof *NEED; ++i)
        if (!spfy_vb_riff_get(&rvin, NEED[i])) { ++missing;
            printf("        missing chunk %s\n", NEED[i]); }
    ck(missing == 0, "vin has all 14 chunks", "%zu missing", missing);
    ck(spfy_vb_riff_get(&rvdb, "data") && spfy_vb_riff_get(&rvdb, "indx")
       && spfy_vb_riff_get(&rvdb, "fmt "), "vdb has data/indx/fmt", "");

    /* ---------------- fmt ---------------- */
    const spfy_vb_chunk *cf = spfy_vb_riff_get(&rvdb, "fmt ");
    if (cf && cf->n >= 16) {
        uint16_t tag = rd_u16(cf->data), ch = rd_u16(cf->data + 2);
        uint32_t sr = rd_u32(cf->data + 4), bps = rd_u32(cf->data + 8);
        uint16_t ba = rd_u16(cf->data + 12), bits = rd_u16(cf->data + 14);
        ck(tag == 7, "fmt is WAVE_FORMAT_MULAW", "tag=%u", tag);
        ck(ch == 1, "fmt mono", "channels=%u", ch);
        ck(bps == sr * ch * 2u && ba == ch * 2u && bits == 16u,
           "fmt decoded-stream fields", "rate=%u bytes/s=%u align=%u bits=%u",
           sr, bps, ba, bits);
        ck(sr == vdb.sample_rate, "fmt rate matches loader", "%u vs %u",
           sr, vdb.sample_rate);
    }

    /* ---------------- vers / LIST ---------------- */
    const spfy_vb_chunk *cv = spfy_vb_riff_get(&rvin, "vers");
    if (cv && cv->n >= 2) {
        uint16_t ln = rd_u16(cv->data);
        ck((size_t)ln + 2u == cv->n, "vers pstr length", "len=%u chunk=%zu",
           ln, cv->n);
    }
    for (int which = 0; which < 2; ++which) {
        const spfy_vb_chunk *cl = spfy_vb_riff_get(which ? &rvdb : &rvin, "LIST");
        if (!cl || cl->n < 4) continue;
        int okinfo = !memcmp(cl->data, "INFO", 4);
        size_t off = 4, nrec = 0;
        int walked = 1;
        while (off + 8 <= cl->n) {
            uint32_t n = rd_u32(cl->data + off + 4);
            if (off + 8 + n > cl->n) { walked = 0; break; }
            off += 8 + n + (n & 1u);
            ++nrec;
        }
        ck(okinfo && walked, which ? "vdb LIST parses" : "vin LIST parses",
           "%zu INFO records", nrec);
        int sw = 0;
        for (size_t i = 0; i + 12 <= cl->n; ++i)
            if (!memcmp(cl->data + i, "SpeechWorks", 11)) { sw = 1; break; }
        if (sw) warn(which ? "vdb LIST copyright" : "vin LIST copyright",
                     "still claims SpeechWorks");
    }

    /* ---------------- unit / cnts / feat / indx ---------------- */
    printf("\n== inventory ==\n");
    spfy_unit_table_t ut = {0};
    spfy_feat_table_t ft = {0};
    spfy_vdb_lookup_t lk = {0};
    int rc_u = spfy_unit_table_load(&vin, &ut);
    int rc_f = spfy_feat_table_load(&vin, &ft);
    int rc_l = spfy_vdb_lookup_build(&vdb, &lk);
    ck(rc_u == SPFY_OK, "unit table loads", "%u units, stride %u",
       ut.n_units, ut.rec_size);
    ck(rc_f == SPFY_OK, "feat table loads", "%u filenames", ft.n_entries);
    ck(rc_l == SPFY_OK, "vdb lookup builds", "");

    /* ---------------- field census ---------------- */
    if (rc_u == SPFY_OK) {
        printf("\n== field census ==\n");
        field_stat_t ours[CENSUS_N_FIELDS];
        memset(ours, 0, sizeof ours);
        if (census_run(&ut, ours) != 0) {
            warn("field census", "out of memory");
        } else {
            spfy_vin_t rv = {0};
            spfy_unit_table_t rut = {0};
            field_stat_t ref[CENSUS_N_FIELDS];
            int have_ref = 0;
            memset(ref, 0, sizeof ref);
            if (ref_vin) {
                if (spfy_vin_load(ref_vin, &rv) == SPFY_OK
                    && spfy_unit_table_load(&rv, &rut) == SPFY_OK
                    && census_run(&rut, ref) == 0) {
                    have_ref = 1;
                    printf("  reference: %s (v%u, %u units)\n",
                           ref_vin, rut.version, rut.n_units);
                } else {
                    warn("reference census", "cannot read %s", ref_vin);
                }
            }
            printf("  %-18s %8s %6s %7s %6s", "field", "distinct", "mode",
                   "mode%", "max");
            if (have_ref) printf("  | %8s %6s %6s", "ref-dist", "ref-md",
                                 "ref-max");
            printf("\n");
            for (int k = 0; k < CENSUS_N_FIELDS; ++k) {
                printf("  %-18s %8u %6u %6.2f%% %6u", ours[k].name,
                       ours[k].distinct, ours[k].mode_val, ours[k].mode_pct,
                       ours[k].max_val);
                if (have_ref)
                    printf("  | %8u %6u %6u", ref[k].distinct,
                           ref[k].mode_val, ref[k].max_val);
                printf("\n");
            }
            /* ⭐ THE ASSERTION, AND IT NEEDS THE REFERENCE. Constant-ness is
             * only a defect relative to a voice that ships and varies. */
            if (have_ref) {
                for (int k = 0; k < CENSUS_N_FIELDS; ++k) {
                    if (!(ours[k].distinct <= 1 && ref[k].distinct > 1))
                        continue;
                    /* ⚠ ACKNOWLEDGED, NOT SUPPRESSED. --allow-frozen names
                     * the fields a build has DECIDED to leave constant, so
                     * the accepted set is written on the command line where
                     * it can be argued with, instead of being downgraded to a
                     * warning nobody reads. A field not on the list still
                     * fails. */
                    int allowed = 0;
                    if (allow_frozen) {
                        const char *p = strstr(allow_frozen, ours[k].name);
                        size_t ln = strlen(ours[k].name);
                        if (p && (p == allow_frozen || p[-1] == ',')
                            && (p[ln] == '\0' || p[ln] == ','))
                            allowed = 1;
                    }
                    if (allowed)
                        warn("field frozen (allowed)",
                             "%s constant at %u; reference has %u distinct",
                             ours[k].name, ours[k].mode_val, ref[k].distinct);
                    else
                        ck(0, "field is not frozen",
                           "%s constant at %u here, %u distinct values in the "
                           "reference -- the builder never sets it",
                           ours[k].name, ours[k].mode_val, ref[k].distinct);
                }
                ck(1, "field census vs reference",
                   "%d engine-read fields compared", CENSUS_N_FIELDS);
            } else {
                warn("field census", "no --ref-vin, so nothing is asserted "
                     "-- a frozen field cannot be told from a constant one");
            }
            /* The unit table is a view over the VIN's buffer -- freeing the
             * VIN releases it; there is no table-level free. */
            if (have_ref) spfy_vin_free(&rv);
        }
    }

    const spfy_vb_chunk *cn = spfy_vb_riff_get(&rvin, "cnts");
    if (cn && cn->n >= 12)
        ck(rd_u32(cn->data + 8) == ut.n_units, "cnts unit count",
           "%u vs %u", rd_u32(cn->data + 8), ut.n_units);

    /* ---------------- hash: POPULATED, not correct ----------------
     *
     * The header above says `hash` is deliberately unchecked, and that still
     * holds for its CONTENTS -- which joins it prices is spfy_hash_roundtrip's
     * business and an open question besides. This asks only whether the table
     * describes THIS voice at all.
     *
     * ⛔ THE DEFECT THIS CATCHES, MEASURED 2026-08-22. `--template-vin` seeds
     * the VIN with the template's chunks, and S4 is what replaces `hash` with
     * one built from our own units. When S4 died silently -- it does, past
     * ~2.5M units, if it runs in the same process as S1-S3 -- the build kept
     * VENDOR TOM'S hash chunk verbatim (22,100,640 bytes, byte-identical) and
     * exited 0. All 45 checks here passed. A voice carrying another voice's
     * join costs is not obviously broken: it is structurally perfect and
     * prices joins between units that do not exist in it.
     *
     * `head` carries (rows, cells). Our builder emits one row per unit; the
     * vendors emit several, indexing by right-context. So rows >= units holds
     * for every good voice and is not a fitted threshold -- rows < units means
     * some units have no join row at all, which is the defect exactly:
     *
     *     vendor tom  169,579 units   692,190 rows   4.08
     *     vendor jill 185,475 units   560,534 rows   3.02
     *     crsmara     230,514 units   230,514 rows   1.00
     *     crstom_F  2,257,540 units 2,257,540 rows   1.00
     *     BROKEN    2,577,400 units   692,190 rows   0.27   <- tom's table
     */
    const spfy_vb_chunk *ch_hash = spfy_vb_riff_get(&rvin, "hash");
    if (ch_hash && ch_hash->n >= 16
        && memcmp(ch_hash->data, "head", 4) == 0) {
        uint32_t rows = rd_u32(ch_hash->data + 8);
        uint32_t cells = rd_u32(ch_hash->data + 12);
        double per = ut.n_units ? (double)ch_hash->n / ut.n_units : 0.0;
        /* The detail string prints on PASS as well as FAIL, so it stays
         * factual; the diagnosis goes below, only when it is one. */
        ck(rows >= ut.n_units, "hash rows cover every unit",
           "%u rows for %u units (%.2f per unit)", rows, ut.n_units,
           ut.n_units ? (double)rows / ut.n_units : 0.0);
        printf("        hash %zu B, %u rows, %u cells, %.1f B/unit\n",
               ch_hash->n, rows, cells, per);
        if (rows < ut.n_units)
            printf("        ⛔ this hash table does not describe this voice. "
                   "Either S4 never ran, or the --template-vin's hash chunk "
                   "survived the build. Re-run with --s4-only.\n");

        /* ⛔ THE PROBE IS UNBOUNDED IN THE VENDOR ENGINE, SO THE TAIL IS
         * LOAD-BEARING. SWIttsUSel.dll+0xb7e6 is
         *
         *     cmp [cells + (rows[uid_right] + uid_left)*8], uid_right
         *
         * with no test against n_cells in front of it -- the key comparison
         * IS the miss test, and it happens AFTER the read. Our hash.c guards
         * the index (`if (idx >= n_cells) return SPFY_E_OOB`), so a table
         * sized to its last POPULATED cell renders perfectly in spfy_synth
         * and access-violates in Speechify on the first phrase that pairs the
         * widest row with a high uid_left.
         *
         * The rule is not a margin, it is an identity. Every vendor voice
         * carries n_cells == max(rows[]) + n_rows to the cell:
         *
         *     tom      max(rows) 1,724,291 + n_rows 692,190 = 2,416,481  +0
         *     jill               2,059,585 +        560,534 = 2,620,119  +0
         *     javier             1,638,488 +        668,348 = 2,306,836  +0
         *     paulina            1,367,589 +        663,410 = 2,030,999  +0
         *     felix              2,906,700 +        737,394 = 3,644,094  +0
         *
         * Ours shipped SHORT of it -- crstom by 5,602 cells, crsmara by
         * 6,506. crstom died on "attention signal." (rows[222144] 4,449,427 +
         * uid_left 278,391 = 4,727,818 against 4,724,617 cells, reading
         * 25,616 bytes past a 37,797,888-byte allocation); crsmara had the
         * same defect and had simply not been asked yet.
         *
         * Test against n_rows, not the unit count: uid_left is a unit id so
         * n_units would be sufficient, but n_rows is what the vendor used and
         * n_rows >= n_units always. */
        const uint8_t *rp = NULL;
        size_t rn = 0;
        for (size_t o = 0; o + 8 <= ch_hash->n; ) {
            uint32_t sz = rd_u32(ch_hash->data + o + 4);
            if (memcmp(ch_hash->data + o, "rows", 4) == 0) {
                rp = ch_hash->data + o + 8;
                rn = sz;
                break;
            }
            o += 8u + sz + (sz & 1u);
        }
        if (rp && rn >= (size_t)rows * 4u) {
            uint32_t max_row = 0;
            for (uint32_t r = 0; r < rows; ++r) {
                uint32_t v = rd_u32(rp + (size_t)r * 4u);
                if (v > max_row) max_row = v;
            }
            double worst = (double)max_row + rows;
            ck(worst <= (double)cells, "hash tail absorbs the widest probe",
               "max(rows) %u + %u rows = %.0f vs %u cells (%+.0f)",
               max_row, rows, worst, cells, (double)cells - worst);
            if (worst > (double)cells)
                printf("        ⛔ Speechify will access-violate inside "
                       "SWIttsUSel on some phrase. Rebuild the hash (the "
                       "packer pads to max(rows)+n_rows now) or pad an "
                       "existing VIN with vb_hashpad.py --write.\n");
        } else {
            ck(0, "hash rows sub-chunk present", "%s",
               rp ? "rows sub-chunk short" : "no rows sub-chunk");
        }
    } else {
        ck(0, "hash head sub-chunk present", "%s",
           ch_hash ? "no `head` tag" : "no hash chunk");
    }

    /* indx: names unique, offsets monotonic, last within data. */
    const spfy_vb_chunk *ci = spfy_vb_riff_get(&rvdb, "indx");
    const spfy_vb_chunk *cd = spfy_vb_riff_get(&rvdb, "data");
    uint32_t n_indx = 0;
    uint32_t *ioff = NULL;
    char **iname = NULL;
    if (ci && ci->n >= 4 && cd) {
        n_indx = rd_u32(ci->data);
        ioff = (uint32_t *)calloc(n_indx ? n_indx : 1u, 4);
        iname = (char **)calloc(n_indx ? n_indx : 1u, sizeof *iname);
        size_t off = 4;
        uint32_t got = 0, nonmono = 0, oob = 0;
        for (uint32_t i = 0; i < n_indx && off + 6 <= ci->n; ++i) {
            uint32_t o = rd_u32(ci->data + off);
            uint16_t ln = rd_u16(ci->data + off + 4);
            if (off + 6 + ln > ci->n) break;
            char *nm = (char *)malloc((size_t)ln + 1u);
            memcpy(nm, ci->data + off + 6, ln);
            nm[ln] = 0;
            ioff[i] = o; iname[i] = nm;
            if (i && o < ioff[i - 1]) ++nonmono;
            if (o > cd->n) ++oob;
            off += 6u + ln;
            ++got;
        }
        ck(got == n_indx, "indx entries parse", "%u of %u", got, n_indx);
        ck(nonmono == 0, "indx offsets monotonic", "%u out of order", nonmono);
        ck(oob == 0, "indx offsets within data", "%u past end of %zu B",
           oob, cd->n);
        if (got) ck(ioff[got - 1u] == cd->n, "indx sentinel == data size",
                    "%u vs %zu", ioff[got - 1u], cd->n);
        /* Duplicate names would silently point two entries at one offset. */
        uint32_t dup = 0;
        for (uint32_t i = 1; i < got; ++i)
            for (uint32_t j = i + 1u; j < got && j < i + 40u; ++j)
                if (iname[i] && iname[j] && !strcmp(iname[i], iname[j])) ++dup;
        ck(dup == 0, "indx names unique (windowed)", "%u dups", dup);
    }

    /* feat filenames must resolve in the VDB: that is the engine's own key. */
    if (rc_f == SPFY_OK && rc_l == SPFY_OK) {
        uint32_t unresolved = 0;
        for (uint32_t i = 0; i < ft.n_entries; ++i) {
            uint32_t o = 0, s = 0;
            if (spfy_vdb_lookup_by_name(&lk, ft.entries[i].name,
                                        ft.entries[i].name_len, &o, &s) != SPFY_OK)
                ++unresolved;
        }
        ck(unresolved == 0, "every feat filename in VDB", "%u unresolved of %u",
           unresolved, ft.n_entries);
    }

    /* ---------------- unit records ---------------- */
    spfy_ccos_t cc = {0};
    int rc_c = spfy_ccos_load(&vin, &cc);
    ck(rc_c == SPFY_OK, "ccos loads", "%u labels, %u hp_classes",
       cc.n_labels, cc.n_hp_classes);

    if (rc_u == SPFY_OK) {
        const uint32_t bpms = vdb.sample_rate / 1000u ? vdb.sample_rate / 1000u : 8u;
        uint32_t bad_file = 0, bad_span = 0, bad_phone = 0, bad_ctx = 0;
        uint32_t flagb_gap = 0, n_flagb = 0, first_half = 0, checked = 0;
        uint32_t flagb_round = 0;
        long long flagb_maxms = 0;
        uint64_t prev_abs = 0;
        uint32_t prev_dur = 0;
        int prev_ok = 0;
        for (uint32_t u = 0; u < ut.n_units; ++u) {
            spfy_unit_record_t r;
            if (spfy_unit_record_get(&ut, u, &r) != SPFY_OK) { prev_ok = 0; continue; }
            if (r.file_idx >= ft.n_entries) { ++bad_file; prev_ok = 0; continue; }
            uint32_t o = 0, s = 0;
            int resolved = spfy_vdb_lookup_by_name(&lk, ft.entries[r.file_idx].name,
                                                   ft.entries[r.file_idx].name_len,
                                                   &o, &s) == SPFY_OK;
            if (resolved
                && (uint64_t)r.local_pos * bpms + (uint64_t)r.dur_like * bpms > s)
                ++bad_span;
            if (rc_c == SPFY_OK && r.phone_center >= cc.n_labels) ++bad_phone;
            for (int k = 0; k < 4; ++k)
                if (rc_c == SPFY_OK && r.phone_ctx[k] != 0xFF
                    && r.phone_ctx[k] >= cc.n_labels) ++bad_ctx;
            if (r.flag_b) ++n_flagb;
            if (r.is_first_half) ++first_half;

            /* flag_b is the engine's "this continues the previous unit", and
             * dag_join_cb grants it a FREE join. The invariant is therefore
             * ACOUSTIC ADJACENCY IN `data`, not equality of file_idx -- a
             * recording stored as several contiguous indx entries really is
             * continuous across the boundary, which is why 230 of jill's
             * flag_b units sit on a file_idx change and are correct. */
            uint64_t abs_off = (uint64_t)o + (uint64_t)r.local_pos * bpms;
            if (r.flag_b && u) {
                if (!prev_ok) {
                    ++flagb_gap;
                } else {
                    long long d = (long long)abs_off
                                - (long long)(prev_abs + (uint64_t)prev_dur * bpms);
                    long long dms = d / (long long)bpms;
                    if (d) {
                        if (dms < 0) dms = -dms;
                        if (dms > flagb_maxms) flagb_maxms = dms;
                        /* local_pos and dur_like are integer MILLISECONDS, so
                         * a boundary falling mid-ms leaves a 1 ms seam that is
                         * rounding, not a discontinuity. */
                        if (dms <= 1) ++flagb_round; else ++flagb_gap;
                    }
                }
                ++checked;
            }
            if (r.flag_b && u == 0) ++flagb_gap;
            prev_abs = abs_off;
            prev_dur = r.dur_like;
            prev_ok = resolved;
        }
        ck(bad_file == 0, "unit.file_idx in range", "%u bad", bad_file);
        ck(bad_phone == 0, "unit.phone_center < n_labels", "%u bad", bad_phone);
        ck(bad_ctx == 0, "unit.phone_ctx < n_labels", "%u bad", bad_ctx);
        /* ⚠ THRESHOLD CALIBRATED ON THE VENDORS, NOT ASSUMED. flag_b grants a
         * FREE join, so byte-adjacency looks like it ought to be exact -- but
         * jill ships 252 of 169,380 (0.15%) and tom 472 of 151,393 (0.31%)
         * where it is not, tom's worst gap 11 ms and jill's a local_pos u16
         * wrap. So the format tolerates it and only a much higher rate means
         * something structurally different. */
        double gap_pct = 100.0 * flagb_gap / (double)(checked ? checked : 1u);
        if (gap_pct <= 1.0)
            ck(1, "flag_b audio adjacency", "%u of %u non-contiguous (%.2f%%, "
               "vendors 0.15-0.31%%), max %lld ms",
               flagb_gap, checked, gap_pct, flagb_maxms);
        else
            ck(0, "flag_b audio adjacency", "%u of %u non-contiguous (%.2f%%) "
               "-- vendors sit at 0.15-0.31%%, max %lld ms",
               flagb_gap, checked, gap_pct, flagb_maxms);
        (void)flagb_round;
        /* edge_frames.c already tolerates an over-long span (it counts
         * n_missing and skips the unit), so a handful is format-legal slop
         * rather than corruption. jill ships 31. */
        if (bad_span == 0)
            ck(1, "unit span within its recording", "0 over-long");
        else if (bad_span * 1000u <= ut.n_units)
            warn("unit span within its recording",
                 "%u over-long (%.3f%%); the engine skips these", bad_span,
                 100.0 * bad_span / (double)ut.n_units);
        else
            ck(0, "unit span within its recording", "%u over-long (%.2f%%)",
               bad_span, 100.0 * bad_span / (double)ut.n_units);
        printf("        flag_b on %.2f%% of units, is_first_half %.2f%% "
               "[tom 19.65, jill 19.93]\n",
               100.0 * n_flagb / (double)ut.n_units,
               100.0 * first_half / (double)ut.n_units);
    }

    /* ---------------- mean ---------------- */
    printf("\n== stats ==\n");
    const spfy_vb_chunk *cm = spfy_vb_riff_get(&rvin, "mean");
    if (cm && cm->n >= 8) {
        uint32_t rows = rd_u32(cm->data), cols = rd_u32(cm->data + 4);
        ck(cm->n == 8u + (size_t)rows * cols * 4u, "mean size consistent",
           "%u x %u", rows, cols);
        uint32_t nonfinite = 0, populated = 0;
        for (uint32_t r = 0; r < rows; ++r) {
            int any = 0;
            for (uint32_t c = 0; c < cols; ++c) {
                float v;
                memcpy(&v, cm->data + 8 + ((size_t)r * cols + c) * 4u, 4);
                if (!isfinite(v)) ++nonfinite;
                if (v != 0.0f) any = 1;
            }
            populated += any ? 1u : 0u;
        }
        ck(nonfinite == 0, "mean all finite", "%u non-finite", nonfinite);
        printf("        %u of %u classes populated\n", populated, rows);
        /* ⚠ NOT AN ERROR WHEN ccos HAS MORE. tom ships 47 ccos labels (the
         * 47th is the empty string, a sentinel) against a 92-row mean, so
         * 2*n_labels > rows is vendor-normal. Only a mean SHORTER than the
         * hp_classes actually used would be a real out-of-range risk. */
        if (rc_c == SPFY_OK && rows < 2u * cc.n_labels)
            printf("        note: mean has %u rows, ccos 2*%u=%u hp_classes "
                   "(tom is the same; the extra label is a sentinel)\n",
                   rows, cc.n_labels, 2u * cc.n_labels);
    }

    /* ---------------- prsl ---------------- */
    spfy_prsl_t prsl;
    if (spfy_prsl_load(&vin, &prsl) == SPFY_OK) {
        uint32_t bad_uid = 0, empty = 0, badkey = 0;
        size_t slots = 0;
        for (uint32_t g = 0; g < prsl.n_groups; ++g) {
            uint32_t k = prsl.groups[g].context_key;
            uint32_t L = k / 10000u, C = (k / 100u) % 100u, R = k % 100u;
            if (L > 92u || C > 92u || R > 92u) ++badkey;
            if (!prsl.groups[g].n_candidates) ++empty;
            slots += prsl.groups[g].n_candidates;
            for (uint32_t i = 0; i < prsl.groups[g].n_candidates; ++i)
                if (spfy_prsl_cand(prsl.groups[g].candidates, i) >= ut.n_units)
                    ++bad_uid;
        }
        ck(bad_uid == 0, "prsl candidates in range", "%u bad of %zu",
           bad_uid, slots);
        ck(badkey == 0, "prsl context keys well-formed", "%u bad", badkey);
        ck(empty == 0, "prsl groups non-empty", "%u empty", empty);
        printf("        %u groups, %zu candidate slots\n", prsl.n_groups, slots);
        spfy_prsl_free(&prsl);
    } else {
        ck(0, "prsl loads", "loader rejected it");
    }

    /* ---------------- durt / f0tr ---------------- */
    printf("\n== trees ==\n");
    for (int which = 0; which < 2; ++which) {
        spfy_cart_t c = {0};
        int rc2 = which ? spfy_cart_load_f0tr(&vin, &c)
                        : spfy_cart_load_durt(&vin, &c);
        const char *nm = which ? "f0tr" : "durt";
        if (rc2 != SPFY_OK) { ck(0, nm, "loader rejected it"); continue; }
        uint32_t badq = 0, badchild = 0, nonfinite = 0, leaves = 0, nodes = 0;
        /* ⛔ THE LEAF'S SECOND FLOAT IS 1/sd, NOT A VARIANCE. The engine
         * squares `(f0_context - mean) * var`, which is a z-score only if the
         * field is a precision. Both vendors sit in 0.017..1.0 with tom's
         * maximum exactly 1.0000 -- an sd floor of 1. A generator that writes
         * wagon's raw variance (~400 for durt) puts every duration cost about
         * 10^6 times too high, and NOTHING else here notices: the container
         * verifies, the trees round-trip, the voice just stops hearing any
         * cost but duration. */
        uint32_t badvar = 0;
        float maxvar = 0.0f;
        for (uint32_t t = 0; t < c.n_trees; ++t) {
            const spfy_cart_tree_t *tr = &c.trees[t];
            for (uint32_t i = 0; i < tr->n_nodes; ++i) {
                const spfy_cart_node_t *n = &tr->nodes[i];
                ++nodes;
                if (n->yes_child < 0) {
                    ++leaves;
                    if (!isfinite(n->leaf_mean) || !isfinite(n->leaf_var))
                        ++nonfinite;
                    if (n->leaf_var < 0.0f || n->leaf_var > 1.0f) ++badvar;
                    if (n->leaf_var > maxvar) maxvar = n->leaf_var;
                    continue;
                }
                if (n->q_index >= c.n_ques) ++badq;
                if ((uint32_t)n->yes_child >= tr->n_nodes
                    || n->no_child >= tr->n_nodes) ++badchild;
            }
        }
        char label[32];
        snprintf(label, sizeof label, "%s children in range", nm);
        ck(badchild == 0, label, "%u bad", badchild);
        snprintf(label, sizeof label, "%s question indices", nm);
        ck(badq == 0, label, "%u out of %u questions", badq, c.n_ques);
        snprintf(label, sizeof label, "%s leaves finite", nm);
        ck(nonfinite == 0, label, "%u non-finite", nonfinite);
        snprintf(label, sizeof label, "%s leaf var is 1/sd", nm);
        ck(badvar == 0, label, "%u outside (0,1]; max %.4f", badvar, maxvar);
        printf("        %u trees, %u nodes, %u leaves, %u questions\n",
               c.n_trees, nodes, leaves, c.n_ques);
        /* Question keys must be ones the walker can answer. */
        uint32_t badkey = 0;
        for (uint32_t q = 0; q < c.n_ques; ++q)
            if (c.ques[q].type >= 16u) ++badkey;
        snprintf(label, sizeof label, "%s question keys < 16", nm);
        ck(badkey == 0, label, "%u bad", badkey);
        spfy_cart_free(&c);
    }

    /* ---------------- ccos ---------------- */
    if (rc_c == SPFY_OK) {
        uint32_t bad_diag = 0, bad_sym = 0;
        for (uint32_t hp = 0; hp < cc.n_hp_classes; ++hp)
            for (uint32_t s = 0; s < SPFY_CCOS_N_SLOTS; ++s) {
                const float *m = spfy_ccos_table(&cc, hp, s);
                if (!m) continue;
                for (uint32_t i = 0; i < cc.n_labels; ++i) {
                    if (m[(size_t)i * cc.n_labels + i] != 0.0f) ++bad_diag;
                    for (uint32_t j = i + 1u; j < cc.n_labels; ++j)
                        if (m[(size_t)i * cc.n_labels + j]
                            != m[(size_t)j * cc.n_labels + i]) ++bad_sym;
                }
            }
        ck(bad_diag == 0, "ccos diagonal zero", "%u non-zero", bad_diag);
        ck(bad_sym == 0, "ccos symmetric", "%u asymmetric", bad_sym);
    }

    /* ---------------- hist ---------------- */
    const spfy_vb_chunk *ch2 = spfy_vb_riff_get(&rvin, "hist");
    if (ch2 && ch2->n >= 24 && !memcmp(ch2->data, "head", 4)
        && !memcmp(ch2->data + 16, "data", 4)) {
        uint32_t hn = rd_u32(ch2->data + 8);
        int32_t sub = (int32_t)rd_u32(ch2->data + 12);
        uint32_t dn = rd_u32(ch2->data + 20);
        ck(dn == hn * 4u, "hist head/data agree", "n=%u sub_off=%d data=%u B",
           hn, sub, dn);
        uint32_t nonfinite = 0;
        int allzero = 1;
        for (uint32_t i = 0; i < hn && 24u + i * 4u + 4u <= ch2->n; ++i) {
            float v;
            memcpy(&v, ch2->data + 24 + i * 4u, 4);
            if (!isfinite(v)) ++nonfinite;
            if (v != 0.0f) allzero = 0;
        }
        ck(nonfinite == 0, "hist finite", "%u non-finite", nonfinite);
        if (allzero)
            warn("hist is flat", "no F0 in the inventory; the engine's gate "
                                 "cannot fire, so it is never read");
    }

    printf("\n%d passed, %d failed, %d warnings\n", g_pass, g_fail, g_warn);
    if (expect_clean && g_fail)
        printf("⛔ --expect-clean: a check failed on a container that should "
               "be clean. Fix the CHECK first if this is a vendor.\n");
    free(ioff);
    if (iname) { for (uint32_t i = 0; i < n_indx; ++i) free(iname[i]); free(iname); }
    return g_fail ? 1 : 0;
}
