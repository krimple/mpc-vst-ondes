#include "osc.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float g_rate_os;   /* oversampled rate; the same for every instance */

/* Sine table, linear interpolation: error about 3e-7 (-130 dB), far below the -60 dB the tests ask of O with F. */
#define SIN_N 4096
static float sin_tab[SIN_N + 1];
static inline float sin2pi(double p) {
    const double x = p * SIN_N;
    const int i = (int)x;
    const float f = (float)(x - i);
    return sin_tab[i] + f * (sin_tab[i + 1] - sin_tab[i]);
}

/* Per-timbre output gains, set by ear against Loriod's chart (calibration: docs/DESIGN.md Q1). */
#define GAIN_O   0.80f
#define GAIN_8   1.80f    /* |sin| - 2/pi peaks at 0.36 */
#define GAIN_C   0.75f
#define GAIN_G   0.40f
#define GAIN_GP  0.55f
#define GAIN_N   0.35f    /* target RMS of the DC-free pulse */
#define N_MAXG   1.20f    /* cap on N's RMS gain: a very narrow pulse stays thin, as a fixed-width pulse does */

/* 2-point polyBLEP residual for a unit step, t = phase since the edge (0..1), dt = phase increment. */
static inline float blep(double t, double dt) {
    if (t < dt) { double x = t / dt; return (float)(-(1 - x) * (1 - x)); }
    if (t > 1 - dt) { double x = (t - 1) / dt; return (float)((1 + x) * (1 + x)); }
    return 0;
}
/* Its integral: the residual for a unit change of slope per sample. */
static inline float blamp(double t, double dt) {
    if (t < dt) { double x = 1 - t / dt; return (float)(x * x * x / 3); }
    if (t > 1 - dt) { double x = 1 + (t - 1) / dt; return (float)(x * x * x / 3); }
    return 0;
}
static inline double since(double p, double edge) { double r = p - edge; return r < 0 ? r + 1 : r; }

static void g_coefs(osc_t *o) {
    double w = 2 * M_PI * o->g_cut / g_rate_os, c = cos(w), al = sin(w) / (2 * 0.7071), a0 = 1 + al;
    o->g_b0 = (float)((1 - c) / 2 / a0); o->g_b1 = (float)((1 - c) / a0); o->g_b2 = o->g_b0;
    o->g_a1 = (float)(-2 * c / a0); o->g_a2 = (float)((1 - al) / a0);
    o->g_cut_set = o->g_cut;
}

void osc_init(osc_t *o, float out_rate) {
    memset(o, 0, sizeof *o);
    g_rate_os = out_rate * OSC_OS;
    for (int i = 0; i <= SIN_N; i++) sin_tab[i] = (float)sin(2 * M_PI * i / SIN_N);   /* same values every time: harmless if instances race */
    for (int k = 0; k < OSC_VOICES; k++) o->v[k].freq = 261.63f;
    o->oct_gain = 0.6f; o->g_cut = 1200; o->impurity = 0.01f; o->creux_clip = 0.9f;   /* h_n ~ sin(n*pi*k/2)/n^2: below ~0.85 the odd harmonics stop falling steadily (0.8 nulls h5) */
    o->nas_width_s = 150e-6f;
    /* Blackman-windowed sinc, cutoff 20.5 kHz: stopband from about 26 kHz at 4x, so nothing aliases below 18 kHz. */
    const double fc = 20500.0 / g_rate_os, mid = (OSC_FIR_TAPS - 1) / 2.0;
    double sum = 0;
    for (int i = 0; i < OSC_FIR_TAPS; i++) {
        double x = i - mid, s = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
        double w = 0.42 - 0.5 * cos(2 * M_PI * i / (OSC_FIR_TAPS - 1)) + 0.08 * cos(4 * M_PI * i / (OSC_FIR_TAPS - 1));
        o->fir[i] = (float)(s * w); sum += s * w;
    }
    for (int i = 0; i < OSC_FIR_TAPS; i++) o->fir[i] = (float)(o->fir[i] / sum);
    g_coefs(o);
}

/* One oversampled sample of one voice's note mix. */
/* Double precision on purpose: a single-precision version measured slower on the Key 61's Cortex-A17 (worst case 41% of a
 * block against 31%, device, 2026-11-26). */
