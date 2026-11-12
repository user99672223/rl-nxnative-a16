// Unreal Engine 3's system settings, held from the host: the engine keeps its render and quality settings (FSystemSettings: ScreenPercentage, MaxAnisotropy, DynamicShadows,
// MotionBlur, Bloom, ...) in one global object and describes each with a row of a table in main's data: {u32 type, u32 flags, name, address of the variable, range, help text, 0},
// 48 bytes, the texts UTF-32 (the game's TCHAR). The table gives every setting's name and where its value is, in any build, so nothing here is a fixed address: the table is found
// by its rows' shape (ue3_find), once, at the first use.
// Found on Rocket League 1.0 (Panic Button's Unreal Engine 3, SDK 3.5.0): 101 rows at main+0x2a4da08, the variables in bss at main+0x2e2adb8..+0x2e2b214; it also carries that
// port's own rows (UpscaleScreenPercentage, UpscaleTargetFramerateDocked / Undocked, MinimumScreenScale, AllowDynamicResolution).
//
//   DM_UE3_SET=Name=value,Name=value   values the host holds: written when the game has loaded its own (the first frame) and again whenever the game changes them (it applies a
//                                      settings bucket when the console is docked or undocked, and its dynamic resolution moves ScreenPercentage)
//   DM_UE3_DUMP=1                      logs every setting with its value at frames 1 and 600 (what the game's config gave it)
//   dm_ue3_set / dm_ue3_get / dm_ue3_list   the same while the game runs (the diagnostic panel; DM_CTL "ue3 Name=value", "ue3 Name=" stops holding it, "ue3dump")
// Types: 2 int32, 4 float, 5 a bool kept as an int32. A setting the engine reads every frame takes effect at once (ScreenPercentage, the post-process switches, shadow sizes);
// one it reads when it creates something (MaxAnisotropy at sampler creation, texture pool sizes) only for what is created after.
#include "hle.h"
#include <stdatomic.h>

enum { UE3_INT = 2, UE3_FLOAT = 4, UE3_BOOL = 5 };
#define UE3_MAX 256
#define DM_UE3_KICK_DEFAULT "merge"   // seen on an iPhone 15 (2026-11-12): switched on mid-match, 87 -> 76 passes a frame and 38 -> 51 fps, no change in the picture reported
#define UE3_IMAGE_MAX ((uint64_t)1 << 30)   // no main with its bss is larger
typedef struct { char name[48]; int type; uint8_t *var; int held; uint32_t want; uint32_t orig; int have_orig; } ue3_row;
static ue3_row g_row[UE3_MAX]; static int g_nrow; static pthread_once_t g_once = PTHREAD_ONCE_INIT; static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_any_held;

