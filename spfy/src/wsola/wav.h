#ifndef SPFY_WSOLA_WAV_H
#define SPFY_WSOLA_WAV_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Streaming s16 PCM sink used by the WSOLA streamer. */

/* Callback signature for streaming sinks. */
typedef int (*spfy_wav_write_fn)(void *ctx, const int16_t *samples,
                                 size_t n_samples);

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
} spfy_wav_writer_t;

/* Set the whole-utterance time-scale factor. */
void spfy_wav_set_stretch(spfy_wav_writer_t *w, float factor);

int  spfy_wav_open (spfy_wav_writer_t *w, const char *path,
                    uint32_t sample_rate);
int  spfy_wav_open_callback(spfy_wav_writer_t *w,
                            spfy_wav_write_fn cb, void *ctx,
                            uint32_t sample_rate);
int  spfy_wav_write(spfy_wav_writer_t *w, const int16_t *samples, size_t n);
int  spfy_wav_close(spfy_wav_writer_t *w);

#endif
