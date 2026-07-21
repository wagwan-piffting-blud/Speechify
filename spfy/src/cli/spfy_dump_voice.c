/* spfy_dump_voice -- M0a verification CLI.
 *
 *   spfy_dump_voice <voice.vin> <voice.vdb> <voice.vcf>
 *       Load + summarise, then probe a few VCF params.
 *
 *   spfy_dump_voice --roundtrip-vin <in.vin> <out.vin>
 *   spfy_dump_voice --roundtrip-vdb <in.vdb> <out.vdb>
 *       Read, deobfuscate, re-obfuscate, write to out. The output must
 *       be byte-identical to the input (`cmp in out`); this is the
 *       M0a definition-of-done check for chunk-byte roundtripping.
 */

#include <spfy/spfy.h>
#include <spfy/spfy_voice.h>

#include "../common/file_io.h"
#include "../common/obfuscation.h"
#include "../voice/voice.h"
#include "../voice/unit_table.h"
#include "../voice/vcf_matrix.h"
#include "../voice/ccos.h"
#include "../voice/feat_table.h"
#include "../voice/phone_order.h"
#include "../voice/vdb_lookup.h"
#include "../usel/prsl.h"
#include "../usel/hash.h"
#include "../fe/phoneset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int do_roundtrip(const char *in_path, const char *out_path)
{
    uint8_t *buf = NULL;
    size_t   n   = 0;
    int rc = spfy_slurp_file(in_path, &buf, &n);
    if (rc != SPFY_OK) {
        fprintf(stderr, "read %s: %s\n", in_path, spfy_strerror(rc));
        return 1;
    }
    /* deobfuscate then re-obfuscate -- symmetric, must yield identical bytes */
    spfy_unobfuscate_ce(buf, n);
    spfy_unobfuscate_ce(buf, n);

    FILE *fp = fopen(out_path, "wb");
    if (!fp) { free(buf); fprintf(stderr, "open %s\n", out_path); return 1; }
    size_t w = fwrite(buf, 1, n, fp);
    int werr = (w != n);
    fclose(fp);
    free(buf);
    if (werr) { fprintf(stderr, "short write\n"); return 1; }
    fprintf(stdout, "roundtripped %zu bytes: %s -> %s\n", n, in_path, out_path);
    fprintf(stdout, "verify with: cmp %s %s\n", in_path, out_path);
    return 0;
}

static int do_summary(const char *vin, const char *vdb, const char *vcf)
{
    spfy_voice *v = NULL;
    int rc = spfy_voice_open(vin, vdb, vcf, &v);
    if (rc != SPFY_OK) {
        fprintf(stderr, "spfy_voice_open: %s\n", spfy_strerror(rc));
        return 1;
    }

    printf("%s\n", spfy_version());
    printf("vin           : %s\n", vin);
    printf("vdb           : %s\n", vdb);
    printf("vcf           : %s\n", vcf);
    printf("sample_rate   : %u Hz\n", spfy_voice_sample_rate(v));
    printf("n_units       : %zu\n", spfy_voice_n_units(v));
    printf("n_recordings  : %zu\n", spfy_voice_n_recordings(v));
    printf("hash_n_rows   : %zu\n", spfy_voice_hash_n_rows(v));
    printf("hash_n_cells  : %zu\n", spfy_voice_hash_n_cells(v));

    static const char *probes[] = {
        "tts.voiceCfg.JOIN_COST_WEIGHT",
        "tts.voiceCfg.JOIN_COST_OFFSET",
        "tts.voiceCfg.ABS_F0_WEIGHT",
        "tts.voiceCfg.DUR_WEIGHT",
        "tts.voiceCfg.HALFPHONE_CAND_PRUNE_THRESH",
    };
    printf("vcf params:\n");
    for (size_t i = 0; i < sizeof probes / sizeof *probes; ++i) {
        const char *s = NULL;
        int prc = spfy_voice_vcf_get_str(v, probes[i], &s);
        printf("  %-44s = %s\n", probes[i],
               prc == SPFY_OK ? s : "(missing)");
    }
    if (getenv("SPFY_DUMP_ALL_VCF")) {
        printf("\nALL vcf params:\n");
        for (const spfy_vcf_kv_t *kv = v->vcf.params; kv; kv = kv->next) {
            printf("  %s = %s\n", kv->key, kv->value);
        }
    }

    spfy_voice_close(v);
    return 0;
}

