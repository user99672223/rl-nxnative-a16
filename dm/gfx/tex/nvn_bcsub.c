// nvn_bcsub.h: the ASTC pack and the CPU decoders behind it.
#include "nvn_bcsub.h"
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include "dm_sha256.h"
#endif

nvn_bcsub_stats g_bcsub_stats;

// NVN formats (nvn_fmt_mtl.c): 0x41 RGB_DXT1, 0x42 RGBA_DXT1, 0x43 DXT3, 0x44 DXT5, 0x45..0x48 the same in sRGB, 0x49 RGTC1 unsigned, 0x4a signed, 0x4b RGTC2 unsigned, 0x4c signed,
// 0x4d BPTC unorm (BC7), 0x4e BPTC sRGB, 0x4f BPTC signed float (BC6H), 0x50 unsigned float
int nvn_bcsub_kind(int f, int *srgb, int *sign) {
    int k = 0, s = 0, g = 0;
    if (f >= 0x41 && f <= 0x48) { static const int kk[4] = {1, 1, 2, 3}; k = kk[(f - 0x41) & 3]; s = f >= 0x45; }
    else if (f == 0x49 || f == 0x4a) { k = 4; g = f == 0x4a; }
    else if (f == 0x4b || f == 0x4c) { k = 5; g = f == 0x4c; }
    else if (f == 0x4d || f == 0x4e) { k = 7; s = f == 0x4e; }
    else if (f == 0x4f || f == 0x50) { k = 6; g = f == 0x4f; }
    if (srgb) *srgb = s;
    if (sign) *sign = g;
    return k;
}

typedef struct { uint8_t key[16]; uint64_t off; uint32_t len; uint8_t bx, by; uint16_t pad; } pack_ent;
static const uint8_t *g_pack; static size_t g_pack_len; static const pack_ent *g_ent; static uint32_t g_n;

int nvn_bcsub_open(const char *path) {
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    struct stat st; if (fstat(fd, &st) || st.st_size < 16) { close(fd); return -1; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (m == MAP_FAILED) return -1;
    const uint8_t *p = m; uint32_t n; memcpy(&n, p + 8, 4);
    if (memcmp(p, "DMBCA1\0\0", 8) || 16 + (uint64_t)n * sizeof(pack_ent) > (uint64_t)st.st_size) { munmap(m, (size_t)st.st_size); return -1; }
    g_pack = p; g_pack_len = (size_t)st.st_size; g_ent = (const pack_ent *)(p + 16); g_n = n;
    return 0;
}
int nvn_bcsub_count(void) { return (int)g_n; }

void nvn_bcsub_key(int kind, uint32_t w, uint32_t h, const void *blocks, size_t n, uint8_t key[16]) {
    uint32_t hdr[3] = {(uint32_t)kind, w, h}; uint8_t dg[32];
#ifdef __APPLE__
    CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, hdr, 12);
    for (size_t o = 0; o < n;) { size_t k = n - o > (1u << 30) ? (1u << 30) : n - o; CC_SHA256_Update(&c, (const uint8_t *)blocks + o, (CC_LONG)k); o += k; }
    CC_SHA256_Final(dg, &c);
#else
    sha256_ctx c; sha256_init(&c); sha256_update(&c, (const uint8_t *)hdr, 12); sha256_update(&c, blocks, n); sha256_final(&c, dg);
#endif
    memcpy(key, dg, 16);
}

// The first entry with this key (entries are sorted by key, then block size); g_n when there is none
static uint32_t first_of(const uint8_t key[16]) {
    uint32_t lo = 0, hi = g_n;
    while (lo < hi) { uint32_t mid = lo + (hi - lo) / 2; if (memcmp(g_ent[mid].key, key, 16) < 0) lo = mid + 1; else hi = mid; }
    return lo < g_n && !memcmp(g_ent[lo].key, key, 16) ? lo : g_n;
}
unsigned nvn_bcsub_sizes(const uint8_t key[16]) {
    unsigned m = 0;
    for (uint32_t i = first_of(key); i < g_n && !memcmp(g_ent[i].key, key, 16); i++) if (g_ent[i].bx == g_ent[i].by && g_ent[i].bx < 32) m |= 1u << g_ent[i].bx;
    return m;
}
const uint8_t *nvn_bcsub_find(const uint8_t key[16], int b, uint32_t *len) {
    for (uint32_t i = first_of(key); i < g_n && !memcmp(g_ent[i].key, key, 16); i++)
        if (g_ent[i].bx == b && g_ent[i].by == b && g_ent[i].off + g_ent[i].len <= g_pack_len) { if (len) *len = g_ent[i].len; return g_pack + g_ent[i].off; }
    return NULL;
}