static inline float tick(osc_t *o, osc_voice_t *vc, double p, double dt) {
    float out = 0;
    const float s = sin2pi(p);
    if (o->on[T_O]) out += GAIN_O * (s + o->impurity * (3 * s - 4 * s * s * s));
    if (o->on[T_8]) {
        /* full-wave rectified sine ("doubleur"): even harmonics only; cusps (slope +4pi per cycle) at 0 and 0.5 */
        float v = fabsf(s) - (float)(2 / M_PI);
        const float d = (float)(4 * M_PI * dt / 2);
        v += d * (blamp(p, dt) + blamp(since(p, 0.5), dt));
        out += GAIN_8 * o->oct_gain * v;
    }
    if (o->on[T_C]) {
        /* triangle (peak at 0.25) clipped at +-k ("écrêtage"): four corners of slope +-4 per cycle */
        const double k = o->creux_clip;
        double tr = p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
        if (tr > k) tr = k; else if (tr < -k) tr = -k;
        const float d = (float)(4 * dt / 2);
        float v = (float)tr + d * (-blamp(since(p, k / 4), dt) - blamp(since(p, (2 - k) / 4), dt)
                                   + blamp(since(p, (2 + k) / 4), dt) + blamp(since(p, 1 - k / 4), dt));
        out += GAIN_C * v / (float)k;
    }
    float sq = 0;
    if (o->on[T_G] || o->on[T_GP]) sq = (p < 0.5 ? 1.f : -1.f) + blep(p, dt) - blep(since(p, 0.5), dt);
    if (o->on[T_G]) out += GAIN_G * sq;
    if (o->on[T_GP]) {
        float y = o->g_b0 * sq + vc->g_z1;
        vc->g_z1 = o->g_b1 * sq - o->g_a1 * y + vc->g_z2;
        vc->g_z2 = o->g_b2 * sq - o->g_a2 * y;
        out += GAIN_GP * y;
    }
    if (o->on[T_N]) {
        /* fixed-duration pulse ("impulsions"): the duty grows with pitch, capped at 40% */
        double w = o->nas_width_s * vc->freq;
        if (w > 0.4) w = 0.4;
        if (w < dt) w = dt;
        float v = (p < w ? 1.f : 0.f) - (float)w + 0.5f * (blep(p, dt) - blep(since(p, w), dt));
        float g = GAIN_N / sqrtf((float)(w * (1 - w)));
        out += (g > N_MAXG ? N_MAXG : g) * v;
    }
    return out;
}

void osc_render(osc_t *o, float *out, int n) {
    if (o->on[T_GP] && o->g_cut != o->g_cut_set) g_coefs(o);
    double dt[OSC_VOICES];
    int act[OSC_VOICES], na = 0;
    for (int k = 0; k < OSC_VOICES; k++) {
        osc_voice_t *vc = &o->v[k];
        if (vc->amp0 == 0 && vc->amp1 == 0) continue;   /* silent: no phase advance either (a new note starts it fresh) */
        double f = vc->freq;
        if (f < 16) f = 16; else if (f > 8400) f = 8400;
        dt[k] = f / g_rate_os;
        act[na++] = k;
    }
    const float step = 1.f / (n * OSC_OS);
    int t = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < OSC_OS; j++, t++) {
            float v = 0;
            for (int a = 0; a < na; a++) {
                osc_voice_t *vc = &o->v[act[a]];
                const float g = vc->amp0 + (vc->amp1 - vc->amp0) * (t * step);
                v += g * tick(o, vc, vc->phase, dt[act[a]]);
                vc->phase += dt[act[a]];
                if (vc->phase >= 1) vc->phase -= 1;
            }
            o->hist[o->hpos] = o->hist[o->hpos + OSC_FIR_TAPS] = v;   /* doubled ring: a contiguous window */
            if (++o->hpos == OSC_FIR_TAPS) o->hpos = 0;
        }
        const float *h = o->hist + o->hpos;   /* oldest .. newest */
        float acc = 0;
        for (int k = 0; k < OSC_FIR_TAPS; k++) acc += o->fir[k] * h[k];
        out[i] = acc;
    }
}