static int do_unit(const char *vin_path, uint32_t uid)
{
    spfy_vin_t vin = {0};
    int rc = spfy_vin_load(vin_path, &vin);
    if (rc != SPFY_OK) {
        fprintf(stderr, "vin_load: %s\n", spfy_strerror(rc));
        return 1;
    }
    spfy_unit_table_t t = {0};
    rc = spfy_unit_table_load(&vin, &t);
    if (rc != SPFY_OK) {
        fprintf(stderr, "unit_table_load: %s\n", spfy_strerror(rc));
        spfy_vin_free(&vin);
        return 1;
    }
    spfy_unit_record_t r = {0};
    rc = spfy_unit_record_get(&t, uid, &r);
    if (rc != SPFY_OK) {
        fprintf(stderr, "unit %u: %s\n", uid, spfy_strerror(rc));
        spfy_vin_free(&vin);
        return 1;
    }
    printf("unit_table version : %u\n", t.version);
    printf("n_units            : %u\n", t.n_units);
    printf("unit %u:\n", uid);
    printf("  file_idx       : %u\n", r.file_idx);
    printf("  local_pos      : %u (ms)\n", r.local_pos);
    printf("  dur_like       : %u (ms)\n", r.dur_like);
    printf("  sp_syl_in_phrase  : %u\n", r.sp_syl_in_phrase);
    printf("  sp_syl_type       : %u\n", r.sp_syl_type);
    printf("  sp_word_in_phrase : %u\n", r.sp_word_in_phrase);
    printf("  sp_syl_in_word    : %u\n", r.sp_syl_in_word);
    printf("  f0_start/end/mid/ctx : %u %u %u %u\n",
           r.f0_start, r.f0_end, r.f0_mid, r.f0_context);
    printf("  phone_center   : %u\n", r.phone_center);
    printf("  is_first_half  : %u\n", r.is_first_half);
    printf("  phone_ctx      : %u %u %u %u\n",
           r.phone_ctx[0], r.phone_ctx[1], r.phone_ctx[2], r.phone_ctx[3]);
    printf("  flag_b         : %u\n", r.flag_b);
    printf("  context_cost   : %u\n", r.context_cost);
    spfy_vin_free(&vin);
    return 0;
}

static int do_proscost(const char *vcf_path)
{
    spfy_vcf_t vcf = {0};
    int rc = spfy_vcf_load(vcf_path, &vcf);
    if (rc != SPFY_OK) {
        fprintf(stderr, "vcf_load: %s\n", spfy_strerror(rc));
        return 1;
    }
    spfy_proscost_matrix_t mats[SPFY_PROSCOST_N];
    rc = spfy_proscost_load(&vcf, mats);
    if (rc != SPFY_OK) {
        fprintf(stderr, "proscost_load: %s\n", spfy_strerror(rc));
        spfy_vcf_free(&vcf);
        return 1;
    }
    static const char *NAMES[] = {
        "sylInPhraseCosts", "sylTypeCosts", "wordInPhraseCosts",
        "sylInWordCosts",   "phoneInSylCosts",
    };
    for (int k = 0; k < SPFY_PROSCOST_N; ++k) {
        printf("[%d] %-22s %u rows x %u cols\n",
               k, NAMES[k], mats[k].n_rows, mats[k].n_cols);
        if (mats[k].n_rows == 0) continue;
        printf("    cols:");
        for (uint32_t c = 0; c < mats[k].n_cols && c < 8; ++c) {
            printf(" %s", mats[k].col_names[c]);
        }
        if (mats[k].n_cols > 8) printf(" ...");
        printf("\n");
        for (uint32_t r = 0; r < mats[k].n_rows; ++r) {
            const char *rn = mats[k].row_names[r] ? mats[k].row_names[r]
                                                  : "(unset)";
            printf("    row %u %-22s :", r, rn);
            for (uint32_t c = 0; c < mats[k].n_cols; ++c) {
                printf(" %6.2f",
                       mats[k].data[(size_t)r * mats[k].n_cols + c]);
            }
            printf("\n");
        }
        continue;
        printf("    row[%s] :", mats[k].row_names[0]);
        for (uint32_t c = 0; c < mats[k].n_cols && c < 8; ++c) {
            printf(" %g", mats[k].data[0 * mats[k].n_cols + c]);
        }
        if (mats[k].n_cols > 8) printf(" ...");
        printf("\n");
    }
    spfy_proscost_free(mats);
    spfy_vcf_free(&vcf);
    return 0;
}

