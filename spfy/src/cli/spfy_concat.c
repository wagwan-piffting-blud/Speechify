/* spfy_concat -- M0b synthesis: concatenate engine-chosen units to WAV. */

#include <spfy/spfy.h>

#include "../voice/voice.h"
#include "../voice/unit_table.h"
#include "../voice/feat_table.h"
#include "../voice/vdb_lookup.h"
#include "../wsola/ulaw.h"
#include "../wsola/wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SILENCE_SENTINEL_UID 169578u

/* Per-unit play record. */
typedef struct {
    uint32_t  uid;
    uint32_t  dl;
    uint16_t  file_idx;
    uint16_t  local_pos;
    uint16_t  dur_like;
    uint8_t   phone_center;
    uint8_t   is_first_half;
    int       valid;
} unit_play_t;

typedef struct {
    unit_play_t *items;
    size_t       n;
    size_t       cap;
} unit_play_list_t;

static int play_list_push(unit_play_list_t *l, unit_play_t item)
{
    if (l->n >= l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        unit_play_t *p = (unit_play_t *)realloc(l->items, cap * sizeof *p);
        if (!p) return SPFY_E_NOMEM;
        l->items = p; l->cap = cap;
    }
    l->items[l->n++] = item;
    return SPFY_OK;
}

static const char *find_key_after(const char *p, const char *end, const char *key)
{
    size_t kn = strlen(key);
    char needle[32];
    if (kn + 3 >= sizeof needle) return NULL;
    needle[0] = '"';
    memcpy(needle + 1, key, kn);
    needle[kn + 1] = '"';
    needle[kn + 2] = ':';
    for (const char *q = p; q + (kn + 3) <= end; ++q) {
        if (memcmp(q, needle, kn + 3) == 0) return q + kn + 3;
    }
    return NULL;
}

static long strtol_at(const char *p, const char *end, char **endp)
{
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    return strtol(p, endp, 10);
}

/* For each `{"uid":N,"lp":...,"dl":M}` object in the JSONL, push (N,M). */
static int parse_play_list(const char *path, unit_play_list_t *out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return SPFY_E_IO;
    char buf[16384];
    while (fgets(buf, sizeof buf, fp)) {
        const char *p   = buf;
        const char *end = buf + strlen(buf);
        for (;;) {
            const char *up = find_key_after(p, end, "uid");
            if (!up) break;
            char *e = NULL;
            long uid = strtol_at(up, end, &e);
            if (e == up) break;
            const char *dp = find_key_after(e, end, "dl");
            if (!dp) { p = e; continue; }
            char *e2 = NULL;
            long dl = strtol_at(dp, end, &e2);
            if (e2 == dp) { p = e; continue; }
            unit_play_t it = {0};
            it.uid = (uint32_t)uid;
            it.dl  = (uint32_t)dl;
            int rc = play_list_push(out, it);
            if (rc != SPFY_OK) { fclose(fp); return rc; }
            p = e2;
        }
    }
    fclose(fp);
    return SPFY_OK;
}

static void annotate_play_list(unit_play_list_t *l, const spfy_unit_table_t *t)
{
    for (size_t i = 0; i < l->n; ++i) {
        unit_play_t *p = &l->items[i];
        if (p->uid == 0 || p->uid >= t->n_units) { p->valid = 0; continue; }
        spfy_unit_record_t r;
        if (spfy_unit_record_get(t, p->uid, &r) != SPFY_OK) {
            p->valid = 0; continue;
        }
        p->file_idx     = r.file_idx;
        p->local_pos    = r.local_pos;
        p->dur_like     = r.dur_like;
        p->phone_center = r.phone_center;
        p->is_first_half= r.is_first_half;
        p->valid = 1;
    }
}

/* Cross-fade state across calls to append_unit_audio. */
#define XFADE_MS      6u
#define XFADE_SAMPLES (XFADE_MS * 8u)

typedef struct {
    int16_t  tail[XFADE_SAMPLES];
    size_t   tail_n;
} xfade_state_t;

static void xfade_apply(xfade_state_t *xf, int16_t *samples, size_t n)
{
    size_t k = xf->tail_n;
    if (k == 0 || n == 0) return;
    if (k > n) k = n;
    for (size_t i = 0; i < k; ++i) {
        long double alpha = (long double)(i + 1) / (long double)(k + 1);
        long double beta  = 1.0L - alpha;
        long double mixed = beta * (long double)xf->tail[i]
                          + alpha * (long double)samples[i];
        if (mixed >  32767.0L) mixed =  32767.0L;
        if (mixed < -32768.0L) mixed = -32768.0L;
        samples[i] = (int16_t)mixed;
    }
}

static void xfade_save(xfade_state_t *xf, const int16_t *samples, size_t n)
{
    if (n >= XFADE_SAMPLES) {
        memcpy(xf->tail, samples + (n - XFADE_SAMPLES),
               XFADE_SAMPLES * sizeof *xf->tail);
        xf->tail_n = XFADE_SAMPLES;
    } else {
        memcpy(xf->tail, samples, n * sizeof *xf->tail);
        xf->tail_n = n;
    }
}

