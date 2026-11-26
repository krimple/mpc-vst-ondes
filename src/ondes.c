/* Ondes: an Ondes Martenot-style instrument (Model 7 signal chain) for the mpc-vst-plugins wrapper.
 * M1: tone generator, drawer switches, Note/Souffle, intensity key, F / Grave / Aigu, Puissance, Principal speaker.
 * M2: ring (Bague) with latch, keyboard bend and glide, hand vibrato, transposition buttons.
 * Voices: Mono (re-attack every note), Legato (overlapping notes slide, one attack), Poly (OSC_VOICES voices).
 * Design: docs/DESIGN.md. Parameters come from the generated params.h (vst/params.json), in VST index order. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <glob.h>
#include <pthread.h>
#include "plugin_dir.h"
#include "engine.h"
#include "params.h"
#include "osc.h"

#define SR 44100.0f
#define MAXBLK 128
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { P_T_O, P_T_8, P_T_C, P_T_G, P_T_GP, P_T_N, P_T_S, P_T_T, P_OCT_LEVEL, P_G_LEVEL, P_SOUFFLE, P_FELT, P_GRAVE,
       P_AIGU, P_VOLUME, P_OCTAVE, P_TUNE, P_KEY_SOURCE, P_KEY_MODE, P_KEY_CURVE, P_KEY_DEAD, P_KEY_SMOOTH,
       P_ENV_ATT, P_ENV_REL, P_DRIVE,
       P_MODE, P_RING_SPAN, P_RING_SLEW, P_RING_LATCH, P_GLIDE, P_BEND_RANGE, P_VIB_SRC, P_VIB_RATE, P_VIB_DEPTH,
       P_VIB_HUMAN, P_TR_M14, P_TR_P14, P_TR_P12, P_TR_P1, P_TR_P3, P_TR_P5, P_STRIP_NOTES, P_VOICE_MODE, P_PRESET, P_COUNT };
_Static_assert(P_COUNT == NPARAMS, "enum out of step with params.json");
_Static_assert(P_COUNT <= 64, "report mask is 64 bits");

enum { KS_VELOCITY, KS_AFTERTOUCH, KS_MODWHEEL, KS_EXPRESSION, KS_BREATH };
enum { KM_GATED, KM_KEYONLY };
enum { MODE_CLAVIER, MODE_BAGUE };
enum { VS_MODWHEEL, VS_AFTERTOUCH, VS_OFF };
enum { VM_MONO, VM_LEGATO, VM_POLY };
#define DIP_S 0.003f             /* Mono re-attack and Poly voice steal: fade the old note out this fast first */
#define POLY_GAIN 0.5f           /* headroom for several voices into one amplifier */

/* ---- presets: MPC's PRESET field lists VST programs (the wrapper's vst.json "programs" on the "preset" param).
 * Programs 0-9 are built in; the rest are the user's saved presets, read from
 * the .xpl files in <drive>/MPC Documents/Plugin Presets/Instruments/<plugin folder>, read by a thread when the plugin is created
 * (MPC saves them there but lists only programs in the field: device, Key 61, 2026-11-26). Each built-in preset starts
 * from the defaults, then sets what it lists. */
#define NPROG 40
#define USER_MAX (NPROG - NBUILTIN)
#define USER_STATE_MAX 4096
static const struct { const char *name, *set; } BUILTIN[] = {
    {"Pure Sine", "felt=1\n"},
    {"Lyric", "t_8=1\nvib_depth=35\n"},
    {"Hollow Clarinet", "t_o=0\nt_c=1\ngrave=3\n"},
    {"Bowed Strings", "t_o=0\nt_g=1\nt_gp=1\nvib_depth=25\nenv_att_ms=60\n"},
    {"Nasal Bassoon", "t_o=0\nt_n=1\nt_gp=1\noctave=0\n"},
    {"Breath", "t_s=1\nsouffle_bal=40\n"},
    {"Tutti", "t_o=0\nt_t=1\nvolume=-9\n"},
    {"Ribbon Glissando", "mode=1\nring_span=1\nring_latch=1\nt_8=1\n"},
    {"Expressive Pedal", "key_source=3\nkey_mode=1\nt_8=1\n"},
    {"Poly Organ", "voice_mode=2\nt_8=1\nt_c=1\nenv_att_ms=5\nenv_rel_ms=200\n"},
};
#define NBUILTIN ((int)(sizeof BUILTIN / sizeof BUILTIN[0]))

