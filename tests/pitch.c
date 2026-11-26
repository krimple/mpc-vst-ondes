/* Offline checks of M2's playing controls: transposition buttons, octave, keyboard bend, glide, ring (Bague) with its
 * latch, and the hand vibrato. Reads the engine's pitch through the test-only "dbg_semis" key (semitones from A4), plus
 * an FFT check that the audio follows. Build and run: tests/run.sh */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "engine.h"

#define SR 44100
static const mpc_engine_t *E;
static int fails;
/* Set a param as a preset or saved state does: not subject to the tap debounce, which merges a host's second flip of an
 * Off/On switch within 250 ms (src/ondes.c TOGGLE_DEBOUNCE_S) and would swallow these back-to-back test writes. */
static void setp(void *s, const char *k, const char *v) { char b[96]; snprintf(b, sizeof b, "%s=%s\n", k, v); E->set_param(s, "state", b); }

static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(int ok, const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    printf("%s ", ok ? "ok  " : "FAIL"); vprintf(fmt, a); printf("\n");
    va_end(a);
    if (!ok) fails++;
}

static void run_ms(void *s, double ms) {
    static int16_t blk[256];
    for (int n = (int)(ms * SR / 1000 / 128 + 0.5); n > 0; n--) E->render(s, blk, 128);
}
static double semis(void *s) { char b[32]; E->get_param(s, "dbg_semis", b, sizeof b); return atof(b); }
static void midi3(void *s, int a, int b, int c) { uint8_t m[3] = {(uint8_t)a, (uint8_t)b, (uint8_t)c}; E->midi(s, m, 3); }
static void bend(void *s, int v) { midi3(s, 0xE0, v & 127, v >> 7); }
static void *fresh(void) {
    void *s = E->create(NULL);
    setp(s, "drive", "0");
    setp(s, "ring_slew_ms", "0");
    setp(s, "vib_human", "0");
    return s;
}

/* Frequency of the loudest component (O only), Hann window + parabolic interpolation on dB. */
static double measure_hz(void *s) {
    enum { N = 16384 };
    static double re[N], im[N];
    static int16_t blk[256];
    for (int i = 0; i < N; i += 128) {
        E->render(s, blk, 128);
        for (int k = 0; k < 128; k++) re[i + k] = blk[2 * k] / 32768.0 * (0.5 - 0.5 * cos(2 * M_PI * (i + k) / (N - 1)));
    }
    /* direct DFT around the peak is enough: scan coarse bins, then refine */
    double best = 0; int bb = 1;
    for (int b = 8; b < 2000; b++) {
        double r = 0, q = 0;
        for (int i = 0; i < N; i += 4) { double a = 2 * M_PI * b * i / N; r += re[i] * cos(a); q -= re[i] * sin(a); }
        if (r * r + q * q > best) { best = r * r + q * q; bb = b; }
    }
    double m[3];
    for (int j = -1; j <= 1; j++) {
        double r = 0, q = 0;
        for (int i = 0; i < N; i++) { double a = 2 * M_PI * (bb + j) * i / N; r += re[i] * cos(a); q -= re[i] * sin(a); }
        m[j + 1] = 10 * log10(r * r + q * q + 1e-30);
    }
    (void)im;
    double d = 0.5 * (m[0] - m[2]) / (m[0] - 2 * m[1] + m[2]);
    return (bb + d) * (double)SR / N;
}
static double cents(double hz, double ref) { return 1200 * log2(hz / ref); }

