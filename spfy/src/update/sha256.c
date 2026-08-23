#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t ror(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void sha256_block(spfy_sha256_t *c, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4]     << 24) |
               ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] <<  8) |
               ((uint32_t)p[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
    e = c->state[4]; f = c->state[5]; g  = c->state[6]; h = c->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g;  c->state[7] += h;
}

void spfy_sha256_init(spfy_sha256_t *c)
{
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->bits  = 0;
    c->buf_n = 0;
}

void spfy_sha256_update(spfy_sha256_t *c, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;

    c->bits += (uint64_t)n * 8u;
    if (c->buf_n) {
        size_t want = 64u - c->buf_n;
        size_t take = n < want ? n : want;
        memcpy(c->buf + c->buf_n, p, take);
        c->buf_n += take;
        p += take;
        n -= take;
        if (c->buf_n == 64u) {
            sha256_block(c, c->buf);
            c->buf_n = 0;
        }
    }
    while (n >= 64u) {
        sha256_block(c, p);
        p += 64;
        n -= 64;
    }
    if (n) {
        memcpy(c->buf, p, n);
        c->buf_n = n;
    }
}

void spfy_sha256_final(spfy_sha256_t *c, uint8_t out[32])
{
    uint64_t bits = c->bits;
    uint8_t  pad  = 0x80;
    uint8_t  len[8];
    int i;

    spfy_sha256_update(c, &pad, 1);
    c->bits = bits;                     /* padding is not message length */
    while (c->buf_n != 56u) {
        uint8_t z = 0;
        spfy_sha256_update(c, &z, 1);
        c->bits = bits;
    }
    for (i = 0; i < 8; i++)
        len[i] = (uint8_t)((bits >> (56 - i * 8)) & 0xffu);
    spfy_sha256_update(c, len, 8);

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)((c->state[i] >> 24) & 0xffu);
        out[i * 4 + 1] = (uint8_t)((c->state[i] >> 16) & 0xffu);
        out[i * 4 + 2] = (uint8_t)((c->state[i] >>  8) & 0xffu);
        out[i * 4 + 3] = (uint8_t)( c->state[i]        & 0xffu);
    }
}

void spfy_sha256_hex(const uint8_t digest[32], char out[65])
{
    static const char hexd[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        out[i * 2]     = hexd[(digest[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hexd[digest[i] & 0x0f];
    }
    out[64] = '\0';
}

int spfy_sha256_file(const char *path, char out_hex[65])
{
    enum { CHUNK = 1024 * 1024 };
    spfy_sha256_t c;
    uint8_t digest[32];
    uint8_t *buf;
    FILE *fp;
    size_t got;

    fp = fopen(path, "rb");
    if (!fp) return -1;
    buf = (uint8_t *)malloc(CHUNK);
    if (!buf) { fclose(fp); return -1; }

    spfy_sha256_init(&c);
    while ((got = fread(buf, 1, CHUNK, fp)) > 0)
        spfy_sha256_update(&c, buf, got);

    if (ferror(fp)) {
        free(buf);
        fclose(fp);
        return -1;
    }
    free(buf);
    fclose(fp);
    spfy_sha256_final(&c, digest);
    spfy_sha256_hex(digest, out_hex);
    return 0;
}