/* Decode (file_idx, lp_ms, dur_ms) into a freshly-allocated s16 buffer. */
static int decode_unit_samples(uint32_t file_idx,
                               uint32_t lp_ms, uint32_t dur_ms,
                               const spfy_feat_table_t *feat,
                               const spfy_vdb_t        *vdb,
                               const spfy_vdb_lookup_t *lookup,
                               int16_t **out_samples, size_t *out_n)
{
    *out_samples = NULL; *out_n = 0;
    if (dur_ms == 0) return SPFY_OK;
    if (file_idx >= feat->n_entries) return SPFY_E_OOB;
    const spfy_feat_entry_t *fe = &feat->entries[file_idx];

    uint32_t rec_offset = 0, rec_size = 0;
    int rc = spfy_vdb_lookup_by_name(lookup, fe->name, fe->name_len,
                                     &rec_offset, &rec_size);
    if (rc != SPFY_OK) return rc;

    uint32_t off_in_rec    = lp_ms * 8u;
    uint32_t bytes_to_play = dur_ms * 8u;
    /* The names say "bytes" because u-law made bytes and samples the same
     * thing. */
    uint32_t bps = vdb->bytes_per_sample ? vdb->bytes_per_sample : 1u;
    uint32_t rec_n = rec_size / bps;
    if (off_in_rec >= rec_n) return SPFY_OK;
    if (off_in_rec + bytes_to_play > rec_n)
        bytes_to_play = rec_n - off_in_rec;
    if (bytes_to_play == 0) return SPFY_OK;

    int16_t *buf = (int16_t *)malloc(bytes_to_play * sizeof *buf);
    if (!buf) return SPFY_E_NOMEM;
    spfy_vdb_decode(vdb, rec_offset, off_in_rec, bytes_to_play, buf);
    *out_samples = buf;
    *out_n       = bytes_to_play;
    return SPFY_OK;
}

static int write_with_xfade(spfy_wav_writer_t *wav, xfade_state_t *xf,
                            int16_t *samples, size_t n)
{
    if (n == 0) return SPFY_OK;
    xfade_apply(xf, samples, n);
    xfade_save (xf, samples, n);
    return spfy_wav_write(wav, samples, n);
}

static int append_recording_span(uint32_t file_idx,
                                 uint32_t lp_ms, uint32_t dur_ms,
                                 const spfy_feat_table_t *feat,
                                 const spfy_vdb_t        *vdb,
                                 const spfy_vdb_lookup_t *lookup,
                                 xfade_state_t *xf,
                                 spfy_wav_writer_t *wav)
{
    int16_t *buf = NULL; size_t n = 0;
    int rc = decode_unit_samples(file_idx, lp_ms, dur_ms, feat, vdb, lookup,
                                 &buf, &n);
    if (rc != SPFY_OK) return rc;
    rc = write_with_xfade(wav, xf, buf, n);
    free(buf);
    return rc;
}

/* Cross-rec pair: linear blend the two halves' audio over max(n1,n2)
 * samples. */