static int do_prsl(const char *vin_path, uint32_t key)
{
    spfy_vin_t vin = {0};
    int rc = spfy_vin_load(vin_path, &vin);
    if (rc != SPFY_OK) {
        fprintf(stderr, "vin_load: %s\n", spfy_strerror(rc));
        return 1;
    }
    spfy_prsl_t p = {0};
    rc = spfy_prsl_load(&vin, &p);
    if (rc != SPFY_OK) {
        fprintf(stderr, "prsl_load: %s\n", spfy_strerror(rc));
        spfy_vin_free(&vin); return 1;
    }
    printf("prsl groups: %u\n", p.n_groups);
    if (p.n_groups > 0) {
        printf("first key  : %u  (n_cand=%u)\n",
               p.groups[0].context_key, p.groups[0].n_candidates);
        printf("last  key  : %u  (n_cand=%u)\n",
               p.groups[p.n_groups - 1].context_key,
               p.groups[p.n_groups - 1].n_candidates);
    }

    const uint32_t *cands = NULL; uint32_t n_cands = 0;
    rc = spfy_prsl_lookup(&p, key, &cands, &n_cands);
    if (rc == SPFY_OK) {
        printf("\nlookup key=%u -> %u candidates", key, n_cands);
        printf("\n  ");
        for (uint32_t i = 0; i < n_cands && i < 16; ++i) {
            printf("%u ", cands[i]);
        }
        if (n_cands > 16) printf("...");
        printf("\n");
    } else {
        /* Print neighbors so we can eyeball the fallback structure. */
        printf("\nlookup key=%u -> miss\n", key);
        printf("nearby keys (lo decompose: l=%u c=%u r=%u):\n",
               key / 10000u, (key / 100u) % 100u, key % 100u);
        uint32_t closest_lo = 0, closest_hi = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < p.n_groups; ++i) {
            uint32_t k = p.groups[i].context_key;
            if (k <  key && k > closest_lo) closest_lo = k;
            if (k >  key && k < closest_hi) closest_hi = k;
        }
        if (closest_lo > 0)
            printf("  closest below : %u  (l=%u c=%u r=%u)\n",
                   closest_lo, closest_lo/10000u,
                   (closest_lo/100u)%100u, closest_lo%100u);
        if (closest_hi != 0xFFFFFFFFu)
            printf("  closest above : %u  (l=%u c=%u r=%u)\n",
                   closest_hi, closest_hi/10000u,
                   (closest_hi/100u)%100u, closest_hi%100u);
        /* Also probe documented fallback keys: drop right, drop center, etc. */
        uint32_t l = key / 10000u, c = (key / 100u) % 100u, r = key % 100u;
        struct { const char *name; uint32_t k; } probes[] = {
            { "drop right (r=0)", l * 10000u + c * 100u },
            { "drop left  (l=0)", c * 100u + r },
            { "center only    ", c * 100u },
            { "left  only     ", l * 10000u },
        };
        for (size_t i = 0; i < sizeof probes / sizeof *probes; ++i) {
            const uint32_t *cc = NULL; uint32_t nn = 0;
            int hit = spfy_prsl_lookup(&p, probes[i].k, &cc, &nn) == SPFY_OK;
            printf("  %s = %u  -> %s (n_cand=%u)\n",
                   probes[i].name, probes[i].k,
                   hit ? "HIT" : "miss", hit ? nn : 0);
        }
    }

    spfy_prsl_free(&p);
    spfy_vin_free(&vin);
    return 0;
}

