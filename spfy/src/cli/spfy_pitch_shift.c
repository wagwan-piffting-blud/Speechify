/* spfy_pitch_shift — standalone A/B tester for the TD-PSOLA pitch
 * shifter. Reads a minimal RIFF/WAVE int16 mono file, applies a
 * `semitones` shift, writes the result back as the same format.
 *
 *   spfy_pitch_shift <in.wav> <semitones> <out.wav>
 *
 * Used to listen-test pitch shift quality before wiring the DSP into
 * the SAPI shim. Does NOT touch the synth pipeline — operates purely
 * on rendered PCM. */

#include "pitch_shift.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Minimal RIFF/WAVE reader for the format we ship (int16 mono PCM).
 * Returns 0 on success. *out is malloc'd PCM samples; caller free()s. */
static int read_wav(const char *path, int16_t **out, size_t *n_samples,
                    uint32_t *sample_rate)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    uint8_t hdr[44];
    if (fread(hdr, 1, 44, f) != 44) { fclose(f); return -1; }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "%s: not a RIFF/WAVE file\n", path);
        fclose(f); return -1;
    }
    *sample_rate = (uint32_t)hdr[24]
                 | ((uint32_t)hdr[25] << 8)
                 | ((uint32_t)hdr[26] << 16)
                 | ((uint32_t)hdr[27] << 24);
    /* Find 'data' chunk — naive linear scan from offset 36. */
    long data_off = -1;
    uint32_t data_sz = 0;
    fseek(f, 12, SEEK_SET);
    while (!feof(f)) {
        uint8_t chunk[8];
        if (fread(chunk, 1, 8, f) != 8) break;
        uint32_t sz = (uint32_t)chunk[4]
                    | ((uint32_t)chunk[5] << 8)
                    | ((uint32_t)chunk[6] << 16)
                    | ((uint32_t)chunk[7] << 24);
        if (memcmp(chunk, "data", 4) == 0) {
            data_off = ftell(f);
            data_sz = sz;
            break;
        }
        fseek(f, (long)sz, SEEK_CUR);
    }
    if (data_off < 0) { fclose(f); return -1; }
    fseek(f, data_off, SEEK_SET);
    *n_samples = data_sz / 2u;
    *out = (int16_t *)malloc(*n_samples * sizeof(int16_t));
    if (!*out) { fclose(f); return -1; }
    size_t got = fread(*out, sizeof(int16_t), *n_samples, f);
    fclose(f);
    if (got != *n_samples) { free(*out); return -1; }
    return 0;
}

/* Tiny RIFF/WAVE int16 mono writer mirroring spfy_wav.c's header format. */
static int write_wav(const char *path, const int16_t *samples,
                     size_t n_samples, uint32_t sample_rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    uint32_t data_sz = (uint32_t)(n_samples * 2u);
    uint32_t riff_sz = 36u + data_sz;
    uint8_t  hdr[44];
    memcpy(hdr, "RIFF", 4);
    hdr[4]  = (uint8_t)(riff_sz & 0xFF);
    hdr[5]  = (uint8_t)((riff_sz >> 8) & 0xFF);
    hdr[6]  = (uint8_t)((riff_sz >> 16) & 0xFF);
    hdr[7]  = (uint8_t)((riff_sz >> 24) & 0xFF);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    hdr[16] = 16; hdr[17] = hdr[18] = hdr[19] = 0;
    hdr[20] = 1; hdr[21] = 0;       /* PCM */
    hdr[22] = 1; hdr[23] = 0;       /* mono */
    hdr[24] = (uint8_t)(sample_rate & 0xFF);
    hdr[25] = (uint8_t)((sample_rate >> 8) & 0xFF);
    hdr[26] = (uint8_t)((sample_rate >> 16) & 0xFF);
    hdr[27] = (uint8_t)((sample_rate >> 24) & 0xFF);
    uint32_t br = sample_rate * 2u;
    hdr[28] = (uint8_t)(br & 0xFF);
    hdr[29] = (uint8_t)((br >> 8) & 0xFF);
    hdr[30] = (uint8_t)((br >> 16) & 0xFF);
    hdr[31] = (uint8_t)((br >> 24) & 0xFF);
    hdr[32] = 2; hdr[33] = 0;
    hdr[34] = 16; hdr[35] = 0;
    memcpy(hdr + 36, "data", 4);
    hdr[40] = (uint8_t)(data_sz & 0xFF);
    hdr[41] = (uint8_t)((data_sz >> 8) & 0xFF);
    hdr[42] = (uint8_t)((data_sz >> 16) & 0xFF);
    hdr[43] = (uint8_t)((data_sz >> 24) & 0xFF);
    if (fwrite(hdr, 1, 44, f) != 44
        || fwrite(samples, sizeof(int16_t), n_samples, f) != n_samples) {
        fclose(f); return -1;
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr,
            "usage: spfy_pitch_shift <in.wav> <semitones> <out.wav>\n"
            "  semitones: positive = up, negative = down (range +-12)\n");
        return 1;
    }
    const char *in_path  = argv[1];
    float       semis    = (float)atof(argv[2]);
    const char *out_path = argv[3];

    int16_t *in_pcm = NULL;
    size_t   n      = 0;
    uint32_t sr     = 0;
    if (read_wav(in_path, &in_pcm, &n, &sr) != 0) {
        fprintf(stderr, "read_wav failed\n");
        return 1;
    }
    int16_t *out_pcm = (int16_t *)malloc(n * sizeof(int16_t));
    if (!out_pcm) { free(in_pcm); return 1; }

    int rc = spfy_pitch_shift_block(in_pcm, n, out_pcm, semis, (int)sr);
    if (rc != 0) {
        fprintf(stderr, "pitch_shift_block rc=%d\n", rc);
        free(in_pcm); free(out_pcm); return 1;
    }
    if (write_wav(out_path, out_pcm, n, sr) != 0) {
        fprintf(stderr, "write_wav failed\n");
        free(in_pcm); free(out_pcm); return 1;
    }
    fprintf(stdout, "wrote %s (%zu samples @ %u Hz, shift %+.2f st)\n",
            out_path, n, (unsigned)sr, (double)semis);
    free(in_pcm); free(out_pcm);
    return 0;
}
