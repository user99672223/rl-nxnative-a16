// SHA-256 for bc2astc (the host uses CommonCrypto for the same digest). Written for this tool; FIPS 180-4.
#pragma once
#include <stdint.h>
#include <string.h>
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha256_ctx;
static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static inline uint32_t sha256_rr(uint32_t x, int n) { return x >> n | x << (32 - n); }
static void sha256_block(sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) { uint32_t s0 = sha256_rr(w[i - 15], 7) ^ sha256_rr(w[i - 15], 18) ^ w[i - 15] >> 3, s1 = sha256_rr(w[i - 2], 17) ^ sha256_rr(w[i - 2], 19) ^ w[i - 2] >> 10; w[i] = w[i - 16] + s0 + w[i - 7] + s1; }
    memcpy(a, c->h, sizeof a);
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = sha256_rr(a[4], 6) ^ sha256_rr(a[4], 11) ^ sha256_rr(a[4], 25), ch = (a[4] & a[5]) ^ (~a[4] & a[6]), t1 = a[7] + s1 + ch + sha256_k[i] + w[i];
        uint32_t s0 = sha256_rr(a[0], 2) ^ sha256_rr(a[0], 13) ^ sha256_rr(a[0], 22), mj = (a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]), t2 = s0 + mj;
        a[7] = a[6]; a[6] = a[5]; a[5] = a[4]; a[4] = a[3] + t1; a[3] = a[2]; a[2] = a[1]; a[1] = a[0]; a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) c->h[i] += a[i];
}
static void sha256_init(sha256_ctx *c) {
    static const uint32_t h0[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->h, h0, sizeof h0); c->len = 0; c->n = 0;
}
static void sha256_update(sha256_ctx *c, const uint8_t *p, size_t n) {
    c->len += n;
    if (c->n) { size_t k = 64 - c->n; if (k > n) k = n; memcpy(c->buf + c->n, p, k); c->n += k; p += k; n -= k; if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; } }
    for (; n >= 64; p += 64, n -= 64) sha256_block(c, p);
    if (n) { memcpy(c->buf, p, n); c->n = n; }
}
static void sha256_final(sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->len * 8; uint8_t pad[72] = {0x80}; size_t k = (c->n < 56 ? 56 : 120) - c->n;
    uint8_t lenb[8]; for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, pad, k); sha256_update(c, lenb, 8);
    for (int i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16); out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i]; }
}
