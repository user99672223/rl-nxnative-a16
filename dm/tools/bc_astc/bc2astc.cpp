// bc2astc: re-encodes BC1 / BC2 / BC3 textures as ASTC for GPUs without BC textures (an A16 iPhone), offline, into the pack the host looks levels up in
// (dm/gfx/tex/nvn_bcsub.c). Reads textures from stdin, written by dm/tools/ue3_astc.py:
//   u32 magic 'BCJ2', then per texture: u8 kind (1 BC1, 2 BC2, 3 BC3), u8 mode (0 colour, 1 normal map with x and y in alpha and green, 2 normal map in RGB), u16 level count,
//   then per level, largest first: u32 width, u32 height, u32 length, length bytes of linear blocks (rows of blocks, as the host has them after deswizzling)
// and writes <out>: 'DMBCA1\0\0', u32 count, u32 0, count entries {u8 key[16], u64 offset, u32 length, u8 bx, u8 by, u16 0} sorted by key then block size, then the ASTC blocks.
// key = the first 16 bytes of SHA-256(u32 kind, u32 width, u32 height, the level's linear blocks): what the host computes from the level it is about to upload.
// Block size, per texture: the largest of 6x6, 5x5, 4x4 whose first level keeps PSNR_MIN dB against the BC picture over the channels the mode reads (BC1 colour at 6x6 is 3.56 bits
// per texel against BC1's 4; 4x4 is BC3's 8). All of a texture's levels take that size, since a Metal texture has one format; a level two textures share is stored once per size.
// Build: c++ -O2 -std=c++17 bc2astc.cpp -lastcenc -lpthread (Arm's astcenc library, Apache 2.0, not part of this repository)
#include <astcenc.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "sha256.h"

struct Level { uint32_t w, h; std::vector<uint8_t> data; };
struct Job { uint8_t kind, mode; std::vector<Level> lv; };
struct Entry { uint8_t key[16]; uint64_t off; uint32_t len; uint8_t bx, by; uint16_t pad; };
static_assert(sizeof(Entry) == 32, "entry layout");

static void color565(uint16_t c, uint8_t *o) { int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31; o[0] = (uint8_t)(r << 3 | r >> 2); o[1] = (uint8_t)(g << 2 | g >> 4); o[2] = (uint8_t)(b << 3 | b >> 2); }
// One BC1 colour block (8 bytes) into 16 RGBA texels; three_ok: the three-colour mode with a transparent index exists (BC1), not in BC2 / BC3's colour part
static void bc1_block(const uint8_t *s, uint8_t px[16][4], bool three_ok) {
    uint16_t c0 = (uint16_t)(s[0] | s[1] << 8), c1 = (uint16_t)(s[2] | s[3] << 8); uint8_t p[4][4];
    color565(c0, p[0]); color565(c1, p[1]); p[0][3] = p[1][3] = p[2][3] = p[3][3] = 255;
    if (c0 > c1 || !three_ok) for (int i = 0; i < 3; i++) { p[2][i] = (uint8_t)((2 * p[0][i] + p[1][i] + 1) / 3); p[3][i] = (uint8_t)((p[0][i] + 2 * p[1][i] + 1) / 3); }
    else { for (int i = 0; i < 3; i++) { p[2][i] = (uint8_t)((p[0][i] + p[1][i] + 1) / 2); p[3][i] = 0; } p[3][3] = 0; }
    uint32_t bits = (uint32_t)s[4] | (uint32_t)s[5] << 8 | (uint32_t)s[6] << 16 | (uint32_t)s[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(px[i], p[(bits >> (2 * i)) & 3], 4);
}
static void bc3_alpha(const uint8_t *s, uint8_t px[16][4]) {
    int a0 = s[0], a1 = s[1], a[8]; a[0] = a0; a[1] = a1;
    if (a0 > a1) for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
    else { for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5; a[6] = 0; a[7] = 255; }
    uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= (uint64_t)s[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) px[i][3] = (uint8_t)a[(bits >> (3 * i)) & 7];
}
struct LvRef { uint8_t kind; uint32_t w, h; const std::vector<uint8_t> &data; };
static void decode(const LvRef &j, std::vector<uint8_t> &rgba) {
    uint32_t bw = (j.w + 3) / 4, bh = (j.h + 3) / 4, bpb = j.kind == 1 ? 8 : 16; rgba.assign((size_t)j.w * j.h * 4, 0);
    for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
        const uint8_t *s = &j.data[((size_t)by * bw + bx) * bpb]; uint8_t px[16][4];
        bc1_block(s + (bpb - 8), px, j.kind == 1);
        if (j.kind == 3) bc3_alpha(s, px);
        else if (j.kind == 2) for (int i = 0; i < 16; i++) { int n = (s[i / 2] >> (4 * (i & 1))) & 15; px[i][3] = (uint8_t)(n * 17); }
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            uint32_t X = bx * 4 + x, Y = by * 4 + y; if (X >= j.w || Y >= j.h) continue;
            memcpy(&rgba[((size_t)Y * j.w + X) * 4], px[y * 4 + x], 4);
        }
    }
}