/* Transposition buttons, Model 7 drawer: -1/4, +1/4, +1/2, +1 tone, +tierce, +quinte (semitones; all six: +14.5). */
static const float TRANSPOSE[6] = {-0.5f, 0.5f, 1.f, 2.f, 4.f, 7.f};
#define CHUNK 16                 /* pitch is recomputed every 16 samples (0.36 ms): no audible steps in vibrato or slides */
#define STRIP_TOP 36             /* the touch strip's Notes mode plays MIDI 24-36 on a Key 61 (device, 2026-11-26) */
#define LATCH_JUMP 1024
/* A drawn button toggles its Off/On param on the host side, and MPC delivers one tap as two presses 30-140 ms apart
 * (mpc-vst-plugins docs/NOTES.md 2026-10-30), so a tap flipped a timbre on and straight back off (device, Key 61,
 * 2026-11-26). An Off/On param that changes again within this window of its last accepted change is going straight
 * back: the engine keeps its value and reports the param (mpc_engine_changed), so the host shows the real state. */
#define TOGGLE_DEBOUNCE_S 0.25          /* a bend jump bigger than this straight to the centre is a finger leaving the touch strip */

/* Slider steps (1-5). Placeholders until the JB4 schematic is read (docs/DESIGN.md Q1). */
static const float OCT_GAIN[5] = {0.25f, 0.40f, 0.60f, 0.80f, 1.00f};
static const float G_CUT[5] = {300, 600, 1200, 2400, 4800};
#define FELT_HZ 1200.0f
#define SOUFFLE_GAIN 0.9f

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } bq_t;

static float bq_run(bq_t *f, float x) {
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}
/* RBJ cookbook biquads. type: 0 lowpass, 1 highpass, 2 peak, 3 low shelf, 4 high shelf */
static void bq_set(bq_t *f, int type, double hz, double q, double db) {
    double A = pow(10, db / 40), w = 2 * M_PI * hz / SR, c = cos(w), s = sin(w), al = s / (2 * q);
    double b0, b1, b2, a0, a1, a2, sa = 2 * sqrt(A) * al;
    switch (type) {
    case 0: b0 = (1 - c) / 2; b1 = 1 - c; b2 = b0; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; break;
    case 1: b0 = (1 + c) / 2; b1 = -(1 + c); b2 = b0; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; break;
    case 2: b0 = 1 + al * A; b1 = -2 * c; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * c; a2 = 1 - al / A; break;
    case 3: b0 = A * ((A + 1) - (A - 1) * c + sa); b1 = 2 * A * ((A - 1) - (A + 1) * c); b2 = A * ((A + 1) - (A - 1) * c - sa);
            a0 = (A + 1) + (A - 1) * c + sa; a1 = -2 * ((A - 1) + (A + 1) * c); a2 = (A + 1) + (A - 1) * c - sa; break;
    default: b0 = A * ((A + 1) + (A - 1) * c + sa); b1 = -2 * A * ((A - 1) + (A + 1) * c); b2 = A * ((A + 1) + (A - 1) * c - sa);
            a0 = (A + 1) - (A - 1) * c + sa; a1 = 2 * ((A - 1) - (A + 1) * c); a2 = (A + 1) - (A - 1) * c - sa; break;
    }
    f->b0 = (float)(b0 / a0); f->b1 = (float)(b1 / a0); f->b2 = (float)(b2 / a0); f->a1 = (float)(a1 / a0); f->a2 = (float)(a2 / a0);
}

typedef struct {
    float v[P_COUNT];            /* parameter values in display units (option lists as an index) */
    osc_t osc;
    /* notes: held keys, last-note priority */
    uint8_t held[16]; int nheld; int sustain;
    struct voice { int key; float note, note_cur, vel, env; int gate, sus; float dip; uint32_t age; } vo[OSC_VOICES];
    uint32_t age_ctr;
    float esum[MAXBLK];          /* summed voice envelopes per sample (gates the breath noise) */
    /* intensity key */
    float ctl[5];                /* last value of each key source, 0..1 */
    int ctl_seen[5];             /* has that controller sent anything yet */
    float key_sm;
    /* pitch */
    int bend, bend_prev;         /* 14-bit, 8192 = centre */
    float ring_tgt;              /* Bague: ring position, -1..1 of the span (latched) */
    float bend_cur;              /* semitones, slewed */
    double vib_phase;
    float hum_r, hum_d, hum_r_tgt, hum_d_tgt; int hum_count;
    float semis_dbg;
    /* tap debounce (Off/On params) */
    double flip_t[P_COUNT];
    uint64_t report;             /* params whose host value must be corrected, one bit each */
    /* user presets (filled by the scan thread, read under user_mu) */
    pthread_mutex_t user_mu; pthread_t scan_th; int scan_on;
    int nuser; char (*user_name)[24]; char (*user_state)[USER_STATE_MAX];
    /* noise */
    uint32_t rng; float pk[7];
    /* filters */
    float felt_z; float felt_a;
    bq_t grave, aigu, spk_hp, spk_pk, spk_lp;
    int tone_dirty;
    float buf[MAXBLK];
    char state[4096];
} ondes_t;

