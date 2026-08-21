#ifndef SPFY_VB_IO_H
#define SPFY_VB_IO_H

#include <stddef.h>
#include <stdint.h>

/* I/O layer for the voice builder.
 *
 * Everything here exists because a stage needs it, and every reader is
 * deliberately a port of the Python tool that currently owns the format --
 * `vb_vin.py`, `vb_build1.py`, `vb_fe.py`, `vb_ckls.py` -- rather than a fresh
 * reading of the spec. The gate for the port is byte-equality of the emitted
 * chunks against those tools, so any place this diverges is a bug even when it
 * looks more correct. Divergences that ARE deliberate are commented as such. */

/* ---------------------------------------------------------------------- */
/* Growable byte buffer.                                                    */

typedef struct {
    uint8_t *p;
    size_t   n;
    size_t   cap;
} spfy_vb_buf;

int  spfy_vb_buf_reserve(spfy_vb_buf *b, size_t need);
int  spfy_vb_buf_put (spfy_vb_buf *b, const void *src, size_t n);
int  spfy_vb_buf_u8  (spfy_vb_buf *b, uint8_t  v);
int  spfy_vb_buf_u16 (spfy_vb_buf *b, uint16_t v);
int  spfy_vb_buf_u32 (spfy_vb_buf *b, uint32_t v);
int  spfy_vb_buf_f32 (spfy_vb_buf *b, float    v);
/* u16 length prefix then the bytes, no terminator -- the string form every
 * VIN chunk uses. */
int  spfy_vb_buf_pstr(spfy_vb_buf *b, const char *s);
void spfy_vb_buf_free(spfy_vb_buf *b);

/* ---------------------------------------------------------------------- */
/* In-memory RIFF, XOR 0xCE over the whole file.                            */
/*                                                                          */
/* spfy_vin_load in src/voice is read-only and resolves the chunks the       */
/* ENGINE wants. A builder has to rewrite chunks and re-emit the container,  */
/* and it must preserve two things a naive writer drops: the declared size   */
/* (tom.vin's disagrees with its real length) and each chunk's pad byte.     */

typedef struct {
    char     id[5];
    uint8_t *data;
    size_t   n;
    uint8_t  pad;        /* the original pad byte, when the body was odd   */
    int      has_pad;
    int      owned;      /* data was malloc'd here, not aliased into raw   */
} spfy_vb_chunk;

typedef struct {
    uint32_t       declared;
    char           form[5];
    spfy_vb_chunk *ch;
    size_t         n_ch, cap_ch;
    uint8_t       *raw;      /* the decoded file; chunk bodies alias it    */
    size_t         raw_n;
} spfy_vb_riff;

int  spfy_vb_riff_load(const char *path, spfy_vb_riff *r);
/* An EMPTY container of the given form ("svin" / "WAVE"), for a build that
 * has no donor to patch. Chunks are then added with spfy_vb_riff_put in the
 * order the vendors ship, and spfy_vb_riff_set replaces them as usual. */
int  spfy_vb_riff_new(spfy_vb_riff *r, const char *form);
/* Appends a chunk, taking ownership of `data` (NULL/0 makes an empty one to
 * be filled in later by _set). Fails if the id is already present -- two
 * chunks with one id is a container the engine reads inconsistently. */
int  spfy_vb_riff_put(spfy_vb_riff *r, const char *id, uint8_t *data, size_t n);
const spfy_vb_chunk *spfy_vb_riff_get(const spfy_vb_riff *r, const char *id);
/* Replaces the body, taking ownership of `data`. Returns SPFY_E_FORMAT when
 * the container has no such chunk -- deliberately NOT an append, because
 * every chunk a build writes already exists in the template and a silent
 * append would put it in the wrong place. */
int  spfy_vb_riff_set (spfy_vb_riff *r, const char *id, uint8_t *data, size_t n);
int  spfy_vb_riff_save(spfy_vb_riff *r, const char *path);
void spfy_vb_riff_free(spfy_vb_riff *r);

/* Iterate the sub-chunks inside a chunk body (unit -> vers/data, durt ->
 * tree/ques, ...). Returns 1 and advances *pos, or 0 at the end. */
