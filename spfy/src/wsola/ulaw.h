#ifndef SPFY_WSOLA_ULAW_H
#define SPFY_WSOLA_ULAW_H

#include <stddef.h>
#include <stdint.h>

/* ITU G.711 u-law (mu-law) decoder.
 *
 * One byte -> one s16 PCM sample. The Speechify VDB stores audio as
 * 8 kHz mono u-law (1 byte per sample) despite the WAVE fmt header
 * advertising 16-bit (a Speechify quirk; see reveng/README_TECHNICAL.md
 * "fmt" + "data" sections).
 *
 * The 256-entry decode LUT is computed from the standard G.711 formula
 * lazily on first call. Plan gap #7 calls for verifying this LUT against
 * the engine's own; for now we use the textbook G.711 which is the de
 * facto standard. */

int16_t spfy_ulaw_decode_byte(uint8_t b);

/* Bulk decode: src[n] -> dst[n] s16 samples. */
void spfy_ulaw_decode(const uint8_t *src, size_t n, int16_t *dst);

#endif