static int iv(const ondes_t *s, int p) { return (int)lroundf(s->v[p]); }

static void tone_update(ondes_t *s) {
    bq_set(&s->grave, 3, 100, 0.7071, s->v[P_GRAVE]);
    bq_set(&s->aigu, 4, 3000, 0.7071, s->v[P_AIGU]);
    s->tone_dirty = 0;
}

/* JUCE's base64 variant ("<size>." + 6-bit groups, least significant first), as MPC writes an .xpl <state>. */
static int juce_b64dec(const char *in, uint8_t *out, int cap) {
    static const char T[] = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *dot;
    long n = strtol(in, &dot, 10);
    if (*dot != '.' || n <= 0 || n > cap) return -1;
    uint32_t acc = 0; int bits = 0, bi = 0;
    for (const char *p = dot + 1; *p && *p != '<' && bi < n; p++) {
        const char *q = strchr(T, *p);
        if (!q) return -1;
        acc |= (uint32_t)(q - T) << bits; bits += 6;
        while (bits >= 8 && bi < n) { out[bi++] = (uint8_t)acc; acc >>= 8; bits -= 8; }
    }
    return bi == n ? (int)n : -1;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* An .xpl's Ondes state: <state> -> fxb chunk set ("CcnK", "FBCh", uid "OndM", 128 reserved bytes, chunk size at 0x9C,
 * chunk at 0xA0) -> our "key=value" text. 0 when it is not an Ondes preset. */
