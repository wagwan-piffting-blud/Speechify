#include "riff_write.h"

#include "../../include/spfy/spfy.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define STAGE_BYTES 4096

static int raw_write(spfy_riff_writer *w, const void *p, size_t n)
{
    const uint8_t *src = (const uint8_t *)p;

    if (w->enc != SPFY_RIFF_CE) {
        if (fwrite(src, 1, n, w->fp) != n) return SPFY_E_IO;
        return SPFY_OK;
    }

    /* Staged so the caller's const buffer is never mutated. The vendor uses
     * a 4096-byte stack buffer here for the same reason. */
    uint8_t stage[STAGE_BYTES];
    size_t done = 0;
    while (done < n) {
        size_t take = n - done;
        if (take > STAGE_BYTES) take = STAGE_BYTES;
        for (size_t i = 0; i < take; ++i)
            stage[i] = (uint8_t)(src[done + i] ^ SPFY_OBFUSCATION_BYTE);
        if (fwrite(stage, 1, take, w->fp) != take) return SPFY_E_IO;
        done += take;
    }
    return SPFY_OK;
}

int spfy_riff_write_bytes(spfy_riff_writer *w, const void *p, size_t n)
{
    if (!w || !w->fp || (!p && n)) return SPFY_E_INVAL;
    if (!n) return SPFY_OK;

    int rc = raw_write(w, p, n);
    if (rc != SPFY_OK) return rc;
    w->pos += (long)n;
    return SPFY_OK;
}

int spfy_riff_write_u8(spfy_riff_writer *w, uint8_t v)
{
    return spfy_riff_write_bytes(w, &v, 1);
}

int spfy_riff_write_u16(spfy_riff_writer *w, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v & 0xFFu), (uint8_t)((v >> 8) & 0xFFu) };
    return spfy_riff_write_bytes(w, b, 2);
}

int spfy_riff_write_u32(spfy_riff_writer *w, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v & 0xFFu),         (uint8_t)((v >> 8)  & 0xFFu),
                     (uint8_t)((v >> 16) & 0xFFu), (uint8_t)((v >> 24) & 0xFFu) };
    return spfy_riff_write_bytes(w, b, 4);
}

int spfy_riff_write_f32(spfy_riff_writer *w, float v)
{
    /* writeFloat and writeDWord share one address in the vendor DLL: floats
     * are stored as raw 32-bit LE, byte-copied, with no conversion. */
    union { float f; uint32_t u; } cv;
    cv.f = v;
    return spfy_riff_write_u32(w, cv.u);
}

static int valid_fourcc(const char *id)
{
    if (!id) return 0;
    for (int i = 0; i < 4; ++i) {
        unsigned char c = (unsigned char)id[i];
        if (!c) return 0;
        if (!isalnum(c) && c != ' ') return 0;
    }
    return id[4] == '\0';
}

int spfy_riff_write_fourcc(spfy_riff_writer *w, const char *id)
{
    if (!valid_fourcc(id)) return SPFY_E_INVAL;
    return spfy_riff_write_bytes(w, id, 4);
}

int spfy_riff_write_str_z(spfy_riff_writer *w, const char *s)
{
    if (!s) return SPFY_E_INVAL;
    return spfy_riff_write_bytes(w, s, strlen(s) + 1);
}

int spfy_riff_write_str_w(spfy_riff_writer *w, const char *s)
{
    if (!s) return SPFY_E_INVAL;
    size_t n = strlen(s);
    if (n >= 0x10000u) return SPFY_E_INVAL;

    int rc = spfy_riff_write_u16(w, (uint16_t)n);
    if (rc != SPFY_OK || !n) return rc;
    return spfy_riff_write_bytes(w, s, n);
}