// A UTF-32 text at p inside the module, of printable ASCII, into out; its length, or 0
static int u32_text(const module_t *m, uint64_t p, char *out, int cap) {
    if (p < (uint64_t)m->base || p + 8 > (uint64_t)m->base + m->size || (p & 3)) return 0;
    const uint32_t *s = (const uint32_t *)p; int n = 0;
    while ((uint64_t)(s + n + 1) <= (uint64_t)m->base + m->size && s[n]) { if (s[n] < 0x20 || s[n] > 0x7e || n + 1 >= cap) return 0; out[n] = (char)s[n]; n++; }
    out[n] = 0; return n;
}
static int row_at(const module_t *m, const uint8_t *e, ue3_row *r) {
    uint32_t type; uint64_t name, var, help, zero; memcpy(&type, e, 4); memcpy(&name, e + 8, 8); memcpy(&var, e + 0x10, 8); memcpy(&help, e + 0x20, 8); memcpy(&zero, e + 0x28, 8);
    char h[8];
    if ((type != UE3_INT && type != UE3_FLOAT && type != UE3_BOOL) || zero > 0xff) return 0;   // the last field is 0 in the file and a small flag once the engine has read its config (1 in Rocket League 1.0)
    if (var < (uint64_t)m->base || var + 4 > (uint64_t)m->base + UE3_IMAGE_MAX || (var & 3)) return 0;   // the variables are in main's bss, past the file's own bytes
    if (u32_text(m, name, r->name, sizeof r->name) < 3) return 0;
    if (help && !(help >= (uint64_t)m->base && help < (uint64_t)m->base + m->size)) return 0;
    (void)h; r->type = (int)type; r->var = (uint8_t *)var; r->held = 0; r->have_orig = 0;
    return 1;
}
// The table: the longest run of consecutive 48-byte rows in main's data that includes one named ScreenPercentage
static void ue3_find(void) {
    const module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m) return;
    static const uint32_t key[] = {'S','c','r','e','e','n','P','e','r','c','e','n','t','a','g','e',0};
    uint8_t *b = m->base; uint64_t n = m->size;
    for (uint64_t o = 4; o + sizeof key <= n; o += 4) {
        if (memcmp(b + o, key, sizeof key)) continue;   // the text up to its end: in Rocket League 1.0 it is the tail of UpscaleScreenPercentage's (the linker merged them), and the row points into it
        uint64_t want = (uint64_t)(b + o); int ptrs = 0;
        LOG("ue3 settings: the text ScreenPercentage at main+%#llx", (unsigned long long)o);
        for (uint64_t s = 8; s + 48 <= n; s += 8) {   // the row whose name pointer is that text
            uint64_t v; memcpy(&v, b + s, 8); if (v != want) continue;
            ue3_row r; ptrs++;
            if (!row_at(m, b + s - 8, &r)) { uint64_t q[6]; memcpy(q, b + s - 8, 48); LOG("ue3 settings: main+%#llx points to it but is no row: %#llx %#llx %#llx %#llx %#llx %#llx (main at %p, %#llx bytes)", (unsigned long long)(s - 8), (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4], (unsigned long long)q[5], (void *)b, (unsigned long long)n); continue; }
            uint64_t first = s - 8; while (first >= 48 && row_at(m, b + first - 48, &r)) first -= 48;
            int k = 0; for (uint64_t e = first; e + 48 <= n && k < UE3_MAX && row_at(m, b + e, &g_row[k]); e += 48) k++;
            if (k < 8) { LOG("ue3 settings: only %d rows around main+%#llx", k, (unsigned long long)(s - 8)); continue; }   // a lone look-alike
            g_nrow = k;
            LOG("ue3 settings: %d system settings described at main+%#llx (the variables at main+%#llx..)", k, (unsigned long long)first, (unsigned long long)(g_row[0].var - b));
            return;
        }
        LOG("ue3 settings: %d pointers to that text, none in a table", ptrs);
    }
    LOG("ue3 settings: no table found in main (%p, %#llx bytes)", (void *)b, (unsigned long long)n);
}
static ue3_row *find_row(const char *name) {
    pthread_once(&g_once, ue3_find);
    for (int i = 0; i < g_nrow; i++) if (!strcasecmp(g_row[i].name, name)) return &g_row[i];
    return NULL;
}
static uint32_t parse(const ue3_row *r, const char *v, int *ok) {
    char *end; uint32_t w = 0; *ok = 1;
    if (r->type == UE3_FLOAT) { float f = strtof(v, &end); if (end == v || *end) *ok = 0; memcpy(&w, &f, 4); }
    else if (r->type == UE3_BOOL && (!strcasecmp(v, "true") || !strcasecmp(v, "on"))) w = 1;
    else if (r->type == UE3_BOOL && (!strcasecmp(v, "false") || !strcasecmp(v, "off"))) w = 0;
    else { long x = strtol(v, &end, 0); if (end == v || *end) *ok = 0; w = (uint32_t)(int32_t)x; if (r->type == UE3_BOOL) w = x != 0; }
    return w;
}
static void fmt(const ue3_row *r, uint32_t w, char *out, size_t n) {
    if (r->type == UE3_FLOAT) { float f; memcpy(&f, &w, 4); snprintf(out, n, "%g", f); } else snprintf(out, n, "%d", (int32_t)w);
}
// Holds a setting at a value ("" or NULL: stops holding it and puts back what the game had). 0 on success, -1 for a name the table lacks or a value that is no number
int dm_ue3_set(const char *name, const char *value) {
    ue3_row *r = find_row(name); if (!r) { LOG("[RED] ue3 settings: no setting named %s", name); return -1; }
    pthread_mutex_lock(&g_mu);
    if (!value || !*value) {
        if (r->held && r->have_orig) memcpy(r->var, &r->orig, 4);
        r->held = 0; r->have_orig = 0;
    } else {
        int ok; uint32_t w = parse(r, value, &ok);
        if (!ok) { pthread_mutex_unlock(&g_mu); LOG("[RED] ue3 settings: %s=%s is not a value of its type", name, value); return -1; }
        r->want = w; r->held = 1;
    }
    int any = 0; for (int i = 0; i < g_nrow; i++) any |= g_row[i].held;
    atomic_store(&g_any_held, any);
    pthread_mutex_unlock(&g_mu);
    LOG("ue3 settings: %s %s%s", r->name, value && *value ? "held at " : "left to the game", value && *value ? value : "");
    return 0;
}
// A setting's current value as text, and whether the host holds it; -1 for a name the table lacks
int dm_ue3_get(const char *name, char *out, size_t n, int *held) {
    ue3_row *r = find_row(name); if (!r) return -1;
    uint32_t w; memcpy(&w, r->var, 4); fmt(r, w, out, n); if (held) *held = r->held;
    return 0;
}
// Row i of the table for a list (the panel): its name and type (2 int, 4 float, 5 bool); -1 past the end
int dm_ue3_list(int i, const char **name, int *type) {
    pthread_once(&g_once, ue3_find);
    if (i < 0 || i >= g_nrow) return -1;
    *name = g_row[i].name; *type = g_row[i].type; return 0;
}
void dm_ue3_dump(const char *why) {
    pthread_once(&g_once, ue3_find);
    if (!g_nrow) { LOG("ue3 settings: no system settings table in main"); return; }
    char line[1800]; size_t o = 0;
    for (int i = 0; i < g_nrow; i++) {
        uint32_t w; memcpy(&w, g_row[i].var, 4); char v[32]; fmt(&g_row[i], w, v, sizeof v);
        o += (size_t)snprintf(line + o, sizeof line - o, " %s=%s%s", g_row[i].name, v, g_row[i].held ? "*" : "");
        if (o > 1500 || i + 1 == g_nrow) { LOG("ue3 settings (%s; * held by the host):%s", why, line); o = 0; }
    }
}
// Every present (hle_nvn.c): the held settings are written wherever the game has another value
static void ue3_tick(int present);
// When the game makes its NVN device (hle_nvn.c): Unreal has read its settings by then and sizes its window and render targets from ResX / ResY right after
// (main+0xab43cc), so settings held from the start must be in place before the first present
void dm_ue3set_early(void) { ue3_tick(0); }
void dm_ue3set_tick(void) { ue3_tick(1); }
// ---------- The engine's frame-rate smoothing (UEngine::GetMaxTickRate) ----------
// With bSmoothFrameRate the engine limits each frame to the running average of the last 300 frame times, kept between MinSmoothedFrameRate and MaxSmoothedFrameRate (22 and 62 in
// Rocket League's config). Each frame is held to the average, so no frame can be shorter than it: any wait that overshoots lowers the average for good, down to the minimum.
// Rocket League 1.0 on a virtual Mac settled at 25.9 fps with its game thread asleep in the limiter (main+0xae6154), 45 % of one core in use. The display's vsync already paces
// the frames, so the smoothing is held flat: the minimum is written to the maximum, and the limit is the game's own 62 whatever the average says.
// DM_UE3_SMOOTH=game leaves the engine's smoothing as shipped. Fixed offsets, so keyed by build: main+0x2d0ca40 is the GOT entry of GEngine, +0x754 the maximum, +0x758 the minimum
// (main+0x40f074), +0x440 bit 0x200 bSmoothFrameRate.
static const struct { const char *build; uint64_t engine; uint32_t max, min; } k_smooth[] = {
    { "becfed979bb4204fbd0945cd450fb296", 0x2d0ca40, 0x754, 0x758 },
};
static _Atomic int g_smooth_on = -1;
static void ue3_smooth_tick(void) {
    static const module_t *m; static int row = -2; static float orig; static int have_orig, said;
    if (row == -2) {
        row = -1;
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
        for (int i = 0; m && i < (int)(sizeof k_smooth / sizeof k_smooth[0]); i++) if (!strcmp(m->build_id, k_smooth[i].build)) row = i;
    }
    { static int once; if (!once++) LOG("ue3 settings: frame-rate smoothing: main build %s, %s", m ? m->build_id : "(no main)", row < 0 ? "no row for it" : "row found"); }
    if (row < 0) return;
    int on = atomic_load(&g_smooth_on);
    if (on < 0) { const char *e = getenv("DM_UE3_SMOOTH"); on = e && !strcmp(e, "game") ? 0 : e && !strcmp(e, "off") ? 2 : 1; atomic_store(&g_smooth_on, on); }
    uint64_t slot, eng; memcpy(&slot, m->base + k_smooth[row].engine, 8);   // main's GOT entry of GEngine
    if (slot < (uint64_t)m->base || slot + 8 > (uint64_t)m->base + m->size) return;
    memcpy(&eng, (void *)slot, 8); if (eng < 0x10000) return;   // GEngine is not made yet
    float mx, mn; memcpy(&mx, (uint8_t *)eng + k_smooth[row].max, 4); memcpy(&mn, (uint8_t *)eng + k_smooth[row].min, 4);
    { static int n; if (n < 3 && ++n) LOG("ue3 settings: frame-rate smoothing: GEngine %#llx, maximum %g, minimum %g", (unsigned long long)eng, mx, mn); }
    if (!(mx >= 30.0f && mx <= 480.0f) || !(mn >= 1.0f && mn <= 480.0f)) return;   // not the config's values (yet)
    // off: bSmoothFrameRate (bit 0x200 of the flags at +0x440) is cleared, so GetMaxTickRate returns 0 and the engine's limiter waits for nothing: only the display paces the frames
    { static int cleared; uint32_t fl; memcpy(&fl, (uint8_t *)eng + 0x440, 4);
      if (on == 2 && (fl & 0x200)) { fl &= ~0x200u; memcpy((uint8_t *)eng + 0x440, &fl, 4); if (!cleared++) LOG("ue3 settings: the engine's frame-rate limit is off (bSmoothFrameRate cleared): the display paces the frames"); }
      else if (on != 2 && cleared && !(fl & 0x200)) { fl |= 0x200; memcpy((uint8_t *)eng + 0x440, &fl, 4); cleared = 0; } }
    if (on && mn != mx) {
        if (!have_orig) { orig = mn; have_orig = 1; }
        memcpy((uint8_t *)eng + k_smooth[row].min, &mx, 4);
        if (!said++) LOG("ue3 settings: frame-rate smoothing held flat at the game's maximum, %g fps (its minimum was %g; DM_UE3_SMOOTH=game leaves it)", mx, mn);
    } else if (!on && have_orig && mn == mx) { memcpy((uint8_t *)eng + k_smooth[row].min, &orig, 4); have_orig = 0; said = 0; }
}
// DM_UE3_NAMES=<file> (diagnostic): Unreal's name table (FName index → text) written to the file every 600 presented frames, so the FName indices a probe prints (a
// UFunction's name at +0x48, a key, a delegate) can be read. GNames is a TArray<FNameEntry*> at a fixed offset of the build; an entry holds index << 1 | wide at +8 and its text
// at +0x18 (ANSI, or UTF-32 when wide)
static void ue3_names_dump(void) {
    static const struct { const char *build; uint64_t names; } k_names[] = { { "becfed979bb4204fbd0945cd450fb296", 0x2d99d80 } };
    const char *path = getenv("DM_UE3_NAMES"); if (!path || !*path) return;
    const module_t *m = NULL; uint64_t off = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    for (int i = 0; m && i < (int)(sizeof k_names / sizeof k_names[0]); i++) if (!strcmp(m->build_id, k_names[i].build)) off = k_names[i].names;
    if (!off) return;
    uint64_t data; int32_t n; memcpy(&data, m->base + off, 8); memcpy(&n, m->base + off + 8, 4);
    if (data < 0x10000 || n <= 0 || n > 4000000) return;
    FILE *f = fopen(path, "w"); if (!f) return;
    for (int32_t i = 0; i < n; i++) {
        uint64_t e; memcpy(&e, (void *)(data + 8ull * i), 8); if (e < 0x10000) continue;
        uint32_t fl; memcpy(&fl, (void *)(e + 8), 4); char t[200]; int k = 0;
        if (fl & 1) { const uint32_t *w = (const uint32_t *)(e + 0x18); while (k < 199 && w[k]) { t[k] = w[k] >= 0x20 && w[k] < 0x7f ? (char)w[k] : '?'; k++; } }
        else { const char *c = (const char *)(e + 0x18); while (k < 199 && c[k]) { t[k] = c[k] >= 0x20 && c[k] < 0x7f ? c[k] : '?'; k++; } }
        t[k] = 0; fprintf(f, "%d\t%s\n", i, t);
    }
    fclose(f);
}
static void ue3_tick(int present) {
    static int inited; static uint64_t frame; static int dump = -1;
    frame += present;
    if (!inited) {
        inited = 1;
        const char *e = getenv("DM_UE3_SET");
        if (e && *e) {
            char *s = strdup(e), *save = NULL;
            for (char *t = strtok_r(s, ",", &save); t; t = strtok_r(NULL, ",", &save)) { char *eq = strchr(t, '='); if (!eq) { LOG("[RED] DM_UE3_SET: %s has no value", t); continue; } *eq = 0; dm_ue3_set(t, eq + 1); }
            free(s);
        }
        const char *d = getenv("DM_UE3_DUMP"); dump = d && atoi(d) > 0;
        { void dm_nx_kick_start(void); dm_nx_kick_start(); }
        { void dm_nx_settings_start(void); extern char **environ; for (char **q = environ; *q; q++) if (!strncmp(*q, "DM_UE3_", 7) || !strncmp(*q, "DM_UE3S_", 8)) { dm_nx_settings_start(); break; } }
    }
    if (present) ue3_smooth_tick();
    if (present && frame % 600 == 300) ue3_names_dump();
    if (dump && !present) dm_ue3_dump("device initialisation");
    if (dump && present && (frame == 1 || frame == 600)) dm_ue3_dump(frame == 1 ? "first frame" : "frame 600");
    if (!atomic_load(&g_any_held)) return;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nrow; i++) {
        ue3_row *r = &g_row[i]; if (!r->held) continue;
        uint32_t w; memcpy(&w, r->var, 4);
        if (w == r->want) continue;
        if (!r->have_orig) { r->orig = w; r->have_orig = 1; }
        memcpy(r->var, &r->want, 4);
    }
    pthread_mutex_unlock(&g_mu);
}

