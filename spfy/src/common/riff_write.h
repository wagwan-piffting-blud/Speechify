#ifndef SPFY_COMMON_RIFF_WRITE_H
#define SPFY_COMMON_RIFF_WRITE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "obfuscation.h"

/* RIFF writer — reconstruction of SWIttsRiffWriter (SWIttsEngineUtil.dll
 * ordinals 2/4/7/9/12/16/29-44, image base 0x06b40000).
 *
 * The shipped runtime imports only SWIttsRiffReader; nothing imports a single
 * writer symbol. The writer is in the DLL because ttsEngine/src/util was
 * shared with the offline voice compiler, which is the only thing that ever
 * called it. Contract recovered by decompilation — see
 * reveng/DLL_ANALYSIS.md section 9.
 *
 * Three details a naive implementation gets wrong:
 *   - Every byte goes through the cipher, including FOURCCs, chunk sizes and
 *     pad bytes. An encrypted container has no plaintext header, and its pad
 *     bytes read as 0xCE on disk rather than 0x00.
 *   - closeChunk pads to even AFTER computing the size, so the size field
 *     excludes the pad.
 *   - Closing the outermost chunk closes the file. */

#define SPFY_RIFF_MAX_DEPTH 16

typedef enum {
    SPFY_RIFF_PLAIN = 0,
    SPFY_RIFF_CE    = 1   /* buf[i] ^= SPFY_OBFUSCATION_BYTE */
} spfy_riff_enc;

typedef struct {
    FILE          *fp;
    spfy_riff_enc  enc;
    long           pos;                          /* absolute file offset */
    int            depth;
    long           start[SPFY_RIFF_MAX_DEPTH];   /* header offset per open chunk */
} spfy_riff_writer;

/* Opens path, emits "RIFF" + size placeholder + form. */
int spfy_riff_create(spfy_riff_writer *w, const char *path,
                     const char *form, spfy_riff_enc enc);

/* id must be exactly 4 chars, each alphanumeric or space. */
int spfy_riff_open_chunk (spfy_riff_writer *w, const char *id);
int spfy_riff_close_chunk(spfy_riff_writer *w);

int spfy_riff_write_bytes (spfy_riff_writer *w, const void *p, size_t n);
int spfy_riff_write_u8    (spfy_riff_writer *w, uint8_t v);
int spfy_riff_write_u16   (spfy_riff_writer *w, uint16_t v);
int spfy_riff_write_u32   (spfy_riff_writer *w, uint32_t v);
int spfy_riff_write_f32   (spfy_riff_writer *w, float v);
int spfy_riff_write_fourcc(spfy_riff_writer *w, const char *id);

/* writeStringZ: bytes + NUL, no length prefix. */
int spfy_riff_write_str_z(spfy_riff_writer *w, const char *s);

/* writeStringW: uint16 LE length, then bytes, NO terminator. The vendor's "W"
 * is word-prefixed, not wide — the mangled parameter is char const *. */
int spfy_riff_write_str_w(spfy_riff_writer *w, const char *s);

/* LIST/INFO carrying ICOP and ICRD, as writeInfoChunk emits them.
 * The vendor formats both from localtime() at emit time; year and date are
 * explicit here so builds are reproducible and a round trip can reproduce a
 * vendor file's stamp exactly. date must be "YYYY-MM-DD". */
int spfy_riff_write_info(spfy_riff_writer *w, int year, const char *date);

/* Closes any chunks still open, then the file. Idempotent. */
int spfy_riff_finish(spfy_riff_writer *w);

#endif
