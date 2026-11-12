// The performance record: one row per presented frame, kept for the whole session, for the diagnostic panel (the frame-time graph and its figures) and for "Export performance
// log" (a CSV of the rows and a JSON of the session: device, settings, changes made while playing, summary), which the owner of a device this project cannot test on sends back.
// The NVN layer calls dm_perf_frame at each present and dm_perf_gpu when that frame's GPU time is known (8 frames later; hle_nvn.c); everything else is read from here.
// Always on: a row is 36 bytes, a two-hour session at 60 fps 16 MB, and the record stops growing there (PERF_MAX_ROWS).
#include "hle.h"
#include "dm_perf.h"
#include <math.h>
#include <stdatomic.h>

#define PERF_CHUNK 16384
#define PERF_MAX_ROWS (PERF_CHUNK * 27)   // 442,368 frames: two hours at 60 fps
static dm_perf_row *g_chunk[PERF_MAX_ROWS / PERF_CHUNK]; static _Atomic uint64_t g_rows; static uint64_t g_first_frame; static int g_have_first;
static pthread_mutex_t g_perf_mu = PTHREAD_MUTEX_INITIALIZER;
#define PERF_MARKS 256
static struct { double t_s; uint64_t row; char text[96]; } g_mark[PERF_MARKS]; static int g_nmark;
static uint64_t g_t0;

__attribute__((weak)) int dm_thermal_now(void);
void dm_mem_now(uint64_t *use, uint64_t *peak, uint64_t *budget, uint64_t *avail, uint64_t *metal);
__attribute__((weak)) void dm_mem_now(uint64_t *use, uint64_t *peak, uint64_t *budget, uint64_t *avail, uint64_t *metal) { if (use) *use = 0; if (peak) *peak = 0; if (budget) *budget = 0; if (avail) *avail = 0; if (metal) *metal = 0; }
// What the graphics layer renders and shows (nvn_mtl.m); 0 x 0 before the first frame and in builds without it
__attribute__((weak)) void mtl_perf_sizes(uint32_t *rw, uint32_t *rh, uint32_t *ow, uint32_t *oh) { *rw = *rh = *ow = *oh = 0; }

static dm_perf_row *row_at(uint64_t i) { return i < PERF_MAX_ROWS && g_chunk[i / PERF_CHUNK] ? &g_chunk[i / PERF_CHUNK][i % PERF_CHUNK] : NULL; }

