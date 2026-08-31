#ifndef SPFY_WSOLA_WAV_H
#define SPFY_WSOLA_WAV_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Streaming s16 PCM sink used by the WSOLA streamer. */

/* Callback signature for streaming sinks. */
typedef int (*spfy_wav_write_fn)(void *ctx, const int16_t *samples,
                                 size_t n_samples);

/* One run of output samples carrying its own pitch/rate treatment.
 *
 * This is how SSML <prosody pitch> and <prosody rate> become audible rather
 * than merely selected-for. spfy_sapi.c has always done exactly this -- see
 * sapi_psola_flush() -- but it did it INSIDE the SAPI DLL, per SAPI fragment,
 * so the CLI, the WASM build and the web demo never got it.
 *
 * `start` is an offset in PRE-processing samples; a span runs to the next
 * span's `start`, and the last runs to end of stream. */
typedef struct {
    uint32_t start;
    float    psola_st;      /* residual pitch shift in semitones, 0 = none  */
    float    rate;          /* time-scale, >1 = faster/shorter, 1 = none    */
} spfy_wav_span_t;

typedef struct {
    /* Mutually exclusive: fp set for file mode, write_cb set for callback
     * mode. */
    FILE             *fp;
    spfy_wav_write_fn write_cb;
    void             *cb_ctx;
    uint32_t          sample_rate;
    uint32_t          n_samples_written;
    /* Optional whole-utterance time-scale, applied at close() so callers
     * see one artefact instead of a post-process step. */
    float             stretch;
    int16_t          *sbuf;
    size_t            sbuf_n, sbuf_cap;
    /* Per-span pitch/rate, applied at close() BEFORE `stretch`. */
    spfy_wav_span_t  *spans;
    size_t            n_spans;
    int               hold;      /* buffer even with stretch == 1 */
} spfy_wav_writer_t;

/* Set the whole-utterance time-scale factor. */
void spfy_wav_set_stretch(spfy_wav_writer_t *w, float factor);

/* Start holding output so per-span DSP is possible at close().
 *
 * ⚠ MUST be called BEFORE the first spfy_wav_write(), because a sample that
 * has already gone to the file or the callback cannot be pitch-shifted
 * afterwards. The caller normally knows a span exists (a \!pp or \!wp tag is
 * present) well before it knows WHERE the spans fall, which is why arming and
 * describing are two calls. */
void spfy_wav_hold(spfy_wav_writer_t *w);

/* Hand over the span list. Takes a COPY; `spans` must be sorted by `start`.
 * A no-op unless spfy_wav_hold() armed the buffer. */
int  spfy_wav_set_spans(spfy_wav_writer_t *w,
                        const spfy_wav_span_t *spans, size_t n);

int  spfy_wav_open (spfy_wav_writer_t *w, const char *path,
                    uint32_t sample_rate);
int  spfy_wav_open_callback(spfy_wav_writer_t *w,
                            spfy_wav_write_fn cb, void *ctx,
                            uint32_t sample_rate);
int  spfy_wav_write(spfy_wav_writer_t *w, const int16_t *samples, size_t n);
int  spfy_wav_close(spfy_wav_writer_t *w);

#endif