// ---------- The player's settings that are system settings underneath (dm/games/settings.tsv, shown for a profile with ue3_settings on) ----------
//   DM_UE3_SCALE=0.5..1.0     the 3D picture's size: below 1.0 ScreenPercentage is held at that percentage and the game's dynamic resolution is off (it would move it);
//                                1.0 leaves both to the game
//   DM_UE3_DYNRES=game|off           off holds AllowDynamicResolution at 0: the game renders its full size for the console mode
//   DM_UE3_DYNRES_MIN=0.5..1.0       with DM_UE3_DYNRES=game: the lowest scale its dynamic resolution may go to (MinimumScreenScale), 0 = the game's own
//   DM_UE3S_<Setting>=game|<value>   any row of the table by name (DM_UE3S_MotionBlur=0, DM_UE3S_MaxShadowResolution=512): game leaves it alone
// Read when the game starts (every present thread's first tick) and applied again by dm_nx_setting_live when the panel or the in-game menu changes one.
void dm_perf_mark(const char *what);
__attribute__((weak)) void dm_perf_mark(const char *what) { (void)what; }
static int ue3_kick_build(void) {   // the builds whose patches.py has the kick gate
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) return !strcmp(g_mod[i].build_id, "becfed979bb4204fbd0945cd450fb296");
    return 0;
}
static void nx_apply(const char *env, const char *v, int live) {
    char note[160]; snprintf(note, sizeof note, "%s=%s", env, v ? v : "");
    if (!strcmp(env, "DM_UE3_SCALE")) {
        float s = v ? strtof(v, NULL) : 1.0f;
        if (!(s >= 0.25f && s < 0.9999f)) { dm_ue3_set("ScreenPercentage", ""); if (!getenv("DM_UE3_DYNRES") || strcmp(getenv("DM_UE3_DYNRES"), "off")) dm_ue3_set("AllowDynamicResolution", ""); }
        else { char p[16]; snprintf(p, sizeof p, "%g", s * 100.0f); dm_ue3_set("ScreenPercentage", p); dm_ue3_set("AllowDynamicResolution", "0"); }
    } else if (!strcmp(env, "DM_UE3_DYNRES")) {
        const char *rs = getenv("DM_UE3_SCALE"); int scaled = rs && strtof(rs, NULL) < 0.9999f && strtof(rs, NULL) >= 0.25f;
        dm_ue3_set("AllowDynamicResolution", v && !strcmp(v, "off") ? "0" : scaled ? "0" : "");
    } else if (!strcmp(env, "DM_UE3_DYNRES_MIN")) {
        float s = v ? strtof(v, NULL) : 0; if (s >= 0.25f && s <= 1.0f) { char p[16]; snprintf(p, sizeof p, "%g", s); dm_ue3_set("MinimumScreenScale", p); } else dm_ue3_set("MinimumScreenScale", "");
    } else if (!strcmp(env, "DM_UE3_RES")) {
        // Not while the game runs: its window and render targets were made at the size it started with, and a new ResX / ResY under them draws a part of the picture in a
        // corner of the old targets (an iPhone 15, the in-game menu's Render resolution changed during a match). The launcher keeps the value for the next start
        if (live) { LOG("ue3 settings: render resolution %s is for the next start of the game (its targets are made when it starts)", v ? v : "game"); return; }
        // The size the game renders at (its window, viewport and render targets; 1280x720 as shipped): read by the engine when it starts, so a change takes a restart
        unsigned w = 0, h = 0; if (v && sscanf(v, "%ux%u", &w, &h) == 2 && w >= 640 && w <= 3840 && h >= 360 && h <= 2160) { char a[16], b[16]; snprintf(a, sizeof a, "%u", w); snprintf(b, sizeof b, "%u", h); dm_ue3_set("ResX", a); dm_ue3_set("ResY", b); }
        else { dm_ue3_set("ResX", ""); dm_ue3_set("ResY", ""); }
    } else if (!strcmp(env, "DM_UE3_KICK")) {
        // The renderer's mid-frame kicks (RHIKickCommandBuffer), gated by the rewriter's stub on a setting no code reads (dm/games/01005EE0036EC000/patches.py): merge = no
        // kicks, so the frame's command lists are few and its render passes stay open; game = as shipped. Always held, since the config's own value of the setting is arbitrary
        if (ue3_kick_build()) dm_ue3_set("SpeedTreeLeaves", v && !strcmp(v, "merge") ? "1" : "0");
    } else if (!strcmp(env, "DM_UE3_SMOOTH")) { atomic_store(&g_smooth_on, v && !strcmp(v, "game") ? 0 : v && !strcmp(v, "off") ? 2 : 1);
    } else if (!strncmp(env, "DM_UE3S_", 8)) dm_ue3_set(env + 8, v && strcmp(v, "game") ? v : "");
    else return;
    if (live) dm_perf_mark(note);
}
// value NULL: whether env is one of these settings; otherwise applies it now. 1 when it is one
int dm_nx_setting_live(const char *env, const char *value) {
    if (!env || (strcmp(env, "DM_UE3_SCALE") && strcmp(env, "DM_UE3_RES") && strcmp(env, "DM_UE3_KICK") && strcmp(env, "DM_UE3_SMOOTH") && strcmp(env, "DM_UE3_DYNRES") && strcmp(env, "DM_UE3_DYNRES_MIN") && strncmp(env, "DM_UE3S_", 8))) return 0;
    if (!strncmp(env, "DM_UE3S_", 8) && !find_row(env + 8)) return 0;
    if (value) nx_apply(env, value, 1);
    return 1;
}
void dm_nx_kick_start(void) { if (ue3_kick_build() && !getenv("DM_UE3_KICK")) { pthread_once(&g_once, ue3_find); if (g_nrow) nx_apply("DM_UE3_KICK", DM_UE3_KICK_DEFAULT, 0); } }
void dm_nx_settings_start(void) {
    pthread_once(&g_once, ue3_find); if (!g_nrow) return;
    extern char **environ;
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, "DM_UE3", 6)) continue;
        const char *eq = strchr(*e, '='); if (!eq) continue;
        char k[80]; snprintf(k, sizeof k, "%.*s", (int)(eq - *e), *e);
        nx_apply(k, eq + 1, 0);
    }
}
