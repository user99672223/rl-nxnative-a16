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
    if ((type != UE3_INT && type != UE3_FLOAT && type != UE3_BOOL) || zero) return 0;
    if (var < (uint64_t)m->base || var + 4 > (uint64_t)m->base + m->size || (var & 3)) return 0;
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
        if (memcmp(b + o, key, sizeof key) || memcmp(b + o - 4, "\0\0\0\0", 4)) continue;   // the whole text, not the tail of UpscaleScreenPercentage
        uint64_t want = (uint64_t)(b + o);
        for (uint64_t s = 8; s + 48 <= n; s += 8) {   // the row whose name pointer is that text
            uint64_t v; memcpy(&v, b + s, 8); if (v != want) continue;
            ue3_row r; if (!row_at(m, b + s - 8, &r)) continue;
            uint64_t first = s - 8; while (first >= 48 && row_at(m, b + first - 48, &r)) first -= 48;
            int k = 0; for (uint64_t e = first; e + 48 <= n && k < UE3_MAX && row_at(m, b + e, &g_row[k]); e += 48) k++;
            if (k < 8) continue;   // a lone look-alike
            g_nrow = k;
            LOG("ue3 settings: %d system settings described at main+%#llx (the variables at main+%#llx..)", k, (unsigned long long)first, (unsigned long long)(g_row[0].var - b));
            return;
        }
    }
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
void dm_ue3set_tick(void) {
    static int inited; static uint64_t frame; static int dump = -1;
    frame++;
    if (!inited) {
        inited = 1;
        const char *e = getenv("DM_UE3_SET");
        if (e && *e) {
            char *s = strdup(e), *save = NULL;
            for (char *t = strtok_r(s, ",", &save); t; t = strtok_r(NULL, ",", &save)) { char *eq = strchr(t, '='); if (!eq) { LOG("[RED] DM_UE3_SET: %s has no value", t); continue; } *eq = 0; dm_ue3_set(t, eq + 1); }
            free(s);
        }
        const char *d = getenv("DM_UE3_DUMP"); dump = d && atoi(d) > 0;
        { void dm_nx_settings_start(void); extern char **environ; for (char **q = environ; *q; q++) if (!strncmp(*q, "NX_RENDER_SCALE=", 16) || !strncmp(*q, "NX_DYNRES", 9) || !strncmp(*q, "NX_UE3_", 7)) { dm_nx_settings_start(); break; } }
    }
    if (dump && (frame == 1 || frame == 600)) dm_ue3_dump(frame == 1 ? "first frame" : "frame 600");
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
//   NX_RENDER_SCALE=0.5..1.0     the 3D picture's size: below 1.0 ScreenPercentage is held at that percentage and the game's dynamic resolution is off (it would move it);
//                                1.0 leaves both to the game
//   NX_DYNRES=game|off           off holds AllowDynamicResolution at 0: the game renders its full size for the console mode
//   NX_DYNRES_MIN=0.5..1.0       with NX_DYNRES=game: the lowest scale its dynamic resolution may go to (MinimumScreenScale), 0 = the game's own
//   NX_UE3_<Setting>=game|<value>   any row of the table by name (NX_UE3_MotionBlur=0, NX_UE3_MaxShadowResolution=512): game leaves it alone
// Read when the game starts (every present thread's first tick) and applied again by dm_nx_setting_live when the panel or the in-game menu changes one.
void dm_perf_mark(const char *what);
__attribute__((weak)) void dm_perf_mark(const char *what) { (void)what; }
static void nx_apply(const char *env, const char *v, int live) {
    char note[160]; snprintf(note, sizeof note, "%s=%s", env, v ? v : "");
    if (!strcmp(env, "NX_RENDER_SCALE")) {
        float s = v ? strtof(v, NULL) : 1.0f;
        if (!(s >= 0.25f && s < 0.9999f)) { dm_ue3_set("ScreenPercentage", ""); if (!getenv("NX_DYNRES") || strcmp(getenv("NX_DYNRES"), "off")) dm_ue3_set("AllowDynamicResolution", ""); }
        else { char p[16]; snprintf(p, sizeof p, "%g", s * 100.0f); dm_ue3_set("ScreenPercentage", p); dm_ue3_set("AllowDynamicResolution", "0"); }
    } else if (!strcmp(env, "NX_DYNRES")) {
        const char *rs = getenv("NX_RENDER_SCALE"); int scaled = rs && strtof(rs, NULL) < 0.9999f && strtof(rs, NULL) >= 0.25f;
        dm_ue3_set("AllowDynamicResolution", v && !strcmp(v, "off") ? "0" : scaled ? "0" : "");
    } else if (!strcmp(env, "NX_DYNRES_MIN")) {
        float s = v ? strtof(v, NULL) : 0; if (s >= 0.25f && s <= 1.0f) { char p[16]; snprintf(p, sizeof p, "%g", s); dm_ue3_set("MinimumScreenScale", p); } else dm_ue3_set("MinimumScreenScale", "");
    } else if (!strncmp(env, "NX_UE3_", 7)) dm_ue3_set(env + 7, v && strcmp(v, "game") ? v : "");
    else return;
    if (live) dm_perf_mark(note);
}
// value NULL: whether env is one of these settings; otherwise applies it now. 1 when it is one
int dm_nx_setting_live(const char *env, const char *value) {
    if (!env || (strcmp(env, "NX_RENDER_SCALE") && strcmp(env, "NX_DYNRES") && strcmp(env, "NX_DYNRES_MIN") && strncmp(env, "NX_UE3_", 7))) return 0;
    if (!strncmp(env, "NX_UE3_", 7) && !find_row(env + 7)) return 0;
    if (value) nx_apply(env, value, 1);
    return 1;
}
void dm_nx_settings_start(void) {
    pthread_once(&g_once, ue3_find); if (!g_nrow) return;
    extern char **environ;
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, "NX_", 3)) continue;
        const char *eq = strchr(*e, '='); if (!eq) continue;
        char k[80]; snprintf(k, sizeof k, "%.*s", (int)(eq - *e), *e);
        nx_apply(k, eq + 1, 0);
    }
}
