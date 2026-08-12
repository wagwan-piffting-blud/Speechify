#ifndef SPFY_WSOLA_ULAW_H
#define SPFY_WSOLA_ULAW_H

#include <stddef.h>
#include <stdint.h>

/* ITU G.711 u-law (mu-law) decoder. */

int16_t spfy_ulaw_decode_byte(uint8_t b);

void spfy_ulaw_decode(const uint8_t *src, size_t n, int16_t *dst);

#endif
