/* Byte-exact round trip through the reconstructed RIFF writer.
 *
 * Reads a container, decrypts it, walks its top-level chunks, re-emits them
 * through src/common/riff_write, and diffs the result against the original.
 *
 * This is the acceptance test for the writer contract recovered from
 * SWIttsEngineUtil.dll (reveng/DLL_ANALYSIS.md section 9). Any divergence
 * means the contract is wrong, and it is far cheaper to learn that here than
 * after a builder is written on top of it.
 *
 * Nested chunks are re-emitted as opaque payloads, which is sufficient for
 * byte-exactness: LIST bodies already contain their form type and children.
 * Pad bytes are NOT part of a chunk payload, so the writer regenerates them —
 * if the vendor padded with anything other than a ciphered zero, the diff
 * reports it.
 *
 *   spfy_riff_roundtrip <in.vin> [out.vin]
 */

#include "../common/file_io.h"
#include "../common/obfuscation.h"
#include "../common/riff.h"
#include "../common/riff_write.h"
#include "../../include/spfy/spfy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int looks_encrypted(const uint8_t *d, size_t n)
{
    if (n < 4) return 0;
    if (!memcmp(d, "RIFF", 4)) return 0;
    uint8_t h[4];
    spfy_unobfuscate_ce_copy(h, d, 4);
    return !memcmp(h, "RIFF", 4);
}

static void report_first_diff(const uint8_t *a, size_t na,
                              const uint8_t *b, size_t nb)
{
    size_t lim = na < nb ? na : nb;
    for (size_t i = 0; i < lim; ++i) {
        if (a[i] != b[i]) {
            fprintf(stderr, "  first difference at 0x%zx: orig 0x%02x, ours 0x%02x\n",
                    i, a[i], b[i]);
            return;
        }
    }
    fprintf(stderr, "  common prefix identical; lengths differ only\n");
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <in.vin> [out.vin]\n", argv[0]);
        return 2;
    }
    const char *in_path  = argv[1];
    const char *out_path = (argc == 3) ? argv[2] : "roundtrip.out";

    uint8_t *raw = NULL;
    size_t   n   = 0;
    int rc = spfy_slurp_file(in_path, &raw, &n);
    if (rc != SPFY_OK) {
        fprintf(stderr, "cannot read %s (%d)\n", in_path, rc);
        return 1;
    }

    spfy_riff_enc enc = SPFY_RIFF_PLAIN;
    uint8_t *plain = raw;
    if (looks_encrypted(raw, n)) {
        enc = SPFY_RIFF_CE;
        plain = (uint8_t *)malloc(n);
        if (!plain) { free(raw); fprintf(stderr, "out of memory\n"); return 1; }
        spfy_unobfuscate_ce_copy(plain, raw, n);
    }

    if (n < 12 || memcmp(plain, "RIFF", 4)) {
        fprintf(stderr, "%s is not a RIFF container\n", in_path);
        if (plain != raw) free(plain);
        free(raw);
        return 1;
    }

    char form[5];
    memcpy(form, plain + 8, 4);
    form[4] = '\0';
    printf("in   : %s (%zu bytes, form '%s', %s)\n", in_path, n, form,
           enc == SPFY_RIFF_CE ? "XOR 0xCE" : "plain");

    spfy_riff_writer w;
    rc = spfy_riff_create(&w, out_path, form, enc);
    if (rc != SPFY_OK) {
        fprintf(stderr, "cannot create %s (%d)\n", out_path, rc);
        if (plain != raw) free(plain);
        free(raw);
        return 1;
    }

    uint32_t riff_size = (uint32_t)plain[4] | ((uint32_t)plain[5] << 8) |
                         ((uint32_t)plain[6] << 16) | ((uint32_t)plain[7] << 24);
    size_t body_end = (size_t)riff_size + 8;
    if (body_end > n) body_end = n;

    spfy_riff_iter it;
    spfy_riff_iter_init(&it, plain + 12, body_end - 12);

    spfy_chunk c;
    int count = 0;
    while ((rc = spfy_riff_iter_next(&it, &c)) == 1) {
        char id[5];
        spfy_fourcc_str(c.fourcc, id);
        int r = spfy_riff_open_chunk(&w, id);
        if (r == SPFY_OK) r = spfy_riff_write_bytes(&w, c.data, c.size);
        if (r == SPFY_OK) r = spfy_riff_close_chunk(&w);
        if (r != SPFY_OK) {
            fprintf(stderr, "write failed on chunk '%s' (%d)\n", id, r);
            spfy_riff_finish(&w);
            if (plain != raw) free(plain);
            free(raw);
            return 1;
        }
        printf("  %-4s %12u\n", id, c.size);
        ++count;
    }
    if (rc < 0) fprintf(stderr, "warning: chunk walk stopped early (truncated?)\n");

    rc = spfy_riff_finish(&w);
    if (rc != SPFY_OK) {
        fprintf(stderr, "finish failed (%d)\n", rc);
        if (plain != raw) free(plain);
        free(raw);
        return 1;
    }

    uint8_t *back = NULL;
    size_t   bn   = 0;
    rc = spfy_slurp_file(out_path, &back, &bn);
    if (rc != SPFY_OK) {
        fprintf(stderr, "cannot read back %s (%d)\n", out_path, rc);
        if (plain != raw) free(plain);
        free(raw);
        return 1;
    }

    printf("out  : %s (%zu bytes, %d chunks)\n", out_path, bn, count);

    int ok = (bn == n) && !memcmp(raw, back, n);
    if (ok) {
        printf("RESULT: BYTE-IDENTICAL\n");
    } else {
        printf("RESULT: DIFFERS (orig %zu, ours %zu)\n", n, bn);
        report_first_diff(raw, n, back, bn);
    }

    free(back);
    if (plain != raw) free(plain);
    free(raw);
    return ok ? 0 : 1;
}