int spfy_riff_open_chunk(spfy_riff_writer *w, const char *id)
{
    if (!w || !w->fp) return SPFY_E_INVAL;
    if (!valid_fourcc(id)) return SPFY_E_INVAL;
    if (w->depth >= SPFY_RIFF_MAX_DEPTH) return SPFY_E_OOB;

    w->start[w->depth] = w->pos;

    int rc = spfy_riff_write_bytes(w, id, 4);
    if (rc != SPFY_OK) return rc;
    rc = spfy_riff_write_u32(w, 0);        /* size placeholder */
    if (rc != SPFY_OK) return rc;

    ++w->depth;
    return SPFY_OK;
}

int spfy_riff_close_chunk(spfy_riff_writer *w)
{
    if (!w || !w->fp) return SPFY_E_INVAL;
    if (w->depth <= 0) return SPFY_E_INVAL;

    long start = w->start[--w->depth];
    long end   = w->pos;

    /* Pad AFTER fixing end, so the size excludes it. The pad goes through
     * write_bytes, so in an encrypted container it lands as 0xCE. */
    if (end & 1L) {
        int rc = spfy_riff_write_u8(w, 0);
        if (rc != SPFY_OK) return rc;
    }

    long saved = w->pos;
    if (fseek(w->fp, start + 4, SEEK_SET) != 0) return SPFY_E_IO;

    int rc = spfy_riff_write_u32(w, (uint32_t)(end - start - 8));
    if (rc != SPFY_OK) return rc;

    if (fseek(w->fp, saved, SEEK_SET) != 0) return SPFY_E_IO;
    w->pos = saved;

    if (w->depth == 0) {
        if (fclose(w->fp) != 0) { w->fp = NULL; return SPFY_E_IO; }
        w->fp = NULL;
    }
    return SPFY_OK;
}

int spfy_riff_create(spfy_riff_writer *w, const char *path,
                     const char *form, spfy_riff_enc enc)
{
    if (!w || !path || !*path || !valid_fourcc(form)) return SPFY_E_INVAL;

    memset(w, 0, sizeof(*w));
    w->enc = enc;

    w->fp = fopen(path, "wb");
    if (!w->fp) return SPFY_E_IO;

    int rc = spfy_riff_open_chunk(w, "RIFF");
    if (rc == SPFY_OK) rc = spfy_riff_write_bytes(w, form, 4);
    if (rc != SPFY_OK) {
        fclose(w->fp);
        w->fp = NULL;
    }
    return rc;
}

int spfy_riff_write_info(spfy_riff_writer *w, int year, const char *date)
{
    if (!w || !date) return SPFY_E_INVAL;

    char copyright[128];
    snprintf(copyright, sizeof copyright,
             "Copyright %d SpeechWorks International, Inc. All Rights Reserved.",
             year);

    int rc = spfy_riff_open_chunk(w, "LIST");
    if (rc != SPFY_OK) return rc;
    rc = spfy_riff_write_bytes(w, "INFO", 4);

    if (rc == SPFY_OK) rc = spfy_riff_open_chunk(w, "ICOP");
    if (rc == SPFY_OK) rc = spfy_riff_write_str_z(w, copyright);
    if (rc == SPFY_OK) rc = spfy_riff_close_chunk(w);

    if (rc == SPFY_OK) rc = spfy_riff_open_chunk(w, "ICRD");
    if (rc == SPFY_OK) rc = spfy_riff_write_str_z(w, date);
    if (rc == SPFY_OK) rc = spfy_riff_close_chunk(w);

    if (rc == SPFY_OK) rc = spfy_riff_close_chunk(w);
    return rc;
}

int spfy_riff_finish(spfy_riff_writer *w)
{
    if (!w) return SPFY_E_INVAL;
    if (!w->fp) return SPFY_OK;

    int rc = SPFY_OK;
    while (w->depth > 0) {
        int r = spfy_riff_close_chunk(w);
        if (r != SPFY_OK) { rc = r; break; }
    }
    if (w->fp) {
        if (fclose(w->fp) != 0 && rc == SPFY_OK) rc = SPFY_E_IO;
        w->fp = NULL;
    }
    return rc;
}