void dm_perf_frame(uint64_t frame, double frame_ms, double work_ms, unsigned passes, unsigned draws, double rt_load_mb, double rt_store_mb, unsigned pso_new) {
    if (!g_t0) g_t0 = now_ns();
    uint64_t i = atomic_load(&g_rows); if (i >= PERF_MAX_ROWS) return;
    if (!g_have_first) { g_first_frame = frame; g_have_first = 1; }
    if (!g_chunk[i / PERF_CHUNK]) { dm_perf_row *c = calloc(PERF_CHUNK, sizeof *c); if (!c) return; pthread_mutex_lock(&g_perf_mu); g_chunk[i / PERF_CHUNK] = c; pthread_mutex_unlock(&g_perf_mu); }
    dm_perf_row *r = row_at(i); uint64_t use = 0; dm_mem_now(&use, NULL, NULL, NULL, NULL);
    uint32_t rw, rh, ow, oh; mtl_perf_sizes(&rw, &rh, &ow, &oh);
    r->frame = (uint32_t)frame; r->t_s = (float)((now_ns() - g_t0) / 1e9); r->frame_ms = (float)frame_ms; r->cpu_ms = (float)work_ms; r->gpu_ms = -1;
    r->passes = (uint16_t)(passes > 65535 ? 65535 : passes); r->draws = (uint16_t)(draws > 65535 ? 65535 : draws);
    r->rt_mb = (float)(rt_load_mb + rt_store_mb); r->pso_new = (uint8_t)(pso_new > 255 ? 255 : pso_new);
    r->thermal = (int8_t)(dm_thermal_now ? dm_thermal_now() : -1); r->mem_mb = (uint16_t)(use >> 20 > 65535 ? 65535 : use >> 20);
    r->render_w = (uint16_t)rw; r->render_h = (uint16_t)rh;
    atomic_store(&g_rows, i + 1);
}
void dm_perf_gpu(uint64_t frame, double gpu_ms) {
    if (!g_have_first || frame < g_first_frame) return;
    uint64_t n = atomic_load(&g_rows);
    // rows are in frame order, one a frame: the row is 8 or so back from the end
    for (uint64_t k = 0; k < 64 && k < n; k++) { dm_perf_row *r = row_at(n - 1 - k); if (r && r->frame == (uint32_t)frame) { r->gpu_ms = (float)gpu_ms; return; } }
}
void dm_perf_mark(const char *what) {
    pthread_mutex_lock(&g_perf_mu);
    if (g_nmark < PERF_MARKS) { g_mark[g_nmark].t_s = g_t0 ? (now_ns() - g_t0) / 1e9 : 0; g_mark[g_nmark].row = atomic_load(&g_rows); snprintf(g_mark[g_nmark].text, sizeof g_mark[g_nmark].text, "%s", what); g_nmark++; }
    pthread_mutex_unlock(&g_perf_mu);
    LOG("perf: %s", what);
}
uint64_t dm_perf_rows(void) { return atomic_load(&g_rows); }
int dm_perf_recent(dm_perf_row *out, int max) {
    uint64_t n = atomic_load(&g_rows); int c = (int)(n < (uint64_t)max ? n : (uint64_t)max);
    for (int k = 0; k < c; k++) out[k] = *row_at(n - (uint64_t)c + (uint64_t)k);
    return c;
}
static int fcmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static float pct(float *v, int n, double p) { if (!n) return -1; int k = (int)(n * p); return v[k < n ? k : n - 1]; }
// Figures over the rows [from, to): frames per second from the frame times, percentiles of the frame time, means of the CPU and GPU times (GPU: the rows that have one)
void dm_perf_window(uint64_t from, uint64_t to, dm_perf_sum *s) {
    memset(s, 0, sizeof *s); uint64_t n = atomic_load(&g_rows); if (to > n) to = n; if (from >= to) { s->gpu_ms = s->gpu_p95 = -1; return; }
    int c = (int)(to - from); float *ft = malloc(sizeof(float) * (size_t)c * 2), *gt = ft + c; int ng = 0; double sf = 0, sc = 0, sg = 0, sd = 0, sp = 0, sr = 0;
    if (!ft) return;
    for (int k = 0; k < c; k++) { const dm_perf_row *r = row_at(from + (uint64_t)k); ft[k] = r->frame_ms; sf += r->frame_ms; sc += r->cpu_ms; sd += r->draws; sp += r->passes; sr += r->rt_mb;
        if (r->gpu_ms >= 0) { gt[ng++] = r->gpu_ms; sg += r->gpu_ms; } }
    qsort(ft, (size_t)c, sizeof *ft, fcmp); qsort(gt, (size_t)ng, sizeof *gt, fcmp);
    s->frames = (uint32_t)c; s->fps = sf > 0 ? (float)(c * 1000.0 / sf) : 0; s->frame_p50 = pct(ft, c, 0.50); s->frame_p95 = pct(ft, c, 0.95); s->frame_p99 = pct(ft, c, 0.99); s->frame_max = ft[c - 1];
    s->cpu_ms = (float)(sc / c); s->gpu_ms = ng ? (float)(sg / ng) : -1; s->gpu_p95 = pct(gt, ng, 0.95); s->draws = (float)(sd / c); s->passes = (float)(sp / c); s->rt_mb = (float)(sr / c);
    const dm_perf_row *last = row_at(to - 1); s->render_w = last->render_w; s->render_h = last->render_h; s->thermal = last->thermal; s->mem_mb = last->mem_mb;
    free(ft);
}
// The last second or so, for the panel's figures
void dm_perf_now(dm_perf_sum *s) {
    uint64_t n = atomic_load(&g_rows), from = n; double ms = 0;
    while (from > 0 && ms < 1000.0 && n - from < 600) { from--; ms += row_at(from)->frame_ms; }
    dm_perf_window(from, n, s);
    uint32_t rw, rh, ow, oh; mtl_perf_sizes(&rw, &rh, &ow, &oh); s->out_w = (uint16_t)ow; s->out_h = (uint16_t)oh; if (rw) { s->render_w = (uint16_t)rw; s->render_h = (uint16_t)rh; }
}

