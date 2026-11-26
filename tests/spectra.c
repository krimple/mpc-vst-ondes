/* Offline checks of the timbres against Loriod's timbre chart (Fig. 2) and of the intensity key.
 * Drives the engine through its mpc_engine() API, as the wrapper does. Build and run: tests/run.sh */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"

#define SR 44100
#define N 32768                 /* FFT size, about 0.74 s */
#define SETTLE 8820             /* 0.2 s before the window */

static const mpc_engine_t *E;
static int fails;
/* Set a param as a preset or saved state does: not subject to the tap debounce, which merges a host's second flip of an
 * Off/On switch within 250 ms (src/ondes.c TOGGLE_DEBOUNCE_S) and would swallow these back-to-back test writes. */
static void setp(void *s, const char *k, const char *v) { char b[96]; snprintf(b, sizeof b, "%s=%s\n", k, v); E->set_param(s, "state", b); }

static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void check(int ok, const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    printf("%s ", ok ? "ok  " : "FAIL"); vprintf(fmt, a); printf("\n");
    va_end(a);
    if (!ok) fails++;
}

static void fft(double *re, double *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int b = n >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j ^= b;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2 * M_PI / len, wr = cos(a), wi = sin(a);
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                double ur = re[i + k], ui = im[i + k];
                double vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                double vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}

static double mag[N / 2];

/* Instance with every timbre off, a clean chain (drive 0, flat tone), velocity source at full velocity. */
static void *fresh(void) {
    void *s = E->create(NULL);
    const char *off[] = {"t_o", "t_8", "t_c", "t_g", "t_gp", "t_n", "t_s", "t_t"};
    for (int i = 0; i < 8; i++) setp(s, off[i], "0");
    setp(s, "drive", "0");
    return s;
}

/* Hold `note` and take the magnitude spectrum (dB, Blackman-Harris window) after it settles. */
static void spectrum(void *s, int note) {
    uint8_t on[3] = {0x90, (uint8_t)note, 127};
    E->midi(s, on, 3);
    static int16_t blk[256];
    static double re[N], im[N];
    for (int i = 0; i < SETTLE; i += 128) E->render(s, blk, 128);
    for (int i = 0; i < N; i += 128) {
        E->render(s, blk, 128);
        for (int k = 0; k < 128; k++) re[i + k] = blk[2 * k] / 32768.0;
    }
    for (int i = 0; i < N; i++) {
        double x = 2 * M_PI * i / (N - 1);
        re[i] *= 0.35875 - 0.48829 * cos(x) + 0.14128 * cos(2 * x) - 0.01168 * cos(3 * x);
        im[i] = 0;
    }
    fft(re, im, N);
    for (int i = 0; i < N / 2; i++) mag[i] = 10 * log10(re[i] * re[i] + im[i] * im[i] + 1e-30);
}
static double freq_of(int note) { return 440 * pow(2, (note - 69) / 12.0); }
/* Peak level (dB) within +-4 bins of a frequency. */
static double level(double hz) {
    int c = (int)lround(hz * N / SR), best = c;
    for (int b = c - 4; b <= c + 4; b++) if (b > 0 && b < N / 2 && mag[b] > mag[best]) best = b;
    return mag[best];
}
/* Strongest component below 18 kHz that is not within +-6 bins of a harmonic of f, relative to the strongest harmonic. */
static double spur(double f) {
    double top = -999, sp = -999;
    for (int b = 3; b < 18000.0 * N / SR; b++) {
        double hz = b * (double)SR / N, h = hz / f;
        double near = fabs(h - lround(h)) * f * N / SR;
        if (lround(h) >= 1 && near <= 6) { if (mag[b] > top) top = mag[b]; }
        else if (hz > 30 && mag[b] > sp) sp = mag[b];
    }
    return sp - top;
}
static double rms_block(void *s, int blocks) {
    static int16_t blk[256];
    double acc = 0;
    for (int i = 0; i < blocks; i++) {
        E->render(s, blk, 128);
        for (int k = 0; k < 256; k++) acc += (double)blk[k] * blk[k];
    }
    return sqrt(acc / (blocks * 256.0)) / 32768.0;
}

/* Harmonics 1..12 of `t` at `note`, relative to the strongest one. */
static void harmonics(const char *t, int note, int felt, double *h) {
    void *s = fresh();
    setp(s, t, "1");
    setp(s, "felt", felt ? "1" : "0");
    spectrum(s, note);
    double f = freq_of(note), top = -999;
    for (int k = 1; k <= 12; k++) { h[k] = level(k * f); if (h[k] > top) top = h[k]; }
    for (int k = 1; k <= 12; k++) h[k] -= top;
    E->destroy(s);
}
static double worst(const double *h, int parity, int from) {   /* loudest harmonic with k % 2 == parity, k >= from */
    double w = -999;
    for (int k = from; k <= 10; k++) if (k % 2 == parity && h[k] > w) w = h[k];
    return w;
}

