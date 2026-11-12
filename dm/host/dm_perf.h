// The performance record (hle_perf.c): one row per presented frame, read by the diagnostic panel and written out by "Export performance log".
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t frame;          // the game's frame number
    float t_s;               // seconds since the first recorded frame
    float frame_ms;          // start of this frame to start of the next (what the player sees as the frame rate)
    float cpu_ms;            // the game's and the host's work in the frame, without the wait for the display
    float gpu_ms;            // GPU time of the frame's command buffers; -1 until it is known (8 frames later) or when the GPU reports none
    float rt_mb;             // render-target bytes loaded and stored in the frame's passes, in MiB (tile memory traffic)
    uint16_t draws, passes;
    uint16_t mem_mb;         // what iOS counts against the app's memory limit
    uint16_t render_w, render_h;   // the 3D picture's size
    uint8_t pso_new;         // pipelines built during the frame (a stutter's usual cause)
    int8_t thermal;          // NSProcessInfo.thermalState, -1 where there is none
} dm_perf_row;

typedef struct {
    uint32_t frames;
    float fps, frame_p50, frame_p95, frame_p99, frame_max, cpu_ms, gpu_ms, gpu_p95, draws, passes, rt_mb;
    uint16_t render_w, render_h, out_w, out_h, mem_mb;
    int8_t thermal;
} dm_perf_sum;

void dm_perf_frame(uint64_t frame, double frame_ms, double work_ms, unsigned passes, unsigned draws, double rt_load_mb, double rt_store_mb, unsigned pso_new);
void dm_perf_gpu(uint64_t frame, double gpu_ms);
void dm_perf_mark(const char *what);              // a change made while playing (a setting), noted with the row it took effect at
uint64_t dm_perf_rows(void);
int dm_perf_recent(dm_perf_row *out, int max);    // the newest rows, oldest first; returns how many
void dm_perf_window(uint64_t from, uint64_t to, dm_perf_sum *s);
void dm_perf_now(dm_perf_sum *s);                 // the last second
int dm_perf_export(const char *base, const char *info_json);   // <base>.csv and <base>.json; 0 on success
