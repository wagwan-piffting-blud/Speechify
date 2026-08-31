#include "wav.h"
#include "../../include/spfy/spfy.h"
#include "../dsp/time_stretch.h"
#include "../dsp/pitch_shift.h"

#include <stdlib.h>
#include <string.h>

static void put_le_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v       & 0xFF);
    p[1] = (uint8_t)((v >> 8 )& 0xFF);
    p[2] = (uint8_t)((v >> 16)& 0xFF);
    p[3] = (uint8_t)((v >> 24)& 0xFF);
}

static void put_le_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v       & 0xFF);
    p[1] = (uint8_t)((v >> 8)& 0xFF);
}

static int write_header(spfy_wav_writer_t *w, uint32_t n_samples)
{
    uint8_t hdr[44];
    uint32_t data_size = n_samples * 2u;
    uint32_t riff_size = data_size + 36u;
    memcpy(hdr + 0,  "RIFF", 4);
    put_le_u32(hdr + 4, riff_size);
    memcpy(hdr + 8,  "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    put_le_u32(hdr + 16, 16u);
    put_le_u16(hdr + 20, 1u);
    put_le_u16(hdr + 22, 1u);
    put_le_u32(hdr + 24, w->sample_rate);
    put_le_u32(hdr + 28, w->sample_rate * 2u);
    put_le_u16(hdr + 32, 2u);
    put_le_u16(hdr + 34, 16u);
    memcpy(hdr + 36, "data", 4);
    put_le_u32(hdr + 40, data_size);
    if (fseek(w->fp, 0, SEEK_SET) != 0) return SPFY_E_IO;
    if (fwrite(hdr, 1, 44, w->fp) != 44u) return SPFY_E_IO;
    return SPFY_OK;
}

int spfy_wav_open(spfy_wav_writer_t *w, const char *path, uint32_t sample_rate)
{
    if (!w || !path) return SPFY_E_INVAL;
    memset(w, 0, sizeof *w);
    w->fp = fopen(path, "wb+");
    if (!w->fp) return SPFY_E_IO;
    w->sample_rate = sample_rate;
    return write_header(w, 0);
}

int spfy_wav_open_callback(spfy_wav_writer_t *w, spfy_wav_write_fn cb,
                           void *ctx, uint32_t sample_rate)
{
    if (!w || !cb) return SPFY_E_INVAL;
    memset(w, 0, sizeof *w);
    w->write_cb    = cb;
    w->cb_ctx      = ctx;
    w->sample_rate = sample_rate;
    return SPFY_OK;
}

void spfy_wav_set_stretch(spfy_wav_writer_t *w, float factor)
{
    if (!w) return;
    w->stretch = (factor > 0.0f) ? factor : 0.0f;
}

void spfy_wav_hold(spfy_wav_writer_t *w)
{
    if (w) w->hold = 1;
}

int spfy_wav_set_spans(spfy_wav_writer_t *w,
                       const spfy_wav_span_t *spans, size_t n)
{
    if (!w) return SPFY_E_INVAL;
    free(w->spans);
    w->spans = NULL;
    w->n_spans = 0;
    if (!spans || n == 0) return SPFY_OK;
    w->spans = (spfy_wav_span_t *)malloc(n * sizeof *w->spans);
    if (!w->spans) return SPFY_E_NOMEM;
    memcpy(w->spans, spans, n * sizeof *w->spans);
    w->n_spans = n;
    return SPFY_OK;
}

/* Pitch-shift then time-scale one span, appending to *acc.
 *
 * Order matters and matches sapi_psola_flush(): PSOLA first, WSOLA second.
 * The other way round, the time-scale changes the period spacing the pitch
 * shifter is about to measure, and the shift comes out wrong in proportion
 * to the stretch. */
static int span_process(const int16_t *in, size_t n, float psola_st, float rate,
                        int sr, int16_t **acc, size_t *acc_n, size_t *acc_cap)
{
    const int16_t *cur = in;
    size_t cur_n = n;
    int16_t *owned = NULL;

    if (psola_st != 0.0f && n) {
        int16_t *sh = (int16_t *)malloc(n * sizeof *sh);
        if (!sh) return SPFY_E_NOMEM;
        if (spfy_pitch_shift_block(cur, cur_n, sh, psola_st, sr) != 0) {
            /* A span the shifter cannot handle (too short to find a period)
             * is passed through unshifted rather than dropped -- a gap in
             * the audio is a far worse failure than a flat span. */
            free(sh);
        } else {
            cur = sh; owned = sh;
        }
    }
    if (rate > 0.0f && rate != 1.0f && cur_n) {
        int16_t *st = NULL;
        size_t   st_n = 0;
        if (spfy_time_stretch_block(cur, cur_n, &st, &st_n, rate, sr) == 0
            && st && st_n) {
            free(owned);
            cur = st; owned = st; cur_n = st_n;
        } else {
            free(st);
        }
    }

    if (*acc_n + cur_n > *acc_cap) {
        size_t cap = *acc_cap ? *acc_cap : 65536u;
        while (cap < *acc_n + cur_n) cap *= 2u;
        int16_t *nb = (int16_t *)realloc(*acc, cap * sizeof *nb);
        if (!nb) { free(owned); return SPFY_E_NOMEM; }
        *acc = nb;
        *acc_cap = cap;
    }
    memcpy(*acc + *acc_n, cur, cur_n * sizeof *cur);
    *acc_n += cur_n;
    free(owned);
    return SPFY_OK;
}

static int wav_emit(spfy_wav_writer_t *w, const int16_t *samples, size_t n)
{
    if (w->fp) {
        if (fwrite(samples, sizeof *samples, n, w->fp) != n)
            return SPFY_E_IO;
    } else if (w->write_cb) {
        int rc = w->write_cb(w->cb_ctx, samples, n);
        if (rc != SPFY_OK) return rc;
    } else {
        return SPFY_E_INVAL;
    }
    w->n_samples_written += (uint32_t)n;
    return SPFY_OK;
}

int spfy_wav_write(spfy_wav_writer_t *w, const int16_t *samples, size_t n)
{
    if (!w) return SPFY_E_INVAL;
    if (n == 0) return SPFY_OK;
    if (w->hold || (w->stretch > 0.0f && w->stretch != 1.0f)) {
        /* Hold the whole utterance; WSOLA needs contiguous input to find
         * its correlation peaks, so stretching per burst would seam. */
        if (w->sbuf_n + n > w->sbuf_cap) {
            size_t cap = w->sbuf_cap ? w->sbuf_cap * 2u : 65536u;
            while (cap < w->sbuf_n + n) cap *= 2u;
            int16_t *nb = (int16_t *)realloc(w->sbuf, cap * sizeof *nb);
            if (!nb) return SPFY_E_NOMEM;
            w->sbuf = nb;
            w->sbuf_cap = cap;
        }
        memcpy(w->sbuf + w->sbuf_n, samples, n * sizeof *samples);
        w->sbuf_n += n;
        /* n_samples_written stays in PRE-stretch time here; callers that
         * report positions (word/phrase events) scale by 1/stretch. */
        w->n_samples_written += (uint32_t)n;
        return SPFY_OK;
    }
    if (w->fp) {
        if (fwrite(samples, sizeof *samples, n, w->fp) != n)
            return SPFY_E_IO;
    } else if (w->write_cb) {
        int rc = w->write_cb(w->cb_ctx, samples, n);
        if (rc != SPFY_OK) return rc;
    } else {
        return SPFY_E_INVAL;
    }
    w->n_samples_written += (uint32_t)n;
    return SPFY_OK;
}

int spfy_wav_close(spfy_wav_writer_t *w)
{
    if (!w) return SPFY_E_INVAL;
    if (w->sbuf && w->sbuf_n) {
        int rc = SPFY_OK;

        /* Per-span pitch/rate first, so `stretch` (the whole-utterance
         * SPFY_RATE knob) still means what it always meant: a scale on the
         * finished utterance, spans included. */
        if (w->n_spans) {
            int16_t *acc = NULL;
            size_t   acc_n = 0, acc_cap = 0;
            for (size_t i = 0; i < w->n_spans && rc == SPFY_OK; ++i) {
                size_t a = w->spans[i].start;
                size_t b = (i + 1 < w->n_spans) ? w->spans[i + 1].start
                                                : w->sbuf_n;
                if (a > w->sbuf_n) a = w->sbuf_n;
                if (b > w->sbuf_n) b = w->sbuf_n;
                if (b <= a) continue;
                rc = span_process(w->sbuf + a, b - a,
                                  w->spans[i].psola_st, w->spans[i].rate,
                                  (int)w->sample_rate,
                                  &acc, &acc_n, &acc_cap);
            }
            if (rc == SPFY_OK && acc) {
                free(w->sbuf);
                w->sbuf = acc;
                w->sbuf_n = acc_n;
                w->sbuf_cap = acc_cap;
            } else {
                free(acc);
            }
        }

        w->n_samples_written = 0;
        if (rc == SPFY_OK && w->stretch > 0.0f && w->stretch != 1.0f) {
            int16_t *out = NULL;
            size_t out_n = 0;
            int trc = spfy_time_stretch_block(w->sbuf, w->sbuf_n, &out, &out_n,
                                              w->stretch, (int)w->sample_rate);
            if (trc == 0 && out && out_n) rc = wav_emit(w, out, out_n);
            else                          rc = wav_emit(w, w->sbuf, w->sbuf_n);
            free(out);
        } else if (rc == SPFY_OK) {
            rc = wav_emit(w, w->sbuf, w->sbuf_n);
        }
        free(w->sbuf);
        w->sbuf = NULL;
        w->sbuf_n = w->sbuf_cap = 0;
        free(w->spans);
        w->spans = NULL;
        w->n_spans = 0;
        if (rc != SPFY_OK && w->fp) { fclose(w->fp); w->fp = NULL; return rc; }
    }
    free(w->spans);
    w->spans = NULL;
    w->n_spans = 0;
    if (w->fp) {
        int rc = write_header(w, w->n_samples_written);
        if (fclose(w->fp) != 0 && rc == SPFY_OK) rc = SPFY_E_IO;
        w->fp = NULL;
        return rc;
    }
    w->write_cb = NULL;
    w->cb_ctx   = NULL;
    return SPFY_OK;
}