int main(void) {
    E = mpc_engine();
    double h[13];
    const int A3 = 57;

    harmonics("t_o", A3, 0, h);
    check(worst(h, 0, 2) < -35 && worst(h, 1, 3) < -35, "O: harmonics 2-10 below -35 dB (loudest even %.1f, odd %.1f)", worst(h, 0, 2), worst(h, 1, 3));
    harmonics("t_o", A3, 1, h);
    check(worst(h, 0, 2) < -60 && worst(h, 1, 3) < -60, "O with F: harmonics 2-10 below -60 dB (even %.1f, odd %.1f)", worst(h, 0, 2), worst(h, 1, 3));
    harmonics("t_8", A3, 0, h);
    check(worst(h, 1, 1) < -50, "8: odd harmonics below -50 dB (loudest %.1f)", worst(h, 1, 1));
    check(h[2] > -1, "8: the 2nd harmonic leads (h2 %.1f dB)", h[2]);
    harmonics("t_c", A3, 0, h);
    check(worst(h, 0, 2) < -50, "C: even harmonics below -50 dB (loudest %.1f)", worst(h, 0, 2));
    check(h[3] < -12 && h[5] < h[3] && h[5] > -60, "C: energy low, h5 present and below h3 (h3 %.1f, h5 %.1f)", h[3], h[5]);
    harmonics("t_g", A3, 0, h);
    check(worst(h, 0, 2) < -50, "G: even harmonics below -50 dB (loudest %.1f)", worst(h, 0, 2));
    check(fabs(h[3] + 9.5) < 2.5, "G: h3 near -9.5 dB, a square's 1/3 (%.1f)", h[3]);
    double g9 = h[9];
    harmonics("t_gp", A3, 0, h);
    check(worst(h, 0, 2) < -50, "g: even harmonics below -50 dB (loudest %.1f)", worst(h, 0, 2));
    check(h[9] < g9 - 6, "g: softer than G, h9 at least 6 dB lower (g %.1f, G %.1f)", h[9], g9);
    harmonics("t_n", A3, 0, h);
    check(h[2] > -12 && h[4] > -15, "N: even and odd harmonics both present (h2 %.1f, h4 %.1f)", h[2], h[4]);

    /* N's fixed-width pulse: brighter relative to its fundamental in the bass than in the treble */
    double lo[13], hi[13];
    harmonics("t_n", 36, 0, lo); harmonics("t_n", 84, 0, hi);
    check(lo[2] > hi[2] - 0.5 || lo[3] > hi[3], "N: narrower (flatter spectrum) in the bass (C2 h3 %.1f, C6 h3 %.1f)", lo[3], hi[3]);

    /* aliasing at the top of the keyboard */
    const char *alias[] = {"t_g", "t_n", "t_c", "t_8"};
    for (int i = 0; i < 4; i++) {
        void *s = fresh();
        setp(s, alias[i], "1");
        spectrum(s, 108);
        double sp = spur(freq_of(108));
        /* -66 dB: at 2x oversampling G and N at C8 leave the 17th harmonic folded at -69.5 dB (4x gave -91 dB at twice the
         * per-voice cost on the Key 61; docs/DESIGN.md 3.2). */
        check(sp < -66, "%s at C8: no alias above -66 dB below 18 kHz (worst %.1f dB)", alias[i], sp);
        E->destroy(s);
    }

    /* tuning */
    {
        void *s = fresh();
        setp(s, "t_o", "1");
        spectrum(s, 69);
        int b = 1;
        for (int k = 2; k < N / 2; k++) if (mag[k] > mag[b]) b = k;
        double a = mag[b - 1], c = mag[b + 1], d = 0.5 * (a - c) / (a - 2 * mag[b] + c);
        double hz = (b + d) * SR / N;
        check(fabs(1200 * log2(hz / 440)) < 1, "A4 plays 440 Hz within 1 cent (%.2f Hz)", hz);
        E->destroy(s);
    }

    /* intensity key */
    {
        void *s = fresh();
        setp(s, "t_o", "1");
        setp(s, "key_source", "3");   /* Expression */
        setp(s, "key_mode", "1");     /* Key only */
        uint8_t on[3] = {0x90, 60, 100};
        E->midi(s, on, 3);
        check(rms_block(s, 40) < 1e-4, "Key only: silent until the key moves (note held)");
        uint8_t cc[3] = {0xB0, 11, 127};
        E->midi(s, cc, 3);
        rms_block(s, 20);   /* let the key smoothing settle */
        double full = rms_block(s, 40);
        check(full > 0.05, "Key only: sounds with the key open (rms %.3f)", full);
        uint8_t off[3] = {0x80, 60, 0};
        E->midi(s, off, 3);
        rms_block(s, 20);
        double after = rms_block(s, 40);
        check(fabs(after - full) < 0.01, "Key only: keeps sounding after note-off, the key is the gate (rms %.3f)", after);
        cc[2] = 64; E->midi(s, cc, 3); rms_block(s, 20);
        double half = rms_block(s, 40);
        double want = pow((64 / 127.0 - 0.03) / 0.97, 2.2);
        check(fabs(half / full - want) < 0.03, "Key curve: half travel gives x^2.2 (%.3f, want %.3f)", half / full, want);
        cc[2] = 2; E->midi(s, cc, 3); rms_block(s, 20);
        check(rms_block(s, 20) < 1e-4, "Key dead zone: 2/127 is silent");
        E->destroy(s);
    }
    {
        void *s = fresh();
        setp(s, "t_o", "1");
        uint8_t on[3] = {0x90, 60, 127}, off[3] = {0x80, 60, 0};
        E->midi(s, on, 3);
        double r = rms_block(s, 40);
        check(r > 0.05, "Note-gated, Velocity: a note sounds (rms %.3f)", r);
        E->midi(s, off, 3); rms_block(s, 520);   /* 1.5 s: about 12 release time constants */
        check(rms_block(s, 10) < 1e-4, "Note-gated: silent after release");
        E->destroy(s);
    }

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
