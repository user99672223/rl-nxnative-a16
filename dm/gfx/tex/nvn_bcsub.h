// BC textures on a GPU without BC texture formats (an A16 iPhone, or any device with NX_FORCE_ASTC_ONLY=1): each level the game uploads is looked up by its content in an ASTC pack
// made offline (dm/tools/ue3_astc.py, dm/tools/bc_astc/bc2astc.cpp) and uploaded as ASTC; a level the pack does not hold is decoded on the CPU to RGBA8 (BC1, BC2, BC3) or R8 / RG8
// (BC4, BC5). Pure C: the replay layer (nvn_mtl.m) does the Metal part.
#pragma once
#include <stddef.h>
#include <stdint.h>

// The BC kind of an NVN format: 1 BC1, 2 BC2, 3 BC3, 4 BC4, 5 BC5, 6 BC6H, 7 BC7; 0 for a format that is not BC. *srgb, *sign: an sRGB / a signed format
int nvn_bcsub_kind(int nvn_format, int *srgb, int *sign);
// Maps the pack at path (mmap, read only). 0 when it is in use; -1 when there is no such file or it is not a pack (the host then decodes every level)
int nvn_bcsub_open(const char *path);
int nvn_bcsub_count(void);   // entries of the open pack, 0 without one
// A level's key: the first 16 bytes of SHA-256(u32 kind, u32 width, u32 height, the level's linear blocks), as bc2astc writes it
void nvn_bcsub_key(int kind, uint32_t w, uint32_t h, const void *blocks, size_t n, uint8_t key[16]);
// Block sizes the pack holds this level in: bit b set for b x b (4, 5, 6); 0 when it holds none
unsigned nvn_bcsub_sizes(const uint8_t key[16]);
// The level's ASTC blocks at block size b: ceil(w / b) * ceil(h / b) * 16 bytes inside the mapped pack, valid for the whole run; NULL when the pack has no such entry
const uint8_t *nvn_bcsub_find(const uint8_t key[16], int b, uint32_t *len);
// CPU decode of a level's linear blocks: BC1 / BC2 / BC3 to RGBA8 (4 bytes a texel), BC4 to R8 (1), BC5 to RG8 (2); rows of w texels, h rows. sign: BC4 / BC5 signed
// (out is then signed bytes). Returns the bytes per texel written, 0 for a kind it does not decode (BC6H, BC7)
int nvn_bcsub_decode(int kind, int sign, uint32_t w, uint32_t h, const void *blocks, uint8_t *out);
// A level the pack lacks, for an ASTC texture: a flat-colour ASTC block (the mean of its texels) per b x b block of the decoded level rgba; out takes ceil(w / b) * ceil(h / b) * 16 bytes
void nvn_bcsub_void_blocks(const uint8_t *rgba, uint32_t w, uint32_t h, int b, uint8_t *out);
// Counters for the log and the diagnostic panel
typedef struct { uint64_t hit, miss, hit_bytes, miss_bytes, key_ns; } nvn_bcsub_stats;
extern nvn_bcsub_stats g_bcsub_stats;