static int do_ccos(const char *vin_path, uint32_t hp_class, uint32_t slot)
{
    spfy_vin_t vin = {0};
    int rc = spfy_vin_load(vin_path, &vin);
    if (rc != SPFY_OK) {
        fprintf(stderr, "vin_load: %s\n", spfy_strerror(rc));
        return 1;
    }
    spfy_ccos_t c = {0};
    rc = spfy_ccos_load(&vin, &c);
    if (rc != SPFY_OK) {
        fprintf(stderr, "ccos_load: %s\n", spfy_strerror(rc));
        spfy_vin_free(&vin); return 1;
    }
    printf("ccos n_labels=%u  n_hp_classes=%u  side=%s slot=%u\n",
           c.n_labels, c.n_hp_classes,
           hp_class < c.n_labels ? "LEFT" : "RIGHT", slot);
    const float *t = spfy_ccos_table(&c, hp_class, slot);
    if (!t) {
        fprintf(stderr, "out of range hp_class=%u slot=%u\n", hp_class, slot);
        spfy_ccos_free(&c); spfy_vin_free(&vin); return 1;
    }
    /* Print row 32 (silence label) all cols if requested. */
    {
        const char *r_env = getenv("SPFY_CCOS_ROW");
        if (r_env) {
            uint32_t rr = (uint32_t)atoi(r_env);
            if (rr < c.n_labels) {
                printf("row %u: ", rr);
                for (uint32_t cc = 0; cc < c.n_labels; ++cc) {
                    printf("%.3f ", t[rr * c.n_labels + cc]);
                }
                printf("\n");
                spfy_ccos_free(&c); spfy_vin_free(&vin);
                return 0;
            }
        }
    }
    /* Print top-left 6x6 corner. */
    printf("table (top-left 6x6):\n");
    for (uint32_t i = 0; i < 6 && i < c.n_labels; ++i) {
        printf("  ");
        for (uint32_t j = 0; j < 6 && j < c.n_labels; ++j) {
            printf("%7.3f ", t[i * c.n_labels + j]);
        }
        printf("\n");
    }
    /* Diagonal sanity check (must be 0). */
    int diag_ok = 1;
    for (uint32_t i = 0; i < c.n_labels; ++i) {
        if (t[i * c.n_labels + i] != 0.0f) { diag_ok = 0; break; }
    }
    /* Symmetry check. */
    int sym_ok = 1;
    for (uint32_t i = 0; i < c.n_labels && sym_ok; ++i) {
        for (uint32_t j = 0; j < i; ++j) {
            if (t[i * c.n_labels + j] != t[j * c.n_labels + i]) {
                sym_ok = 0; break;
            }
        }
    }
    printf("diagonal_zero=%s  symmetric=%s\n",
           diag_ok ? "OK" : "FAIL", sym_ok ? "OK" : "FAIL");

    spfy_ccos_free(&c);
    spfy_vin_free(&vin);
    return 0;
}

static int do_resolve(const char *vin_path, const char *vdb_path, uint32_t uid)
{
    spfy_vin_t vin = {0};
    spfy_vdb_t vdb = {0};
    spfy_unit_table_t units = {0};
    spfy_feat_table_t feat  = {0};
    spfy_vdb_lookup_t lookup = {0};
    int rc;
    if ((rc = spfy_vin_load(vin_path, &vin)) != SPFY_OK) goto done;
    if ((rc = spfy_vdb_load(vdb_path, &vdb)) != SPFY_OK) goto done;
    if ((rc = spfy_unit_table_load(&vin, &units)) != SPFY_OK) goto done;
    if ((rc = spfy_feat_table_load(&vin, &feat))  != SPFY_OK) goto done;
    if ((rc = spfy_vdb_lookup_build(&vdb, &lookup)) != SPFY_OK) goto done;

    spfy_unit_record_t r;
    if ((rc = spfy_unit_record_get(&units, uid, &r)) != SPFY_OK) goto done;
    printf("uid %u -> file_idx %u, local_pos %u, dur_like %u\n",
           uid, r.file_idx, r.local_pos, r.dur_like);
    if (r.file_idx >= feat.n_entries) {
        printf("  file_idx out of range (n_entries=%u)\n", feat.n_entries);
        rc = SPFY_E_OOB; goto done;
    }
    const spfy_feat_entry_t *fe = &feat.entries[r.file_idx];
    printf("  feat.filename[%u] = '%.*s'  (stored_id=%u)\n",
           r.file_idx, fe->name_len, fe->name, fe->stored_id);

    uint32_t off = 0, sz = 0;
    int hit = (spfy_vdb_lookup_by_name(&lookup, fe->name, fe->name_len,
                                       &off, &sz) == SPFY_OK);
    if (hit) {
        printf("  vdb '%.*s' @ offset %u, size %u bytes (%.3f s)\n",
               fe->name_len, fe->name, off, sz, (double)sz / 8000.0);
        uint32_t want_off = off + (uint32_t)r.local_pos * 8u;
        uint32_t want_sz  = (uint32_t)r.dur_like * 8u;
        printf("  unit audio: rec_offset+%u .. +%u  (%u bytes = %.3f s)\n",
               (uint32_t)r.local_pos * 8u,
               (uint32_t)r.local_pos * 8u + want_sz, want_sz,
               (double)want_sz / 8000.0);
        if (want_off + want_sz > off + sz) {
            printf("  WARNING: unit window extends past recording end!\n");
        }
    } else {
        printf("  VDB MISS for recording name -- ordering bug somewhere\n");
        rc = SPFY_E_OOB;
    }

done:
    spfy_vdb_lookup_free(&lookup);
    spfy_feat_table_free(&feat);
    spfy_vdb_free(&vdb);
    spfy_vin_free(&vin);
    if (rc != SPFY_OK) fprintf(stderr, "error: %s\n", spfy_strerror(rc));
    return rc == SPFY_OK ? 0 : 1;
}

