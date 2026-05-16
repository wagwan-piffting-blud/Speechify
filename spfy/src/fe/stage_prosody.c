/* Stage 5: Prosody-hint propagation.
 *
 * Climbs prosody tags from %text -> %word -> %syl -> %phoneme via
 * byte-range overlap.
 */

#include "stage_prosody.h"
#include "stage_textnorm.h"
#include "fe.h"
#include "stream.h"

#include <spfy/spfy.h>

#include <stdlib.h>
#include <string.h>

/* Reduce-functions: max for emphasis (unsigned 0..3),
 * max-abs (signed) for pitch and rate. */
static uint16_t reduce_emph(uint16_t a, uint16_t b)
{
    return a > b ? a : b;
}

static int16_t reduce_signed_maxabs(int16_t a, int16_t b)
{
    int16_t aa = a < 0 ? (int16_t)-a : a;
    int16_t bb = b < 0 ? (int16_t)-b : b;
    return aa >= bb ? a : b;
}

int spfy_fe_prosody_run(const spfy_fe_t *fe,
                        spfy_fe_delta_t  *delta)
{
    (void)fe;
    if (!delta) return SPFY_E_INVAL;

    uint32_t n_text = 0;
    const spfy_fe_token_t *xt =
        spfy_fe_stream_tokens(delta, SPFY_STREAM_TEXT, &n_text);

    uint32_t n_word = 0;
    spfy_fe_token_t *xw =
        (spfy_fe_token_t *)spfy_fe_stream_tokens(delta, SPFY_STREAM_WORD,
                                                  &n_word);
    uint32_t n_syl = 0;
    spfy_fe_token_t *xs =
        (spfy_fe_token_t *)spfy_fe_stream_tokens(delta, SPFY_STREAM_SYL,
                                                  &n_syl);
    uint32_t n_phon = 0;
    spfy_fe_token_t *xp =
        (spfy_fe_token_t *)spfy_fe_stream_tokens(delta, SPFY_STREAM_PHONEME,
                                                  &n_phon);

    /* Build a per-byte indexed map of prosody values from %text.
     * %text fields[2] = original byte_off; fields[3] = emphasis;
     * fields[4] = pitch_st; fields[5] = rate_pct. We'll index by
     * byte_off into a flat array sized by max byte offset seen. */
    uint16_t max_byte_off = 0;
    for (uint32_t i = 0; i < n_text; ++i) {
        uint16_t off = xt[i].fields[SPFY_TEXT_FIELD_BYTE_OFF];
        if (off > max_byte_off) max_byte_off = off;
    }
    uint32_t map_n = (uint32_t)max_byte_off + 1;
    uint8_t *byte_emph  = (uint8_t  *)calloc(map_n, sizeof *byte_emph);
    int16_t *byte_pitch = (int16_t  *)calloc(map_n, sizeof *byte_pitch);
    int16_t *byte_rate  = (int16_t  *)calloc(map_n, sizeof *byte_rate);
    if (!byte_emph || !byte_pitch || !byte_rate) {
        free(byte_emph); free(byte_pitch); free(byte_rate);
        return SPFY_E_NOMEM;
    }
    for (uint32_t i = 0; i < n_text; ++i) {
        uint16_t off = xt[i].fields[SPFY_TEXT_FIELD_BYTE_OFF];
        if (off >= map_n) continue;
        byte_emph[off]  = (uint8_t)xt[i].fields[SPFY_TEXT_FIELD_EMPHASIS];
        byte_pitch[off] = (int16_t)xt[i].fields[SPFY_TEXT_FIELD_PITCH_ST];
        byte_rate[off]  = (int16_t)xt[i].fields[SPFY_TEXT_FIELD_RATE_PCT];
    }

    /* Reduce a byte range [lo, lo+len) into per-token prosody fields. */
    #define REDUCE_RANGE(tok, lo, len)                                     \
        do {                                                               \
            uint16_t emph = 0;                                             \
            int16_t  ptch = 0, rate = 0;                                   \
            for (uint32_t k = 0; k < (len); ++k) {                         \
                uint32_t b = (lo) + k;                                     \
                if (b >= map_n) break;                                     \
                emph = reduce_emph(emph, byte_emph[b]);                    \
                ptch = reduce_signed_maxabs(ptch, byte_pitch[b]);          \
                rate = reduce_signed_maxabs(rate, byte_rate[b]);           \
            }                                                              \
            (tok).fields[SPFY_PROSODY_FIELD_EMPHASIS] = emph;              \
            (tok).fields[SPFY_PROSODY_FIELD_PITCH_ST] = (uint16_t)ptch;    \
            (tok).fields[SPFY_PROSODY_FIELD_RATE_PCT] = (uint16_t)rate;    \
        } while (0)

    /* %word: fields[0]=byte_start, fields[1]=byte_len. */
    for (uint32_t i = 0; i < n_word; ++i) {
        uint16_t off = xw[i].fields[0];
        uint16_t len = xw[i].fields[1];
        REDUCE_RANGE(xw[i], off, len);
    }

    /* %syl: fields[0]=byte_start, fields[1]=byte_len. */
    for (uint32_t i = 0; i < n_syl; ++i) {
        uint16_t off = xs[i].fields[0];
        uint16_t len = xs[i].fields[1];
        REDUCE_RANGE(xs[i], off, len);
    }

    /* %phoneme: inherit from parent %syl (linked via syl_id). */
    for (uint32_t i = 0; i < n_phon; ++i) {
        uint16_t sid = xp[i].syl_id;
        if (sid < n_syl) {
            xp[i].fields[SPFY_PROSODY_FIELD_EMPHASIS] =
                xs[sid].fields[SPFY_PROSODY_FIELD_EMPHASIS];
            xp[i].fields[SPFY_PROSODY_FIELD_PITCH_ST] =
                xs[sid].fields[SPFY_PROSODY_FIELD_PITCH_ST];
            xp[i].fields[SPFY_PROSODY_FIELD_RATE_PCT] =
                xs[sid].fields[SPFY_PROSODY_FIELD_RATE_PCT];
        }
    }

    free(byte_emph); free(byte_pitch); free(byte_rate);
    #undef REDUCE_RANGE
    return SPFY_OK;
}