static int xpl_state(const char *path, char *out, int cap) {
    enum { TEXT_MAX = 32768, BIN_MAX = 24576 };   /* heap, not TLS: a dlopen'd library with a large TLS block can fail to load */
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char *text = malloc(TEXT_MAX);
    uint8_t *bin = malloc(BIN_MAX);
    int ok = 0;
    if (text && bin) {
        size_t n = fread(text, 1, TEXT_MAX - 1, f);
        text[n] = 0;
        char *st = strstr(text, "<state>");
        int len = st ? juce_b64dec(st + 7, bin, BIN_MAX) : -1;
        if (len >= 0xA0 && !memcmp(bin, "CcnK", 4) && !memcmp(bin + 8, "FBCh", 4) && be32(bin + 16) == PLUG_UID) {
            uint32_t clen = be32(bin + 0x9C);
            if (clen > 0 && 0xA0 + clen <= (uint32_t)len && clen < (uint32_t)cap) { memcpy(out, bin + 0xA0, clen); out[clen] = 0; ok = 1; }
        }
    }
    fclose(f); free(text); free(bin);
    return ok;
}
static int name_cmp(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

/* The scan thread: every drive's saved presets for this plugin folder, sorted by name, published under user_mu.
 * ONDES_USER_PRESETS=<dir> (tests) scans that folder instead. */
static void *scan_user_presets(void *arg) {
    ondes_t *s = arg;
    char dir[512], pat[3][700];
    int npat = 0;
    const char *env = getenv("ONDES_USER_PRESETS");
    if (env) snprintf(pat[npat++], sizeof pat[0], "%s/*.xpl", env);
    else if (mpc_plugin_dir(dir, sizeof dir)) {
        const char *folder = strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir;
        snprintf(pat[npat++], sizeof pat[0], "/media/*/MPC Documents/Plugin Presets/Instruments/%s/*.xpl", folder);
        snprintf(pat[npat++], sizeof pat[0], "/sdcard/MPC Documents/Plugin Presets/Instruments/%s/*.xpl", folder);
    }
    glob_t g; memset(&g, 0, sizeof g);
    int any = 0;
    for (int i = 0; i < npat; i++) if (glob(pat[i], any ? GLOB_APPEND : 0, NULL, &g) == 0) any = 1;
    if (!any) return NULL;
    qsort(g.gl_pathv, g.gl_pathc, sizeof *g.gl_pathv, name_cmp);   /* full paths: same folder name on every drive */
    char (*names)[24] = calloc(USER_MAX, 24);
    char (*states)[USER_STATE_MAX] = calloc(USER_MAX, USER_STATE_MAX);
    int n = 0;
    for (size_t i = 0; i < g.gl_pathc && n < USER_MAX && names && states; i++) {
        if (!xpl_state(g.gl_pathv[i], states[n], USER_STATE_MAX)) continue;
        const char *b = strrchr(g.gl_pathv[i], '/') + 1;
        int bl = (int)strlen(b) - 4;   /* drop ".xpl" */
        snprintf(names[n], 24, "%.*s", bl > 23 ? 23 : bl, b);
        n++;
    }
    globfree(&g);
    pthread_mutex_lock(&s->user_mu);
    s->user_name = names; s->user_state = states; s->nuser = n;
    pthread_mutex_unlock(&s->user_mu);
    return NULL;
}

static void *create(const char *dir) {
    (void)dir;
    ondes_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    for (int i = 0; i < P_COUNT; i++) {
        const param_t *p = &PARAMS[i];
        s->v[i] = p->nopts ? (float)lroundf(p->def * (p->nopts - 1)) : p->min + (p->max - p->min) * p->def;
    }
    osc_init(&s->osc, SR);
    s->rng = 0x9E3779B9u;
    for (int k = 0; k < OSC_VOICES; k++) { s->vo[k].key = -1; s->vo[k].note = s->vo[k].note_cur = 60; s->vo[k].vel = 1; }
    s->bend = s->bend_prev = 8192;
    s->felt_a = 1 - expf(-2 * (float)M_PI * FELT_HZ / SR);
    bq_set(&s->spk_hp, 1, 70, 0.7071, 0);
    bq_set(&s->spk_pk, 2, 1000, 0.7, 2);
    bq_set(&s->spk_lp, 0, 7000, 0.7071, 0);
    tone_update(s);
    pthread_mutex_init(&s->user_mu, NULL);
    s->scan_on = pthread_create(&s->scan_th, NULL, scan_user_presets, s) == 0;
    return s;
}
static void destroy(void *i) {
    ondes_t *s = i;
    if (s->scan_on) pthread_join(s->scan_th, NULL);
    pthread_mutex_destroy(&s->user_mu);
    free(s->user_name); free(s->user_state);
    free(s);
}

static int find_param(const char *k) {
    for (int i = 0; i < P_COUNT; i++) if (!strcmp(PARAMS[i].key, k)) return i;
    return -1;
}

static void set_one(ondes_t *s, int i, const char *val) {
    const param_t *p = &PARAMS[i];
    float x;
    if (p->nopts) {
        int idx = -1;
        for (int k = 0; k < p->nopts; k++) if (!strcmp(val, p->opts[k])) idx = k;
        if (idx < 0) idx = atoi(val);
        if (idx < 0) idx = 0;
        if (idx >= p->nopts) idx = p->nopts - 1;
        x = (float)idx;
    } else {
        x = (float)atof(val);
        if (x < p->min) x = p->min;
        if (x > p->max) x = p->max;
    }
    if (i == P_VOICE_MODE && x != s->v[i]) {   /* new allocation rules: release everything, nothing stays stuck */
        s->nheld = 0;
        for (int k = 0; k < OSC_VOICES; k++) { s->vo[k].gate = s->vo[k].sus = 0; s->vo[k].key = -1; }
    }
    s->v[i] = x;
    if (i == P_GRAVE || i == P_AIGU) s->tone_dirty = 1;
}

static void set_state(ondes_t *s, const char *st) {
    /* "key=value" lines; unknown keys are ignored, missing ones keep their value */
    char line[128];
    while (*st) {
        int n = 0;
        while (st[n] && st[n] != '\n') n++;
        if (n > 0 && n < (int)sizeof line) {
            memcpy(line, st, n); line[n] = 0;
            char *eq = strchr(line, '=');
            if (eq) { *eq = 0; int i = find_param(line); if (i >= 0) set_one(s, i, eq + 1); }   /* "preset" only stores its index */
        }
        st += n; if (*st) st++;
    }
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static int is_switch(int i) { return PARAMS[i].nopts == 2 && !strcmp(PARAMS[i].opts[0], "Off"); }

/* Load program idx: back to the defaults, then the built-in preset's settings or the saved preset's whole state. */
static void load_program(ondes_t *s, int idx) {
    if (idx < 0 || idx >= NPROG) return;
    if (idx >= NBUILTIN) {   /* an empty slot becomes current (the wrapper's contract) but leaves the sound alone */
        pthread_mutex_lock(&s->user_mu);
        const int ok = idx - NBUILTIN < s->nuser;
        pthread_mutex_unlock(&s->user_mu);
        if (!ok) { s->v[P_PRESET] = (float)idx; return; }
    }
    for (int i = 0; i < P_COUNT; i++) {
        if (i == P_PRESET) continue;
        char b[32];
        const param_t *p = &PARAMS[i];
        if (p->nopts) snprintf(b, sizeof b, "%d", (int)lroundf(p->def * (p->nopts - 1)));
        else snprintf(b, sizeof b, "%g", p->min + (p->max - p->min) * p->def);
        set_one(s, i, b);
    }
    if (idx < NBUILTIN) set_state(s, BUILTIN[idx].set);
    else {
        pthread_mutex_lock(&s->user_mu);
        set_state(s, s->user_state[idx - NBUILTIN]);
        pthread_mutex_unlock(&s->user_mu);
    }
    s->v[P_PRESET] = (float)idx;
    s->tone_dirty = 1;
}

static void set_param(void *inst, const char *key, const char *val) {
    ondes_t *s = inst;
    if (!strcmp(key, "state")) { set_state(s, val); return; }
    int i = find_param(key);
    if (i < 0) return;
    if (i == P_PRESET) { load_program(s, atoi(val)); return; }
    if (is_switch(i)) {
        const float before = s->v[i];
        const double t = now_s();
        set_one(s, i, val);
        if (s->v[i] != before) {
            if (t - s->flip_t[i] < TOGGLE_DEBOUNCE_S) { s->v[i] = before; s->report |= 1ull << i; }   /* the tap's second press */
            else s->flip_t[i] = t;
        }
        return;
    }
    set_one(s, i, val);
}

/* The wrapper asks once per block for params the engine changed by itself; here, switches whose revert it ignored. */
int mpc_engine_changed(void *inst, char *key, int len) {
    ondes_t *s = inst;
    if (!s->report) return 0;
    int i = __builtin_ctzll(s->report);
    s->report &= s->report - 1;
    snprintf(key, len, "%s", PARAMS[i].key);
    return 1;
}

static int get_param(void *inst, const char *key, char *buf, int n) {
    ondes_t *s = inst;
    if (!strcmp(key, "state")) {
        int len = 0;
        for (int i = 0; i < P_COUNT && len < n; i++)
            len += snprintf(buf + len, n - len, "%s=%.6g\n", PARAMS[i].key, s->v[i]);
        return len < n ? len : n - 1;
    }
    /* tests only: voice 0's pitch, voice k's pitch, voices sounding, voice 0's envelope */
    if (!strcmp(key, "dbg_semis")) return snprintf(buf, n, "%.5f", s->semis_dbg);
    if (!strncmp(key, "dbg_semis", 9) && key[9] >= '0' && key[9] < '0' + OSC_VOICES)
        return snprintf(buf, n, "%.5f", s->vo[key[9] - '0'].env > 0 ? s->vo[key[9] - '0'].note_cur - 69 : -999.f);
    if (!strcmp(key, "dbg_active")) { int c = 0; for (int k = 0; k < OSC_VOICES; k++) c += s->vo[k].gate; return snprintf(buf, n, "%d", c); }
    if (!strcmp(key, "dbg_env0")) return snprintf(buf, n, "%.5f", s->vo[0].env);
    if (!strncmp(key, "preset:", 7)) {   /* program names for MPC's PRESET field */
        const int idx = atoi(key + 7);
        if (idx >= 0 && idx < NBUILTIN) return snprintf(buf, n, "%s", BUILTIN[idx].name);
        int r = 0;
        pthread_mutex_lock(&s->user_mu);
        if (idx >= NBUILTIN && idx - NBUILTIN < s->nuser) r = snprintf(buf, n, "%s", s->user_name[idx - NBUILTIN]);
        pthread_mutex_unlock(&s->user_mu);
        return r ? r : snprintf(buf, n, "-");
    }
    int i = find_param(key);
    if (i < 0) return 0;
    if (PARAMS[i].nopts) return snprintf(buf, n, "%d", iv(s, i));
    return snprintf(buf, n, "%g", s->v[i]);
}

/* ---- notes ---- */
/* Strip Notes Up: notes at STRIP_TOP and below sound 1-3 octaves higher (the strip's Notes mode sits in the lowest octave).
 * Held keys are kept as received, so a note-off always finds its note-on. */
static float sounding(const ondes_t *s, int k) {
    const int up = iv(s, P_STRIP_NOTES);
    return (float)(k <= STRIP_TOP ? k + 12 * up : k);
}
static void note_on(ondes_t *s, int k, int vel) {
    if (iv(s, P_MODE) == MODE_BAGUE) { s->ring_tgt = 0; s->bend_cur = 0; }   /* a key puts the ring on that note */
    const int vm = iv(s, P_VOICE_MODE);
    struct voice *v;
    if (vm == VM_POLY) {
        int pick = -1;
        for (int i = 0; i < OSC_VOICES && pick < 0; i++) if (s->vo[i].key == k && s->vo[i].gate) pick = i;   /* same key again */
        for (int i = 0; i < OSC_VOICES && pick < 0; i++) if (!s->vo[i].gate && s->vo[i].env == 0) pick = i;   /* free */
        if (pick < 0)   /* else the oldest released voice, else the oldest held one */
            for (int pass = 0; pass < 2 && pick < 0; pass++)
                for (int i = 0; i < OSC_VOICES; i++)
                    if (s->vo[i].gate == pass && (pick < 0 || s->vo[i].age < s->vo[pick].age)) pick = i;
        v = &s->vo[pick];
        if (v->env > 1e-4f) v->dip = DIP_S;
        v->note = v->note_cur = sounding(s, k);   /* no glide in Poly */
    } else {
        const int overlap = s->nheld > 0;
        for (int i = 0; i < s->nheld; i++) if (s->held[i] == k) { memmove(s->held + i, s->held + i + 1, s->nheld - i - 1); s->nheld--; break; }
        if (s->nheld == 16) { memmove(s->held, s->held + 1, 15); s->nheld--; }
        s->held[s->nheld++] = (uint8_t)k;
        v = &s->vo[0];
        const int sounding_now = v->gate || v->env > 1e-4f;
        v->note = sounding(s, k);
        if (!sounding_now) v->note_cur = v->note;                    /* from silence: start on the note */
        else if (vm == VM_MONO) v->dip = DIP_S;                      /* Mono: re-attack (glide still applies) */
        else if (!overlap) v->note_cur = v->note;                    /* Legato, detached note: jump, no slide */
    }
    v->key = k; v->vel = vel / 127.f; v->gate = 1; v->sus = 0; v->age = ++s->age_ctr;
}
static void note_off(ondes_t *s, int k) {
    if (iv(s, P_VOICE_MODE) == VM_POLY) {
        for (int i = 0; i < OSC_VOICES; i++)
            if (s->vo[i].key == k && s->vo[i].gate && !s->vo[i].sus) { if (s->sustain) s->vo[i].sus = 1; else s->vo[i].gate = 0; }
        return;
    }
    for (int i = 0; i < s->nheld; i++) if (s->held[i] == k) { memmove(s->held + i, s->held + i + 1, s->nheld - i - 1); s->nheld--; break; }
    if (s->nheld) s->vo[0].note = sounding(s, s->held[s->nheld - 1]);   /* back to the key still held, sliding */
    else if (!s->sustain) s->vo[0].gate = 0;
}
static void all_off(ondes_t *s, int hard) {
    s->nheld = 0; s->sustain = 0;
    for (int i = 0; i < OSC_VOICES; i++) { s->vo[i].gate = s->vo[i].sus = 0; if (hard) s->vo[i].env = 0; }
}
static void set_ctl(ondes_t *s, int src, int val) { s->ctl[src] = val / 127.f; s->ctl_seen[src] = 1; }

static void midi(void *inst, const uint8_t *m, int len) {
    ondes_t *s = inst;
    if (len < 1) return;
    const int st = m[0] & 0xF0, d1 = len > 1 ? m[1] & 0x7F : 0, d2 = len > 2 ? m[2] & 0x7F : 0;
    switch (st) {
    case 0x90: if (d2) note_on(s, d1, d2); else note_off(s, d1); break;
    case 0x80: note_off(s, d1); break;
    case 0xD0: set_ctl(s, KS_AFTERTOUCH, d1); break;
    case 0xB0:
        if (d1 == 1) set_ctl(s, KS_MODWHEEL, d2);
        else if (d1 == 2) set_ctl(s, KS_BREATH, d2);
        else if (d1 == 11) set_ctl(s, KS_EXPRESSION, d2);
        else if (d1 == 64) {
            s->sustain = d2 >= 64;
            if (!s->sustain) {
                if (iv(s, P_VOICE_MODE) == VM_POLY) { for (int i = 0; i < OSC_VOICES; i++) if (s->vo[i].sus) s->vo[i].sus = s->vo[i].gate = 0; }
                else if (!s->nheld) s->vo[0].gate = 0;
            }
        }
        else if (d1 == 120 || d1 == 123) all_off(s, d1 == 120);
        break;
    case 0xE0: {
        const int v = d1 | (d2 << 7);
        /* Latch (Bague): the touch strip snaps back to the centre when the finger lifts (device, Key 61, 2026-11-26);
         * the real ring stays where it is left, so a big jump straight to the centre holds the pitch. A wheel coming
         * back passes through the values in between and is followed. */
        const int to_centre = v >= 8191 && v <= 8193, jump = abs(v - s->bend_prev) > LATCH_JUMP;
        s->bend_prev = v;
        s->bend = v;
        if (iv(s, P_MODE) == MODE_BAGUE && !(iv(s, P_RING_LATCH) && to_centre && jump))
            s->ring_tgt = v >= 8192 ? (v - 8192) / 8191.f : (v - 8192) / 8192.f;
        break;
    }
    default: break;
    }
}

/* ---- render ---- */
static float pink(ondes_t *s) {
    /* Paul Kellet's refined pink filter on xorshift white noise */
    s->rng ^= s->rng << 13; s->rng ^= s->rng >> 17; s->rng ^= s->rng << 5;
    const float w = (float)(int32_t)s->rng * (1.0f / 2147483648.0f);
    float *b = s->pk;
    b[0] = 0.99886f * b[0] + w * 0.0555179f; b[1] = 0.99332f * b[1] + w * 0.0750759f;
    b[2] = 0.96900f * b[2] + w * 0.1538520f; b[3] = 0.86650f * b[3] + w * 0.3104856f;
    b[4] = 0.55000f * b[4] + w * 0.5329522f; b[5] = -0.7616f * b[5] - w * 0.0168980f;
    const float o = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
    b[6] = w * 0.115926f;
    return o * 0.11f;
}

static float urand(ondes_t *s) {   /* -1..1 */
    s->rng ^= s->rng << 13; s->rng ^= s->rng >> 17; s->rng ^= s->rng << 5;
    return (float)(int32_t)s->rng * (1.0f / 2147483648.0f);
}
static float slew_k(float ms, float dt) { return ms > 0 ? 1 - expf(-dt * 1000.f / ms) : 1; }

/* The pitch offset shared by every voice (bend or ring, vibrato, transposers, octave, tune), in semitones, minus 69 so a
 * voice's note plus this is semitones from A4; advances ring slew and vibrato by dt. */
static float pitch_shared(ondes_t *s, float dt) {
    float bt;
    if (iv(s, P_MODE) == MODE_BAGUE) bt = s->ring_tgt * 12.f * (iv(s, P_RING_SPAN) + 1);
    else bt = (s->bend >= 8192 ? (s->bend - 8192) / 8191.f : (s->bend - 8192) / 8192.f) * s->v[P_BEND_RANGE];
    s->bend_cur += slew_k(s->v[P_RING_SLEW], dt) * (bt - s->bend_cur);
    /* hand vibrato: depth from the chosen controller; "human" drifts rate and depth toward new random targets */
    const float h = s->v[P_VIB_HUMAN] / 100.f;
    if (--s->hum_count <= 0) { s->hum_r_tgt = urand(s) * 0.15f * h; s->hum_d_tgt = urand(s) * 0.25f * h; s->hum_count = (int)(0.3f / dt); }
    const float hk = slew_k(300, dt);
    s->hum_r += hk * (s->hum_r_tgt - s->hum_r); s->hum_d += hk * (s->hum_d_tgt - s->hum_d);
    const int vs = iv(s, P_VIB_SRC);
    const float amt = vs == VS_MODWHEEL ? s->ctl[KS_MODWHEEL] : vs == VS_AFTERTOUCH ? s->ctl[KS_AFTERTOUCH] : 0;
    s->vib_phase += s->v[P_VIB_RATE] * (1 + s->hum_r) * dt;
    if (s->vib_phase >= 1) s->vib_phase -= 1;
    const float vib = amt * s->v[P_VIB_DEPTH] / 100.f * (1 + s->hum_d) * sinf(2 * (float)M_PI * (float)s->vib_phase);
    float tr = 0;
    for (int k = 0; k < 6; k++) if (iv(s, P_TR_M14 + k)) tr += TRANSPOSE[k];
    return -69 + s->bend_cur + vib + tr + (iv(s, P_OCTAVE) - 1) * 12 + s->v[P_TUNE] / 100.f;
}

static float key_target(ondes_t *s, int src, int mode) {
    if (src == KS_VELOCITY) return 1;
    if (!s->ctl_seen[src]) return mode == KM_KEYONLY ? 0 : 1;   /* untouched: silent when the key is the only gate */
    const float dead = s->v[P_KEY_DEAD] / 100.f;
    float x = (s->ctl[src] - dead) / (1 - dead);
    if (x <= 0) return 0;
    return powf(x, s->v[P_KEY_CURVE]);
}

static void render(void *inst, int16_t *out, int frames) {
    ondes_t *s = inst;
    if (frames > MAXBLK) frames = MAXBLK;
    if (s->tone_dirty) tone_update(s);
    osc_t *o = &s->osc;
    const int tutti = iv(s, P_T_T);
    o->on[T_O] = tutti || iv(s, P_T_O); o->on[T_8] = tutti || iv(s, P_T_8); o->on[T_C] = tutti || iv(s, P_T_C);
    o->on[T_G] = tutti || iv(s, P_T_G); o->on[T_N] = tutti || iv(s, P_T_N); o->on[T_GP] = iv(s, P_T_GP);
    const int felt = iv(s, P_FELT), noise_on = iv(s, P_T_S);
    o->oct_gain = OCT_GAIN[iv(s, P_OCT_LEVEL) - 1];
    o->g_cut = G_CUT[iv(s, P_G_LEVEL) - 1];
    o->impurity = felt ? 0 : 0.01f;
    const int src = iv(s, P_KEY_SOURCE), vm = iv(s, P_VOICE_MODE);
    int mode = iv(s, P_KEY_MODE);
    if (src == KS_VELOCITY) mode = KM_GATED;   /* nothing to play the key with */
    const int key_drone = mode == KM_KEYONLY && vm != VM_POLY;   /* mono Key Only: the key alone shapes the sound */
    const float vgain = vm == VM_POLY ? POLY_GAIN : 1;
    for (int i = 0; i < frames; i += CHUNK) {
        const int n = frames - i < CHUNK ? frames - i : CHUNK;
        const float dt = n / SR;
        const float shared = pitch_shared(s, dt);
        const float ea = slew_k(s->v[P_ENV_ATT], dt), er = slew_k(s->v[P_ENV_REL], dt), ed = slew_k(DIP_S * 1000.f / 3, dt);
        const float kg = vm == VM_MONO || vm == VM_LEGATO ? slew_k(s->v[P_GLIDE], dt) : 1;
        float e0 = 0, e1 = 0;
        for (int k = 0; k < OSC_VOICES; k++) {
            struct voice *v = &s->vo[k];
            osc_voice_t *ov = &o->v[k];
            v->note_cur += kg * (v->note - v->note_cur);
            const float before = v->env;
            if (key_drone && k == 0) v->env = 1;
            else if (key_drone) v->env = 0;
            else if (v->dip > 0) { v->env += ed * (0 - v->env); v->dip -= dt; }
            else {
                const float t = v->gate ? (src == KS_VELOCITY ? v->vel * v->vel : 1) : 0;   /* velocity only when it is the source */
                v->env += (t > v->env ? ea : er) * (t - v->env);
                if (t == 0 && v->env < 1e-6f) v->env = 0;
            }
            ov->amp0 = before * vgain; ov->amp1 = v->env * vgain;
            ov->freq = 440.f * powf(2.f, (v->note_cur + shared) / 12.f);
            e0 += before; e1 += v->env;
        }
        s->semis_dbg = s->vo[0].note_cur + shared;
        if (e0 > 1) e0 = 1;
        if (e1 > 1) e1 = 1;
        for (int j = 0; j < n; j++) s->esum[i + j] = e0 + (e1 - e0) * (j + 1) / n;
        osc_render(o, s->buf + i, n);
    }

    const float bal = s->v[P_SOUFFLE] / 100.f;
    const float ktgt = key_target(s, src, mode);
    const float ksm = s->v[P_KEY_SMOOTH] > 0 ? 1 - expf(-1000.f / (s->v[P_KEY_SMOOTH] * SR)) : 1;
    const float vol = powf(10.f, s->v[P_VOLUME] / 20.f);
    const float drive = s->v[P_DRIVE] / 100.f, dgain = 1 + 6 * drive;

    for (int i = 0; i < frames; i++) {
        float x = s->buf[i];   /* the voices, each already at its envelope */
        if (noise_on) x = x * (1 - bal) + SOUFFLE_GAIN * bal * pink(s) * s->esum[i];
        s->key_sm += ksm * (ktgt - s->key_sm);
        x *= s->key_sm;
        if (felt) { s->felt_z += s->felt_a * (x - s->felt_z); x = s->felt_z; }
        x = bq_run(&s->aigu, bq_run(&s->grave, x));
        x *= vol;
        /* D1 Principal: amplifier (asymmetric soft clip) into a speaker (HP 70 Hz, +2 dB at 1 kHz, LP 7 kHz) */
        if (drive > 0) {   /* Drive 0 is a clean amplifier */
            const float d = x * dgain;
            x += drive * (tanhf(d + 0.3f * d * d) / dgain * 1.5f - x);
        }
        x = bq_run(&s->spk_lp, bq_run(&s->spk_pk, bq_run(&s->spk_hp, x)));
        float y = x * 32767.f;
        if (y > 32767.f) y = 32767.f; else if (y < -32768.f) y = -32768.f;
        out[2 * i] = out[2 * i + 1] = (int16_t)lrintf(y);
    }
}

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, NULL, NULL };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