int main(void) {
    E = mpc_engine();

    /* transposition buttons */
    {
        const char *k[6] = {"tr_m14", "tr_p14", "tr_p12", "tr_p1", "tr_p3", "tr_p5"};
        const double want[6] = {-0.5, 0.5, 1, 2, 4, 7};
        void *s = fresh();
        midi3(s, 0x90, 69, 100);
        int ok = 1;
        for (int i = 0; i < 6; i++) {
            setp(s, k[i], "1"); run_ms(s, 5);
            if (fabs(semis(s) - want[i]) > 1e-3) { ok = 0; printf("     %s: %.4f want %.1f\n", k[i], semis(s), want[i]); }
            setp(s, k[i], "0");
        }
        check(ok, "each transposition button moves the pitch by its interval (-1/4 +1/4 +1/2 +1 +3ce +5te)");
        for (int i = 1; i < 6; i++) setp(s, k[i], "1");
        run_ms(s, 5);
        check(fabs(semis(s) - 14.5) < 1e-3, "+1/4 +1/2 +1 +3ce +5te together: +14.5 semitones (%.3f)", semis(s));
        double hz = measure_hz(s), want_hz = 440 * pow(2, 14.5 / 12);
        check(fabs(cents(hz, want_hz)) < 2, "the audio follows: %.2f Hz, want %.2f (%.2f cents)", hz, want_hz, cents(hz, want_hz));
        E->destroy(s);
    }
    /* octave */
    {
        void *s = fresh();
        setp(s, "octave", "2");
        midi3(s, 0x90, 69, 100); run_ms(s, 5);
        check(fabs(semis(s) - 12) < 1e-3, "octave +1: +12 semitones (%.3f)", semis(s));
        E->destroy(s);
    }
    /* Clavier: bend range and spring-back */
    {
        void *s = fresh();
        setp(s, "bend_range", "2");
        midi3(s, 0x90, 69, 100); bend(s, 16383); run_ms(s, 5);
        check(fabs(semis(s) - 2) < 1e-3, "Clavier: full bend up with range 2 is +2 (%.4f)", semis(s));
        bend(s, 0); run_ms(s, 5);
        check(fabs(semis(s) + 2) < 1e-3, "Clavier: full bend down is -2 (%.4f)", semis(s));
        bend(s, 8192); run_ms(s, 5);
        check(fabs(semis(s)) < 1e-3, "Clavier: a jump back to the centre is followed, no latch (%.4f)", semis(s));
        E->destroy(s);
    }
    /* glide */
    {
        void *s = fresh();
        setp(s, "glide_ms", "100");
        midi3(s, 0x90, 69, 100); run_ms(s, 1000);
        midi3(s, 0x90, 81, 100); run_ms(s, 100);
        double frac = semis(s) / 12;
        check(fabs(frac - (1 - exp(-1))) < 0.03, "glide 100 ms: 63%% of the way after 100 ms (%.1f%%)", frac * 100);
        E->destroy(s);
    }
    /* Bague: span, latch, a key resets the ring */
    {
        void *s = fresh();
        setp(s, "mode", "1");                 /* Bague */
        setp(s, "ring_span", "1");            /* 2 oct */
        midi3(s, 0x90, 69, 100); bend(s, 16383); run_ms(s, 5);
        check(fabs(semis(s) - 24) < 1e-3, "Bague: full bend with a 2-octave span is +24 (%.4f)", semis(s));
        bend(s, 12288); run_ms(s, 5);
        double held = semis(s);
        check(fabs(held - 12) < 0.01, "Bague: half bend up is +12 (%.4f)", held);
        bend(s, 8192); run_ms(s, 5);                  /* the finger lifts: the strip snaps to the centre */
        check(fabs(semis(s) - held) < 1e-3, "latch: a jump straight to the centre holds the pitch (%.4f)", semis(s));
        for (int v = 12288; v >= 8192; v -= 500) bend(s, v);   /* a wheel coming back by small steps */
        bend(s, 8192); run_ms(s, 5);
        check(fabs(semis(s)) < 1e-3, "latch: a wheel returning through the values between is followed to 0 (%.4f)", semis(s));
        bend(s, 12288); run_ms(s, 5);
        midi3(s, 0x90, 72, 100); run_ms(s, 5);
        check(fabs(semis(s) - 3) < 1e-3, "Bague: a key puts the ring on that note, offset cleared (%.4f)", semis(s));
        setp(s, "ring_latch", "0");
        bend(s, 12288); run_ms(s, 5); bend(s, 8192); run_ms(s, 5);
        check(fabs(semis(s) - 3) < 1e-3, "latch off: the jump to the centre is followed (%.4f)", semis(s));
        double hz = (bend(s, 16383), run_ms(s, 5), measure_hz(s)), want = 440 * pow(2, (3 + 24) / 12.0);
        check(fabs(cents(hz, want)) < 2, "Bague: the audio follows the ring: %.2f Hz, want %.2f", hz, want);
        E->destroy(s);
    }
    /* ring slew: no pitch step larger than a cent per sample-chunk during a jump */
    {
        void *s = fresh();
        setp(s, "mode", "1");
        setp(s, "ring_slew_ms", "8");
        midi3(s, 0x90, 69, 100); run_ms(s, 5);
        bend(s, 16383);
        static int16_t blk[256];
        E->render(s, blk, 128);
        double first = semis(s);
        run_ms(s, 100);
        check(first > 0.1 && first < 24 * 0.5 && fabs(semis(s) - 24) < 0.01,
              "ring slew 8 ms: a jump arrives smoothly (%.2f after one block, %.3f after 100 ms)", first, semis(s));
        E->destroy(s);
    }
    /* vibrato */
    {
        void *s = fresh();
        setp(s, "vib_depth", "30");
        setp(s, "vib_rate", "5.5");
        midi3(s, 0x90, 69, 100); run_ms(s, 50);
        double mx = 0;
        for (int i = 0; i < 200; i++) { run_ms(s, 3); mx = fmax(mx, fabs(semis(s))); }
        check(mx < 1e-6, "vibrato: none until the mod wheel moves (max %.4f)", mx);
        midi3(s, 0xB0, 1, 127);
        int cross = 0; double prev = 0; mx = 0;
        for (int i = 0; i < 690; i++) {   /* 2 s in 2.9 ms blocks */
            run_ms(s, 2.9);
            double v = semis(s);
            if (i && (prev < 0) != (v < 0)) cross++;
            prev = v; mx = fmax(mx, fabs(v));
        }
        check(fabs(mx - 0.30) < 0.01, "vibrato: depth 30 cents with the wheel full (peak %.3f st)", mx);
        check(abs(cross - 22) <= 1, "vibrato: 5.5 Hz (%d zero crossings in 2 s, want 22)", cross);
        setp(s, "vib_human", "100");
        double lo = 1, hi = 0;
        for (int w = 0; w < 10; w++) {
            double m = 0;
            for (int i = 0; i < 100; i++) { run_ms(s, 2.9); m = fmax(m, fabs(semis(s))); }
            lo = fmin(lo, m); hi = fmax(hi, m);
        }
        check(hi - lo > 0.02 && hi < 0.30 * 1.3, "vibrato human 100%%: depth drifts (peaks %.3f..%.3f st)", lo, hi);
        setp(s, "vib_src", "2");   /* Off */
        run_ms(s, 5);
        check(fabs(semis(s)) < 1e-6, "vibrato source Off: no vibrato (%.4f)", semis(s));
        E->destroy(s);
    }

    /* Strip Notes Up */
    {
        void *s = fresh();
        midi3(s, 0x90, 24, 100); run_ms(s, 5);
        check(fabs(semis(s) - (24 - 69)) < 1e-3, "Strip Notes Off: note 24 plays as 24 (%.3f)", semis(s));
        setp(s, "strip_notes", "2");      /* +2 Oct */
        midi3(s, 0x90, 36, 100); run_ms(s, 5);
        check(fabs(semis(s) - (60 - 69)) < 1e-3, "Strip Notes +2: note 36 (the strip's top) plays as 60 (%.3f)", semis(s));
        midi3(s, 0x80, 36, 0); run_ms(s, 5);
        check(fabs(semis(s) - (48 - 69)) < 1e-3, "Strip Notes +2: back to held note 24, shifted to 48 (%.3f)", semis(s));
        midi3(s, 0x90, 37, 100); run_ms(s, 5);
        check(fabs(semis(s) - (37 - 69)) < 1e-3, "Strip Notes +2: note 37 is not shifted (%.3f)", semis(s));
        midi3(s, 0x80, 37, 0); midi3(s, 0x80, 24, 0); run_ms(s, 1500);   /* about 12 release time constants */
        static int16_t blk[256]; double acc = 0;
        for (int i = 0; i < 100; i++) { E->render(s, blk, 128); for (int k = 0; k < 256; k++) acc += abs(blk[k]); }
        check(acc / 25600 < 1, "Strip Notes: note-offs release the shifted notes (mean |x| %.3f)", acc / 25600);
        E->destroy(s);
    }

    /* tap debounce: MPC's two presses per tap flip a switch on and straight back */
    {
        void *s = fresh();
        char b[16], k[32];
        E->set_param(s, "t_8", "1"); E->set_param(s, "t_8", "0");   /* one tap: on, then the second press flips it back */
        E->get_param(s, "t_8", b, sizeof b);
        check(!strcmp(b, "1"), "tap debounce: a switch flipped back within 250 ms stays on (t_8 = %s)", b);
        int n = mpc_engine_changed(s, k, sizeof k);
        check(n == 1 && !strcmp(k, "t_8") && mpc_engine_changed(s, k, sizeof k) == 0,
              "tap debounce: the engine reports t_8 once so the host shows it lit");
        struct timespec ts = {0, 300000000}; nanosleep(&ts, NULL);
        E->set_param(s, "t_8", "0"); E->set_param(s, "t_8", "1");   /* next tap, 300 ms later: off, then flipped back */
        E->get_param(s, "t_8", b, sizeof b);
        check(!strcmp(b, "0"), "tap debounce: a later tap turns it off (t_8 = %s)", b);
        E->set_param(s, "t_g", "1"); E->set_param(s, "t_g", "1");   /* if MPC sends the same value twice */
        E->get_param(s, "t_g", b, sizeof b);
        check(!strcmp(b, "1"), "tap debounce: the same value twice simply sets it (t_g = %s)", b);
        E->set_param(s, "state", "t_c=1\n"); E->set_param(s, "state", "t_c=0\n");
        E->get_param(s, "t_c", b, sizeof b);
        check(!strcmp(b, "0"), "tap debounce: state restores are not debounced (t_c = %s)", b);
        E->destroy(s);
    }

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