static std::mutex g_mu; static std::condition_variable g_cv_put, g_cv_get; static std::vector<Job> g_q; static bool g_eof;
static std::mutex g_out_mu; static FILE *g_data; static uint64_t g_data_len; static std::vector<Entry> g_entries; static std::atomic<int> g_fail;

static double g_psnr_min[3] = {38.0, 40.0, 38.0};   // per mode
static std::atomic<uint64_t> g_pick[3][7];             // [mode][block size]: textures that took it

struct Enc {
    std::map<uint32_t, astcenc_context *> ctxs;
    astcenc_context *get(int b, int mode) {
        astcenc_context *&ctx = ctxs[(uint32_t)b | (uint32_t)mode << 8];
        if (!ctx) {
            astcenc_config cfg;
            if (astcenc_config_init(ASTCENC_PRF_LDR, b, b, 1, ASTCENC_PRE_MEDIUM, 0, &cfg) != ASTCENC_SUCCESS) return nullptr;
            // Mode 1 (Rocket League's TC_NormalmapSwizzle DXT5): x and y are in alpha and green; red is constant (255, or 33) and blue nearly so (mean 251, deviation up to 12 in 25
            // sampled textures), so the error is weighed towards green and alpha, with blue kept at half weight in case a shader reads it
            if (mode == 1) { cfg.cw_r_weight = 0.2f; cfg.cw_g_weight = 1; cfg.cw_b_weight = 0.5f; cfg.cw_a_weight = 1; }
            if (astcenc_context_alloc(&cfg, 1, &ctx) != ASTCENC_SUCCESS) { ctx = nullptr; return nullptr; }
        }
        return ctx;
    }
    ~Enc() { for (auto &c : ctxs) if (c.second) astcenc_context_free(c.second); }
};
static bool encode(Enc &E, int b, int mode, uint32_t w, uint32_t h, std::vector<uint8_t> &rgba, std::vector<uint8_t> &out) {
    astcenc_context *ctx = E.get(b, mode); if (!ctx) return false;
    void *slice = rgba.data(); astcenc_image img; img.dim_x = w; img.dim_y = h; img.dim_z = 1; img.data_type = ASTCENC_TYPE_U8; img.data = &slice;
    astcenc_swizzle swz{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    size_t n = (size_t)((w + b - 1) / b) * ((h + b - 1) / b) * 16; out.resize(n);
    bool ok = astcenc_compress_image(ctx, &img, &swz, out.data(), n, 0) == ASTCENC_SUCCESS; astcenc_compress_reset(ctx); return ok;
}
// PSNR of the ASTC blocks against the picture they were made from, over the channels the mode reads
static double psnr(Enc &E, int b, int mode, int kind, uint32_t w, uint32_t h, const std::vector<uint8_t> &rgba, const std::vector<uint8_t> &astc, std::vector<uint8_t> &back) {
    astcenc_context *ctx = E.get(b, mode); back.resize(rgba.size());
    void *slice = back.data(); astcenc_image img; img.dim_x = w; img.dim_y = h; img.dim_z = 1; img.data_type = ASTCENC_TYPE_U8; img.data = &slice;
    astcenc_swizzle swz{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    if (astcenc_decompress_image(ctx, astc.data(), astc.size(), &img, &swz, 0) != ASTCENC_SUCCESS) { astcenc_decompress_reset(ctx); return 0; }
    astcenc_decompress_reset(ctx);
    bool ch[4] = {mode != 1, true, true, mode == 1 || (mode == 0 && kind != 1)}; double se = 0; uint64_t n = 0;
    for (size_t i = 0; i < rgba.size(); i += 4) for (int c = 0; c < 4; c++) if (ch[c]) { double d = (double)rgba[i + c] - back[i + c]; se += d * d; n++; }
    return se == 0 ? 99.0 : 10.0 * log10(65025.0 * (double)n / se);
}
static void worker() {
    Enc E; std::vector<uint8_t> rgba, out, back;
    for (;;) {
        Job j;
        { std::unique_lock<std::mutex> lk(g_mu); g_cv_get.wait(lk, [] { return !g_q.empty() || g_eof; }); if (g_q.empty()) break; j = std::move(g_q.back()); g_q.pop_back(); }
        g_cv_put.notify_one();
        int b = 4;
        for (size_t li = 0; li < j.lv.size(); li++) {
            Level &L = j.lv[li];
            decode(LvRef{j.kind, L.w, L.h, L.data}, rgba);
            bool ok = false;
            if (li == 0) {   // the first (largest) level picks the block size
                for (b = 6; b >= 4; b--) { ok = encode(E, b, j.mode, L.w, L.h, rgba, out); if (!ok || b == 4 || psnr(E, b, j.mode, j.kind, L.w, L.h, rgba, out, back) >= g_psnr_min[j.mode]) break; }
                g_pick[j.mode][b]++;
            } else ok = encode(E, b, j.mode, L.w, L.h, rgba, out);
            if (!ok) { g_fail++; break; }
            Entry e{}; uint32_t hdr[3] = {j.kind, L.w, L.h}; uint8_t dg[32];
            sha256_ctx s; sha256_init(&s); sha256_update(&s, (const uint8_t *)hdr, 12); sha256_update(&s, L.data.data(), L.data.size()); sha256_final(&s, dg);
            memcpy(e.key, dg, 16); e.len = (uint32_t)out.size(); e.bx = e.by = (uint8_t)b;
            std::lock_guard<std::mutex> lk(g_out_mu);
            e.off = g_data_len; if (fwrite(out.data(), 1, out.size(), g_data) != out.size()) g_fail++; g_data_len += out.size(); g_entries.push_back(e);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: bc2astc <out pack> [threads] < records\n"); return 2; }
    unsigned nt = argc > 2 ? (unsigned)atoi(argv[2]) : std::thread::hardware_concurrency(); if (!nt) nt = 2;
    std::string tmp = std::string(argv[1]) + ".data"; g_data = fopen(tmp.c_str(), "wb+"); if (!g_data) { perror("data file"); return 1; }
    uint32_t magic; if (fread(&magic, 4, 1, stdin) != 1 || magic != 0x324a4342) { fprintf(stderr, "[RED] bc2astc: not a BCJ2 stream\n"); return 1; }
    if (getenv("BC2ASTC_PSNR")) { double a, b2, c; if (sscanf(getenv("BC2ASTC_PSNR"), "%lf,%lf,%lf", &a, &b2, &c) == 3) { g_psnr_min[0] = a; g_psnr_min[1] = b2; g_psnr_min[2] = c; } }
    std::vector<std::thread> th; for (unsigned i = 0; i < nt; i++) th.emplace_back(worker);
    uint64_t njobs = 0, nlev = 0;
    for (;;) {
        uint8_t h[4]; size_t r = fread(h, 1, 4, stdin); if (r == 0) break; if (r != 4) { fprintf(stderr, "[RED] bc2astc: cut texture header\n"); return 1; }
        Job j; j.kind = h[0]; j.mode = h[1]; unsigned nl = h[2] | h[3] << 8;
        if (j.kind < 1 || j.kind > 3 || j.mode > 2 || !nl) { fprintf(stderr, "[RED] bc2astc: bad texture (kind %u, mode %u, %u levels)\n", j.kind, j.mode, nl); return 1; }
        for (unsigned k = 0; k < nl; k++) {
            uint32_t q[3]; if (fread(q, 4, 3, stdin) != 3) { fprintf(stderr, "[RED] bc2astc: cut level header\n"); return 1; }
            Level L; L.w = q[0]; L.h = q[1]; uint32_t want = ((L.w + 3) / 4) * ((L.h + 3) / 4) * (j.kind == 1 ? 8u : 16u);
            if (!L.w || !L.h || q[2] != want) { fprintf(stderr, "[RED] bc2astc: bad level (kind %u, %ux%u, %u bytes)\n", j.kind, L.w, L.h, q[2]); return 1; }
            L.data.resize(q[2]); if (fread(L.data.data(), 1, q[2], stdin) != q[2]) { fprintf(stderr, "[RED] bc2astc: cut level data\n"); return 1; }
            j.lv.push_back(std::move(L)); nlev++;
        }
        njobs++;
        std::unique_lock<std::mutex> lk(g_mu); g_cv_put.wait(lk, [&] { return g_q.size() < nt * 4; }); g_q.push_back(std::move(j)); lk.unlock(); g_cv_get.notify_one();
    }
    { std::lock_guard<std::mutex> lk(g_mu); g_eof = true; } g_cv_get.notify_all();
    for (auto &t : th) t.join();
    if (g_fail) { fprintf(stderr, "[RED] bc2astc: %d level(s) failed\n", g_fail.load()); return 1; }
    std::sort(g_entries.begin(), g_entries.end(), [](const Entry &a, const Entry &b) { int c = memcmp(a.key, b.key, 16); return c ? c < 0 : (a.bx != b.bx ? a.bx < b.bx : a.by < b.by); });
    // the same level asked for twice with the same block size is one entry
    std::vector<Entry> uniq; for (auto &e : g_entries) if (uniq.empty() || memcmp(uniq.back().key, e.key, 16) || uniq.back().bx != e.bx || uniq.back().by != e.by) uniq.push_back(e);
    FILE *o = fopen(argv[1], "wb"); if (!o) { perror("pack"); return 1; }
    uint32_t cnt = (uint32_t)uniq.size(), zero = 0; uint64_t base = 16 + (uint64_t)cnt * 32;
    fwrite("DMBCA1\0\0", 1, 8, o); fwrite(&cnt, 4, 1, o); fwrite(&zero, 4, 1, o);
    for (auto e : uniq) { e.off += base; fwrite(&e, 32, 1, o); }
    rewind(g_data); std::vector<uint8_t> buf(1 << 20); size_t n; while ((n = fread(buf.data(), 1, buf.size(), g_data)) > 0) fwrite(buf.data(), 1, n, o);
    fclose(o); fclose(g_data); remove(tmp.c_str());
    fprintf(stderr, "bc2astc: %llu textures, %llu levels in, %u entries, %.1f MB of ASTC blocks\n", (unsigned long long)njobs, (unsigned long long)nlev, cnt, g_data_len / 1048576.0);
    static const char *const mn[3] = {"colour", "normal map (green, alpha)", "normal map (RGB)"};
    for (int m = 0; m < 3; m++) fprintf(stderr, "bc2astc: %s: 6x6 %llu, 5x5 %llu, 4x4 %llu textures (PSNR floor %.0f dB)\n", mn[m], (unsigned long long)g_pick[m][6].load(), (unsigned long long)g_pick[m][5].load(), (unsigned long long)g_pick[m][4].load(), g_psnr_min[m]);
    return 0;
}