static int append_crossrec_pair(uint32_t f1, uint32_t lp1, uint32_t d1,
                                uint32_t f2, uint32_t lp2, uint32_t d2,
                                const spfy_feat_table_t *feat,
                                const spfy_vdb_t        *vdb,
                                const spfy_vdb_lookup_t *lookup,
                                xfade_state_t *xf,
                                spfy_wav_writer_t *wav)
{
    int16_t *a = NULL, *b = NULL;
    size_t   na = 0,   nb = 0;
    int rc = decode_unit_samples(f1, lp1, d1, feat, vdb, lookup, &a, &na);
    if (rc != SPFY_OK) goto out;
    rc = decode_unit_samples(f2, lp2, d2, feat, vdb, lookup, &b, &nb);
    if (rc != SPFY_OK) goto out;

    size_t n_out = na > nb ? na : nb;
    if (n_out == 0) goto out;

    int16_t *mix = (int16_t *)malloc(n_out * sizeof *mix);
    if (!mix) { rc = SPFY_E_NOMEM; goto out; }

    for (size_t i = 0; i < n_out; ++i) {
        long double alpha = (n_out > 1)
            ? (long double)i / (long double)(n_out - 1)
            : 0.0L;
        long double sa = (i < na) ? (long double)a[i] : 0.0L;
        long double sb = (i < nb) ? (long double)b[i] : 0.0L;
        long double m  = sa * (1.0L - alpha) + sb * alpha;
        if (m >  32767.0L) m =  32767.0L;
        if (m < -32768.0L) m = -32768.0L;
        mix[i] = (int16_t)m;
    }
    rc = write_with_xfade(wav, xf, mix, n_out);
    free(mix);

out:
    free(a); free(b);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr,
            "usage: %s <voice.vin> <voice.vdb> <wsola_in.jsonl> <out.wav>\n",
            argv[0]);
        return 2;
    }

    spfy_vin_t vin = {0};
    spfy_vdb_t vdb = {0};
    spfy_unit_table_t units = {0};
    spfy_feat_table_t feat  = {0};
    spfy_vdb_lookup_t lookup = {0};
    unit_play_list_t  plays  = {0};
    spfy_wav_writer_t wav    = {0};
    int rc;

    if ((rc = spfy_vin_load(argv[1], &vin)) != SPFY_OK) goto done;
    if ((rc = spfy_vdb_load(argv[2], &vdb)) != SPFY_OK) goto done;
    if ((rc = spfy_vdb_require_supported(&vdb, argv[2])) != SPFY_OK) goto done;
    if ((rc = spfy_unit_table_load(&vin, &units)) != SPFY_OK) goto done;
    if ((rc = spfy_feat_table_load(&vin, &feat))  != SPFY_OK) goto done;
    if ((rc = spfy_vdb_lookup_build(&vdb, &lookup)) != SPFY_OK) goto done;
    if ((rc = parse_play_list(argv[3], &plays)) != SPFY_OK) goto done;
    annotate_play_list(&plays, &units);

    fprintf(stdout, "voice: %u units, %zu recordings, sample_rate=%u Hz\n",
            units.n_units, vdb.n_indx_entries, vdb.sample_rate);
    fprintf(stdout, "trace: %zu units in playback order\n", plays.n);

    if ((rc = spfy_wav_open(&wav, argv[4], vdb.sample_rate)) != SPFY_OK) goto done;

    /* Pair-aware playback. */
    xfade_state_t xf = {{0}, 0};
    size_t played = 0, skipped = 0, paired_same_rec = 0, paired_cross_rec = 0;
    size_t i = 0;
    while (i < plays.n) {
        unit_play_t p = plays.items[i];
        if (!p.valid || p.uid == 0 || p.uid == SILENCE_SENTINEL_UID) {
            ++skipped; ++i; continue;
        }

        int paired = 0;
        if (i + 1 < plays.n) {
            unit_play_t q = plays.items[i + 1];
            if (q.valid && q.uid != 0 && q.uid != SILENCE_SENTINEL_UID
                && p.phone_center == q.phone_center) {
                paired = 1;
                if (p.file_idx == q.file_idx
                    && q.local_pos >= p.local_pos
                    && q.local_pos <= p.local_pos + p.dur_like + 64u) {
                    /* Same-recording pair: span from first.lp through
                     * second.lp + second.dur_like. */
                    uint32_t span_dur = (uint32_t)q.local_pos
                                      + (uint32_t)q.dur_like
                                      - (uint32_t)p.local_pos;
                    rc = append_recording_span(p.file_idx, p.local_pos,
                                               span_dur, &feat, &vdb,
                                               &lookup, &xf, &wav);
                    ++paired_same_rec;
                } else {
                    /* Cross-recording pair: linear-blend both halves so the
                     * first-half burst (critical for stop consonants /k/,
                     * /d/, /g/) is preserved while transitioning to the
                     * second-half content -- WSOLA-lite. */
                    uint32_t d_first  = (uint32_t)p.dur_like;
                    uint32_t d_second = q.dl ? q.dl : (uint32_t)q.dur_like;
                    rc = append_crossrec_pair(p.file_idx, p.local_pos, d_first,
                                              q.file_idx, q.local_pos, d_second,
                                              &feat, &vdb, &lookup, &xf, &wav);
                    ++paired_cross_rec;
                }
                if (rc != SPFY_OK) goto done;
                played += 2;
                i += 2;
                continue;
            }
        }
        if (!paired) {
            /* Standalone unit (no matching neighbor): play full natural
             * length, trace.dl preferred when present. */
            uint32_t dur = p.dl ? p.dl : (uint32_t)p.dur_like;
            rc = append_recording_span(p.file_idx, p.local_pos, dur,
                                       &feat, &vdb, &lookup, &xf, &wav);
            if (rc != SPFY_OK) goto done;
            ++played;
            ++i;
        }
    }
    rc = spfy_wav_close(&wav);

    fprintf(stdout, "wrote %s: %u s16 samples (%.2f s) "
                    "from %zu units (%zu pairs same-rec, %zu cross-rec, "
                    "%zu sentinel skipped)\n",
            argv[4], wav.n_samples_written,
            (double)wav.n_samples_written / (double)vdb.sample_rate,
            played, paired_same_rec, paired_cross_rec, skipped);

done:
    free(plays.items);
    spfy_vdb_lookup_free(&lookup);
    spfy_feat_table_free(&feat);
    spfy_vin_free(&vin);
    spfy_vdb_free(&vdb);
    if (rc != SPFY_OK) fprintf(stderr, "error: %s\n", spfy_strerror(rc));
    return rc == SPFY_OK ? 0 : 1;
}