// ---- CPU decoders (the same arithmetic as bc2astc.cpp's, which the pack's pictures were made from) ----
static void color565(uint16_t c, uint8_t *o) { int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31; o[0] = (uint8_t)(r << 3 | r >> 2); o[1] = (uint8_t)(g << 2 | g >> 4); o[2] = (uint8_t)(b << 3 | b >> 2); }
static void bc1_block(const uint8_t *s, uint8_t px[16][4], int three_ok) {
    uint16_t c0 = (uint16_t)(s[0] | s[1] << 8), c1 = (uint16_t)(s[2] | s[3] << 8); uint8_t p[4][4];
    color565(c0, p[0]); color565(c1, p[1]); p[0][3] = p[1][3] = p[2][3] = p[3][3] = 255;
    if (c0 > c1 || !three_ok) for (int i = 0; i < 3; i++) { p[2][i] = (uint8_t)((2 * p[0][i] + p[1][i] + 1) / 3); p[3][i] = (uint8_t)((p[0][i] + 2 * p[1][i] + 1) / 3); }
    else { for (int i = 0; i < 3; i++) { p[2][i] = (uint8_t)((p[0][i] + p[1][i] + 1) / 2); p[3][i] = 0; } p[3][3] = 0; }
    uint32_t bits = (uint32_t)s[4] | (uint32_t)s[5] << 8 | (uint32_t)s[6] << 16 | (uint32_t)s[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(px[i], p[(bits >> (2 * i)) & 3], 4);
}
// One BC4 block (also BC3's alpha and each half of BC5): 16 values
static void bc4_block(const uint8_t *s, int sign, uint8_t v[16]) {
    int a[8];
    if (sign) { a[0] = (int8_t)s[0]; a[1] = (int8_t)s[1]; if (a[0] < -127) a[0] = -127; if (a[1] < -127) a[1] = -127; } else { a[0] = s[0]; a[1] = s[1]; }
    if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1] + (sign ? 0 : 3)) / 7;
    else { for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1] + (sign ? 0 : 2)) / 5; a[6] = sign ? -127 : 0; a[7] = sign ? 127 : 255; }
    uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= (uint64_t)s[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) v[i] = (uint8_t)a[(bits >> (3 * i)) & 7];
}
int nvn_bcsub_decode(int kind, int sign, uint32_t w, uint32_t h, const void *blocks, uint8_t *out) {
    if (kind < 1 || kind > 5) return 0;
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bpb = kind == 1 || kind == 4 ? 8 : 16; int bpp = kind == 4 ? 1 : kind == 5 ? 2 : 4;
    for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
        const uint8_t *s = (const uint8_t *)blocks + ((size_t)by * bw + bx) * bpb; uint8_t px[16][4];
        if (kind <= 3) {
            bc1_block(s + (bpb - 8), px, kind == 1);
            if (kind == 3) { uint8_t a[16]; bc4_block(s, 0, a); for (int i = 0; i < 16; i++) px[i][3] = a[i]; }
            else if (kind == 2) for (int i = 0; i < 16; i++) { int n = (s[i / 2] >> (4 * (i & 1))) & 15; px[i][3] = (uint8_t)(n * 17); }
        } else {
            uint8_t r[16], g[16]; bc4_block(s, sign, r); if (kind == 5) bc4_block(s + 8, sign, g);
            for (int i = 0; i < 16; i++) { px[i][0] = r[i]; px[i][1] = kind == 5 ? g[i] : 0; }
        }
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            uint32_t X = bx * 4 + (uint32_t)x, Y = by * 4 + (uint32_t)y; if (X >= w || Y >= h) continue;
            memcpy(out + ((size_t)Y * w + X) * (size_t)bpp, px[y * 4 + x], (size_t)bpp);
        }
    }
    return bpp;
}

// A level the pack does not hold, for a texture that is ASTC: one flat colour per block (ASTC's void-extent block: the marker, no extent, then RGBA as 16-bit values), the mean
// of the b x b texels the block covers. Blurred to the block, but a valid picture of the right colours; rgba is the level decoded (nvn_bcsub_decode)
void nvn_bcsub_void_blocks(const uint8_t *rgba, uint32_t w, uint32_t h, int b, uint8_t *out) {
    uint32_t bw = (w + (uint32_t)b - 1) / (uint32_t)b, bh = (h + (uint32_t)b - 1) / (uint32_t)b;
    for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
        uint32_t s[4] = {0}, n = 0;
        for (uint32_t y = by * (uint32_t)b; y < (by + 1) * (uint32_t)b && y < h; y++) for (uint32_t x = bx * (uint32_t)b; x < (bx + 1) * (uint32_t)b && x < w; x++) {
            const uint8_t *p = rgba + ((size_t)y * w + x) * 4; s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; s[3] += p[3]; n++; }
        uint8_t *o = out + ((size_t)by * bw + bx) * 16;
        static const uint8_t head[8] = {0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        memcpy(o, head, 8);
        for (int c = 0; c < 4; c++) { uint32_t v = n ? (s[c] + n / 2) / n : 0; uint16_t v16 = (uint16_t)(v * 257); o[8 + 2 * c] = (uint8_t)v16; o[9 + 2 * c] = (uint8_t)(v16 >> 8); }
    }
}