/* Derive the per-unit hp_class table from the VIN alone and, when a
 * reference file is supplied, diff it against that. The reference is a
 * Frida-dumped hpclass.bin (one byte per unit); an exact match proves the
 * derivation can replace the capture. */
static int do_hpclass(const char *vin_path, const char *ref_path)
{
    spfy_vin_t vin = {0};
    int rc = spfy_vin_load(vin_path, &vin);
    if (rc != SPFY_OK) {
        fprintf(stderr, "vin_load: %s\n", spfy_strerror(rc));
        return 1;
    }
    spfy_unit_table_t units = {0};
    rc = spfy_unit_table_load(&vin, &units);
    if (rc != SPFY_OK) {
        fprintf(stderr, "unit_table_load: %s\n", spfy_strerror(rc));
        spfy_vin_free(&vin); return 1;
    }
    spfy_phone_order_t po = {0};
    rc = spfy_phone_order_build(&vin, &po);
    if (rc != SPFY_OK) {
        fprintf(stderr, "phone_order_build: %s\n", spfy_strerror(rc));
        spfy_vin_free(&vin); return 1;
    }

    printf("unit version %u  rec_size %u  n_units %u\n",
           units.version, units.rec_size, units.n_units);
    printf("phones %u  labels %u\n", po.n_phones, po.n_labels);
    printf("labl -> feat permutation (non-identity entries):\n");
    uint32_t n_swapped = 0;
    for (uint32_t i = 0; i < po.n_labels; ++i) {
        if (po.labl_to_feat[i] == (uint8_t)i) continue;
        printf("  [%2u] %.*s -> %u\n", i,
               (int)((po.labl_to_feat[i] < po.n_phones)
                     ? po.phone_name_len[po.labl_to_feat[i]] : 0),
               (po.labl_to_feat[i] < po.n_phones)
                   ? po.phone_names[po.labl_to_feat[i]] : "",
               po.labl_to_feat[i]);
        ++n_swapped;
    }
    if (n_swapped == 0) printf("  (identity)\n");

    uint8_t *hp = NULL;
    uint32_t hp_n = 0;
    rc = spfy_phone_order_hpclass(&po, &units, &hp, &hp_n);
    if (rc != SPFY_OK) {
        fprintf(stderr, "hpclass derive: %s\n", spfy_strerror(rc));
        spfy_phone_order_free(&po); spfy_vin_free(&vin); return 1;
    }
    uint32_t max_hp = 0;
    for (uint32_t i = 0; i < hp_n; ++i) if (hp[i] > max_hp) max_hp = hp[i];
    printf("derived hp_class: %u entries, max %u\n", hp_n, max_hp);

    int failed = 0;
    if (ref_path) {
        uint8_t *ref = NULL;
        size_t   ref_n = 0;
        if (spfy_slurp_file(ref_path, &ref, &ref_n) != SPFY_OK) {
            fprintf(stderr, "read %s failed\n", ref_path);
            failed = 1;
        } else if (ref_n != hp_n) {
            printf("MISMATCH: reference is %zu bytes, derived %u\n",
                   ref_n, hp_n);
            failed = 1;
        } else {
            uint32_t bad = 0, first = 0;
            for (uint32_t i = 0; i < hp_n; ++i) {
                if (ref[i] != hp[i]) {
                    if (bad == 0) first = i;
                    ++bad;
                }
            }
            if (bad == 0) {
                printf("EXACT MATCH vs %s (%u units)\n", ref_path, hp_n);
            } else {
                printf("MISMATCH: %u/%u differ, first at uid %u "
                       "(ref %u, derived %u)\n",
                       bad, hp_n, first, ref[first], hp[first]);
                failed = 1;
            }
        }
        free(ref);
    }

    spfy_phone_order_hpclass_free(hp);
    spfy_phone_order_free(&po);
    spfy_vin_free(&vin);
    return failed;
}

