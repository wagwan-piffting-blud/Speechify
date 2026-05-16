/* spfy_time_stretch — A/B tester for the WSOLA time-stretch DSP.
 *
 *   spfy_time_stretch <in.wav> <factor> <out.wav>
 *
 * factor > 1.0 speeds up (shorter output), < 1.0 slows down.
 * Pure post-process on rendered PCM. */

#include "time_stretch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int read_wav(const char *path, int16_t **out, size_t *n,
                    uint32_t *sr)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    uint8_t hdr[44];
    if (fread(hdr, 1, 44, f) != 44) { fclose(f); return -1; }
    *sr = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8)
        | ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
    long data_off = -1; uint32_t data_sz = 0;
    fseek(f, 12, SEEK_SET);
    while (!feof(f)) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) break;
        uint32_t sz = (uint32_t)c[4] | ((uint32_t)c[5] << 8)
                    | ((uint32_t)c[6] << 16) | ((uint32_t)c[7] << 24);
        if (memcmp(c, "data", 4) == 0) {
            data_off = ftell(f); data_sz = sz; break;
        }
        fseek(f, (long)sz, SEEK_CUR);
    }
    if (data_off < 0) { fclose(f); return -1; }
    fseek(f, data_off, SEEK_SET);
    *n = data_sz / 2u;
    *out = (int16_t *)malloc(*n * sizeof(int16_t));
    if (!*out) { fclose(f); return -1; }
    size_t got = fread(*out, sizeof(int16_t), *n, f);
    fclose(f);
    return got == *n ? 0 : -1;
}

static int write_wav(const char *path, const int16_t *samples, size_t n,
                     uint32_t sr)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    uint32_t data_sz = (uint32_t)(n * 2u);
    uint32_t riff_sz = 36u + data_sz;
    uint8_t  hdr[44] = {0};
    memcpy(hdr, "RIFF", 4);
    hdr[4]=(uint8_t)(riff_sz&0xFF); hdr[5]=(uint8_t)((riff_sz>>8)&0xFF);
    hdr[6]=(uint8_t)((riff_sz>>16)&0xFF); hdr[7]=(uint8_t)((riff_sz>>24)&0xFF);
    memcpy(hdr+8, "WAVEfmt ", 8);
    hdr[16]=16; hdr[20]=1; hdr[22]=1;
    hdr[24]=(uint8_t)(sr&0xFF); hdr[25]=(uint8_t)((sr>>8)&0xFF);
    hdr[26]=(uint8_t)((sr>>16)&0xFF); hdr[27]=(uint8_t)((sr>>24)&0xFF);
    uint32_t br=sr*2u;
    hdr[28]=(uint8_t)(br&0xFF); hdr[29]=(uint8_t)((br>>8)&0xFF);
    hdr[30]=(uint8_t)((br>>16)&0xFF); hdr[31]=(uint8_t)((br>>24)&0xFF);
    hdr[32]=2; hdr[34]=16;
    memcpy(hdr+36, "data", 4);
    hdr[40]=(uint8_t)(data_sz&0xFF); hdr[41]=(uint8_t)((data_sz>>8)&0xFF);
    hdr[42]=(uint8_t)((data_sz>>16)&0xFF); hdr[43]=(uint8_t)((data_sz>>24)&0xFF);
    int ok = fwrite(hdr,1,44,f)==44 && fwrite(samples,2,n,f)==n;
    fclose(f);
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <in.wav> <factor> <out.wav>\n"
                        "  factor > 1.0 = speed up, < 1.0 = slow down\n", argv[0]);
        return 1;
    }
    int16_t *in_pcm = NULL; size_t n_in = 0; uint32_t sr = 0;
    if (read_wav(argv[1], &in_pcm, &n_in, &sr) != 0) return 1;
    float factor = (float)atof(argv[2]);
    int16_t *out_pcm = NULL; size_t n_out = 0;
    int rc = spfy_time_stretch_block(in_pcm, n_in, &out_pcm, &n_out,
                                      factor, (int)sr);
    free(in_pcm);
    if (rc != 0) { fprintf(stderr, "stretch rc=%d\n", rc); return 1; }
    if (write_wav(argv[3], out_pcm, n_out, sr) != 0) {
        free(out_pcm); return 1;
    }
    fprintf(stdout, "wrote %s (%zu -> %zu samples @ %u Hz, factor %.3fx)\n",
            argv[3], n_in, n_out, (unsigned)sr, (double)factor);
    free(out_pcm);
    return 0;
}