int  spfy_vb_subchunk(const uint8_t *body, size_t n, size_t *pos,
                      char id_out[5], const uint8_t **data, size_t *data_n);

/* ---------------------------------------------------------------------- */
/* Audio.                                                                   */

typedef struct {
    int16_t *pcm;
    size_t   n_samples;
    int      rate;
    int      channels;
    int      width;
} spfy_vb_wav;

int  spfy_vb_wav_read(const char *path, spfy_vb_wav *out);
void spfy_vb_wav_free(spfy_vb_wav *w);

/* G.711 u-law encode.
 *
 * ⚠ This is CPython's `audioop.lin2ulaw`, not the textbook encoder, because
 * the gate is byte-equality with the Python builder's VDB. audioop shifts the
 * 16-bit sample right by 2 and encodes the resulting 14-bit value, and it
 * clips at 8159 AFTER that shift. A conventional 16-bit encoder differs on
 * roughly a third of samples. */
uint8_t spfy_vb_ulaw_encode(int16_t pcm);
void    spfy_vb_ulaw_encode_block(const int16_t *pcm, size_t n, uint8_t *out);

/* ---------------------------------------------------------------------- */
/* TextGrid: the phones tier, as [start_s, end_s, label].                   */

typedef struct {
    double start, end;
    char   label[32];
} spfy_vb_interval;

int  spfy_vb_textgrid_phones(const char *path, spfy_vb_interval **out,
                             size_t *out_n);

/* ---------------------------------------------------------------------- */
/* The FE's tagged output.                                                  */

/* parse_block(keep_pau=True) with the BREAK markers dropped, which is
 * exactly what vb_build1 asks for. `pau` is a phone here; `xx` contributes
 * nothing. Returns phone NAME indices into a caller-supplied table is not
 * worth the indirection -- the names are short, so they are copied. */
typedef struct {
    char (*ph)[8];
    size_t n;
} spfy_vb_phones;

int  spfy_vb_fe_phones(const char *text, spfy_vb_phones *out);
void spfy_vb_phones_free(spfy_vb_phones *p);

/* parse_spans: words and syllables as [text, first_phone_idx, last_phone_idx]
 * over the SAME phone numbering `spfy_vb_fe_phones` produces. */
typedef struct {
    char    *text;
    uint32_t first, last;
} spfy_vb_span;

typedef struct {
    spfy_vb_span *v;
    size_t        n, cap;
} spfy_vb_spans;

int  spfy_vb_fe_spans(const char *text, spfy_vb_spans *words,
                      spfy_vb_spans *syls);
void spfy_vb_spans_free(spfy_vb_spans *s);

/* ---------------------------------------------------------------------- */
/* vb_spcap's .sp sidecar: the engine's own per-slot targets.               */

typedef struct {
    int32_t ctx[8];
    int32_t sp[8];
    uint8_t n_ctx, n_sp;
} spfy_vb_slot;

int  spfy_vb_sp_read(const char *path, spfy_vb_slot **out, size_t *out_n);

/* ---------------------------------------------------------------------- */
/* vb_segcap's .seg sidecar: the engine's own half-phone boundaries.        */

typedef struct {
    int32_t phone;       /* requested phone id, -1 when the slot is unusable */
    int32_t lo, mid, hi; /* ms                                               */
    uint8_t ok;
} spfy_vb_segent;

/* `ph_of` is the FE-requested phone per position INCLUDING unusable ones, so
 * a neighbour's context still sees a dropped phone. Both arrays are
 * (*out_n) long. */
int  spfy_vb_seg_read(const char *path, spfy_vb_segent **out, size_t *out_n,
                      size_t *n_bad);

/* ---------------------------------------------------------------------- */
/* Misc.                                                                    */

int  spfy_vb_read_text(const char *path, char **out, size_t *out_n);
int  spfy_vb_read_bytes(const char *path, uint8_t **out, size_t *out_n);
int  spfy_vb_file_exists(const char *path);
/* Sorted list of *.wav stems in dir. */
int  spfy_vb_list_stems(const char *dir, char ***out, size_t *out_n);
void spfy_vb_free_stems(char **v, size_t n);

#endif