/* List the voice's phone inventory in feat["name"] order -- the engine's
 * phone-id numbering, and the numbering hp_class and the FE's ctx[] are
 * both built on. Prints the VCF phoneset alongside it so a symbol the FE
 * emits but the VIN does not carry is visible at a glance. */
static int do_phones(const char *vin_path, const char *vcf_path)
{
    spfy_vin_t vin = {0};
    int rc = spfy_vin_load(vin_path, &vin);
    if (rc != SPFY_OK) {
        fprintf(stderr, "vin_load: %s\n", spfy_strerror(rc));
        return 1;
    }
    spfy_phone_order_t po = {0};
    rc = spfy_phone_order_build(&vin, &po);
    if (rc != SPFY_OK) {
        fprintf(stderr, "phone_order_build: %s\n", spfy_strerror(rc));
        spfy_vin_free(&vin); return 1;
    }

    printf("VIN feat[\"name\"] phone order: %u phones "
           "(hp_class = id*2 + half, max %u)\n",
           po.n_phones, po.n_phones ? po.n_phones * 2u - 1u : 0u);
    for (uint32_t i = 0; i < po.n_phones; ++i)
        printf("  %3u  %-8s  hp_class %u/%u\n", i, po.phone_names[i],
               i * 2u, i * 2u + 1u);

    if (vcf_path) {
        spfy_vcf_t vcf = {0};
        if (spfy_vcf_load(vcf_path, &vcf) == SPFY_OK) {
            spfy_phoneset_t ps = {0};
            if (spfy_phoneset_load_from_vcf(&vcf, &ps) == SPFY_OK) {
                printf("\nVCF phoneset: %u phones, silence id %u\n",
                       ps.n_phones, ps.silence_phone_id);
                for (uint32_t i = 0; i < ps.n_phones; ++i) {
                    /* Flag any VCF symbol with no VIN counterpart: those
                     * are exactly the ones the FE cannot map to an engine
                     * phone id. */
                    const char *sym = ps.entries[i].name;
                    int in_vin = (spfy_phone_order_index(&po, sym) != 0xff);
                    printf("  %3u  %-8s  %s\n", i, sym,
                           in_vin ? "" : "<-- NOT IN VIN feat[\"name\"]");
                }
            }
            spfy_phoneset_free(&ps);
            spfy_vcf_free(&vcf);
        }
    }

    spfy_phone_order_free(&po);
    spfy_vin_free(&vin);
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s <voice.vin> <voice.vdb> <voice.vcf>\n"
        "       %s --roundtrip-vin <in.vin> <out.vin>\n"
        "       %s --roundtrip-vdb <in.vdb> <out.vdb>\n"
        "       %s --unit <voice.vin> <unit_id>\n"
        "       %s --proscost <voice.vcf>\n"
        "       %s --prsl <voice.vin> <context_key>\n"
        "       %s --ccos <voice.vin> <hp_class> <slot>\n"
        "       %s --resolve <voice.vin> <voice.vdb> <unit_id>\n"
        "       %s --hpclass <voice.vin> [reference_hpclass.bin]\n"
        "       %s --phones <voice.vin> [voice.vcf]\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0,
        argv0);
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--roundtrip-vin") == 0)
        return do_roundtrip(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "--roundtrip-vdb") == 0)
        return do_roundtrip(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "--unit") == 0)
        return do_unit(argv[2], (uint32_t)strtoul(argv[3], NULL, 10));
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "--hpclass") == 0)
        return do_hpclass(argv[2], argc == 4 ? argv[3] : NULL);
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "--phones") == 0)
        return do_phones(argv[2], argc == 4 ? argv[3] : NULL);
    if (argc == 5 && strcmp(argv[1], "--hash") == 0) {
        spfy_vin_t vin = {0};
        if (spfy_vin_load(argv[2], &vin) != SPFY_OK) {
            fprintf(stderr, "vin_load failed\n"); return 1;
        }
        spfy_hash_t h = {0};
        if (spfy_hash_load(&vin, &h) != SPFY_OK) {
            fprintf(stderr, "hash_load failed\n"); return 1;
        }
        uint32_t prev = (uint32_t)strtoul(argv[3], NULL, 10);
        uint32_t curr = (uint32_t)strtoul(argv[4], NULL, 10);
        float cost = 0.0f;
        int rc = spfy_hash_lookup(&h, prev, curr, &cost);
        if (rc == SPFY_OK) {
            printf("(%u, %u) HIT cellB=%g\n", prev, curr, cost);
        } else {
            printf("(%u, %u) MISS\n", prev, curr);
        }
        printf("  n_rows=%u n_cells=%u\n", h.n_rows, h.n_cells);
        if (curr < h.n_rows) {
            uint32_t ro = h.rows[curr];
            uint64_t target_idx = (uint64_t)ro + prev;
            printf("  rows[%u]=%u  target_idx=%llu\n", curr, ro,
                   (unsigned long long)target_idx);
            if (target_idx < h.n_cells) {
                printf("  cells_A[%llu]=%u cells_B=%g\n",
                       (unsigned long long)target_idx,
                       h.cells_A[target_idx],
                       (double)h.cells_B[target_idx]);
            }
            uint64_t lo = (ro > 16) ? ro - 16 : 0;
            uint64_t hi = ro + 32;
            if (hi > h.n_cells) hi = h.n_cells;
            printf("  window cells_A[%llu..%llu] around rows[curr]:\n",
                   (unsigned long long)lo, (unsigned long long)hi);
            for (uint64_t i = lo; i < hi; ++i) {
                printf("    [%llu] A=%u B=%g delta=%lld\n",
                       (unsigned long long)i, h.cells_A[i],
                       (double)h.cells_B[i],
                       (long long)((int64_t)i - (int64_t)ro));
            }
        }
        printf("  reverse: lookup(curr=%u, prev=%u): ", curr, prev);
        float cost2 = 0.0f;
        int rc2 = spfy_hash_lookup(&h, curr, prev, &cost2);
        printf("rc=%d cost=%g\n", rc2, (double)cost2);
        if (prev < h.n_rows) {
            uint32_t ro2 = h.rows[prev];
            uint64_t ti2 = (uint64_t)ro2 + curr;
            printf("  rows[%u]=%u  reverse_idx=%llu\n", prev, ro2,
                   (unsigned long long)ti2);
            if (ti2 < h.n_cells) {
                printf("  cells_A[%llu]=%u cells_B=%g\n",
                       (unsigned long long)ti2, h.cells_A[ti2],
                       (double)h.cells_B[ti2]);
            }
        }
        spfy_hash_free(&h); spfy_vin_free(&vin);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--proscost") == 0)
        return do_proscost(argv[2]);
    if (argc == 4 && strcmp(argv[1], "--prsl") == 0)
        return do_prsl(argv[2], (uint32_t)strtoul(argv[3], NULL, 10));
    if (argc == 5 && strcmp(argv[1], "--ccos") == 0)
        return do_ccos(argv[2], (uint32_t)strtoul(argv[3], NULL, 10),
                                (uint32_t)strtoul(argv[4], NULL, 10));
    if (argc == 5 && strcmp(argv[1], "--resolve") == 0)
        return do_resolve(argv[2], argv[3],
                          (uint32_t)strtoul(argv[4], NULL, 10));
    if (argc == 4) return do_summary(argv[1], argv[2], argv[3]);
    usage(argv[0]);
    return 2;
}