static void json_str(FILE *f, const char *s) { fputc('"', f); for (; s && *s; s++) { if (*s == '"' || *s == '\\') fputc('\\', f); if ((unsigned char)*s >= 0x20) fputc(*s, f); } fputc('"', f); }
// Writes <base>.csv (one row a frame) and <base>.json (the session: info is a JSON object the front end supplies with the device and the settings, then the changes made while
// playing, the summary of the whole record and of each stretch between changes). Returns 0, or -1 when a file could not be written
int dm_perf_export(const char *base, const char *info_json) {
    char p[1100]; uint64_t n = atomic_load(&g_rows);
    snprintf(p, sizeof p, "%s.csv", base); FILE *f = fopen(p, "w"); if (!f) return -1;
    fprintf(f, "frame,t_s,frame_ms,cpu_ms,gpu_ms,draws,passes,rt_load_store_mb,pso_new,mem_mb,thermal,render_w,render_h\n");
    for (uint64_t i = 0; i < n; i++) { const dm_perf_row *r = row_at(i);
        fprintf(f, "%u,%.3f,%.3f,%.3f,%.3f,%u,%u,%.2f,%u,%u,%d,%u,%u\n", r->frame, r->t_s, r->frame_ms, r->cpu_ms, r->gpu_ms, r->draws, r->passes, r->rt_mb, r->pso_new, r->mem_mb, r->thermal, r->render_w, r->render_h); }
    if (fclose(f)) return -1;
    snprintf(p, sizeof p, "%s.json", base); f = fopen(p, "w"); if (!f) return -1;
    uint64_t use, peak, budget, avail, metal; dm_mem_now(&use, &peak, &budget, &avail, &metal);
    fprintf(f, "{\n  \"format\": \"nxnative performance log 1\",\n  \"info\": %s,\n  \"frames\": %llu,\n  \"record_full\": %s,\n", info_json && *info_json ? info_json : "{}", (unsigned long long)n, n >= PERF_MAX_ROWS ? "true" : "false");
    fprintf(f, "  \"memory_mb\": {\"in_use\": %llu, \"peak\": %llu, \"budget\": %llu, \"left_before_limit\": %llu, \"metal_allocated\": %llu},\n", (unsigned long long)(use >> 20), (unsigned long long)(peak >> 20),
            (unsigned long long)(budget >> 20), (unsigned long long)(avail >> 20), (unsigned long long)(metal >> 20));
    pthread_mutex_lock(&g_perf_mu);
    fprintf(f, "  \"stretches\": [\n");
    for (int k = -1; k < g_nmark; k++) {   // the stretch before the first change, then one from each change to the next
        uint64_t a = k < 0 ? 0 : g_mark[k].row, b = k + 1 < g_nmark ? g_mark[k + 1].row : n; dm_perf_sum s; dm_perf_window(a, b, &s);
        fprintf(f, "    {\"from_row\": %llu, \"to_row\": %llu, \"t_s\": %.1f, \"change\": ", (unsigned long long)a, (unsigned long long)b, k < 0 ? 0.0 : g_mark[k].t_s); json_str(f, k < 0 ? "start" : g_mark[k].text);
        fprintf(f, ", \"fps\": %.2f, \"frame_ms\": {\"p50\": %.2f, \"p95\": %.2f, \"p99\": %.2f, \"max\": %.2f}, \"cpu_ms\": %.2f, \"gpu_ms\": %.2f, \"gpu_ms_p95\": %.2f, \"draws\": %.0f, \"passes\": %.0f, \"rt_load_store_mb\": %.1f, "
                   "\"render\": [%u, %u], \"thermal\": %d, \"mem_mb\": %u}%s\n", s.fps, s.frame_p50, s.frame_p95, s.frame_p99, s.frame_max, s.cpu_ms, s.gpu_ms, s.gpu_p95, s.draws, s.passes, s.rt_mb, s.render_w, s.render_h,
                s.thermal, s.mem_mb, k + 1 < g_nmark ? "," : "");
    }
    pthread_mutex_unlock(&g_perf_mu);
    dm_perf_sum all; dm_perf_window(0, n, &all);
    fprintf(f, "  ],\n  \"all\": {\"fps\": %.2f, \"frame_ms\": {\"p50\": %.2f, \"p95\": %.2f, \"p99\": %.2f, \"max\": %.2f}, \"cpu_ms\": %.2f, \"gpu_ms\": %.2f, \"gpu_ms_p95\": %.2f}\n}\n",
            all.fps, all.frame_p50, all.frame_p95, all.frame_p99, all.frame_max, all.cpu_ms, all.gpu_ms, all.gpu_p95);
    return fclose(f) ? -1 : 0;
}
// DM_PERF_EXPORT=<base path>: the same export when the process ends normally (the benchmark in CI and on the Mac, where there is no panel)
__attribute__((destructor)) static void perf_export_at_exit(void) { const char *b = getenv("DM_PERF_EXPORT"); if (b && *b && atomic_load(&g_rows)) dm_perf_export(b, getenv("DM_PERF_INFO")); }
