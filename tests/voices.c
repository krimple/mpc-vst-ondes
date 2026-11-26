/* Offline checks of the voice modes: Mono (re-attack every note), Legato (overlapping notes slide, one attack, a detached
 * note jumps), Poly (6 voices, allocation, stealing, sustain, Key Only). Reads the engine's test-only keys
 * (dbg_semis, dbg_semisK, dbg_active, dbg_env0). Build and run: tests/run.sh */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"

#define SR 44100
static const mpc_engine_t *E;
static int fails;

static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(int ok, const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    printf("%s ", ok ? "ok  " : "FAIL"); vprintf(fmt, a); printf("\n");
    va_end(a);
    if (!ok) fails++;
}
/* Set a param as a preset or saved state does (not subject to the tap debounce of Off/On switches). */
static void setp(void *s, const char *k, const char *v) { char b[96]; snprintf(b, sizeof b, "%s=%s\n", k, v); E->set_param(s, "state", b); }
static void run_ms(void *s, double ms) { static int16_t blk[256]; for (int n = (int)(ms * SR / 1000 / 128 + 0.5); n > 0; n--) E->render(s, blk, 128); }
static double dbg(void *s, const char *k) { char b[32]; E->get_param(s, k, b, sizeof b); return atof(b); }
static void on(void *s, int k) { uint8_t m[3] = {0x90, (uint8_t)k, 100}; E->midi(s, m, 3); }
static void off(void *s, int k) { uint8_t m[3] = {0x80, (uint8_t)k, 0}; E->midi(s, m, 3); }
static void cc(void *s, int c, int v) { uint8_t m[3] = {0xB0, (uint8_t)c, (uint8_t)v}; E->midi(s, m, 3); }
static double rms(void *s, int blocks) {
    static int16_t blk[256]; double a = 0;
    for (int i = 0; i < blocks; i++) { E->render(s, blk, 128); for (int k = 0; k < 256; k++) a += (double)blk[k] * blk[k]; }
    return sqrt(a / (blocks * 256.0)) / 32768.0;
}
static void *fresh(const char *mode) {
    void *s = E->create(NULL);
    setp(s, "drive", "0"); setp(s, "vib_human", "0"); setp(s, "ring_slew_ms", "0");
    setp(s, "voice_mode", mode);
    return s;
}
/* how many of the 6 voices sound at each of these pitches (semitones from A4) */
static int count_at(void *s, const double *want, int n) {
    int got = 0;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 6; k++) { char key[16]; snprintf(key, sizeof key, "dbg_semis%d", k); if (fabs(dbg(s, key) - want[i]) < 1e-3) { got++; break; } }
    return got;
}

int main(void) {
    E = mpc_engine();

    /* default */
    { void *s = E->create(NULL); char b[16]; E->get_param(s, "voice_mode", b, sizeof b);
      check(!strcmp(b, "1"), "default voice mode is Legato (%s)", b); E->destroy(s); }

    /* Mono: an overlapping note re-attacks (the envelope dips), glide applies to every change */
    {
        void *s = fresh("0");
        on(s, 60); run_ms(s, 200);
        double full = dbg(s, "dbg_env0");
        on(s, 64); run_ms(s, 3);
        double dip = dbg(s, "dbg_env0");
        check(full > 0.6 && dip < 0.5 * full, "Mono: an overlapping note re-attacks (env %.2f -> %.2f; peak is velocity 100 squared)", full, dip);
        run_ms(s, 200);
        check(dbg(s, "dbg_env0") > 0.95 * full, "Mono: ... and attacks back up (%.2f)", dbg(s, "dbg_env0"));
        off(s, 64); off(s, 60); run_ms(s, 50);
        setp(s, "glide_ms", "100");
        on(s, 72); run_ms(s, 100);
        double f = (dbg(s, "dbg_semis") - (60 - 69)) / 12;   /* releasing E first sent the pitch back to the C still held */
        check(f > 0.55 && f < 0.7, "Mono: a detached note still glides, from C (%.0f%% after 100 ms with glide 100 ms)", f * 100);
        E->destroy(s);
    }
    /* Legato: overlapping notes slide without re-attack; a detached note jumps */
    {
        void *s = fresh("1");
        setp(s, "glide_ms", "100");
        on(s, 60); run_ms(s, 200);
        double full = dbg(s, "dbg_env0");
        on(s, 72); run_ms(s, 3);
        check(dbg(s, "dbg_env0") > 0.95 * full, "Legato: an overlapping note keeps the envelope (%.2f -> %.2f)", full, dbg(s, "dbg_env0"));
        run_ms(s, 97);
        double f = (dbg(s, "dbg_semis") - (60 - 69)) / 12;
        check(f > 0.5 && f < 0.75, "Legato: and slides to it (%.0f%% after 100 ms with glide 100 ms)", f * 100);
        off(s, 72); off(s, 60); run_ms(s, 40);
        on(s, 48); run_ms(s, 3);
        check(fabs(dbg(s, "dbg_semis") - (48 - 69)) < 1e-3, "Legato: a detached note jumps, no slide (%.3f)", dbg(s, "dbg_semis"));
        check(dbg(s, "dbg_env0") > 0.05, "Legato: a detached note during the release does not dip to 0 first (env %.2f)", dbg(s, "dbg_env0"));
        E->destroy(s);
    }
    /* Poly */
    {
        void *s = fresh("2");
        on(s, 60); on(s, 64); on(s, 67); run_ms(s, 50);
        const double chord[3] = {60 - 69, 64 - 69, 67 - 69};
        check(dbg(s, "dbg_active") == 3 && count_at(s, chord, 3) == 3, "Poly: a C major chord sounds on 3 voices (%g held)", dbg(s, "dbg_active"));
        double r3 = rms(s, 40);
        check(r3 > 0.05 && r3 < 0.9, "Poly: the chord is audible and not clipping (rms %.3f)", r3);
        off(s, 64); run_ms(s, 5);
        check(dbg(s, "dbg_active") == 2, "Poly: releasing E leaves 2 held (%g)", dbg(s, "dbg_active"));
        on(s, 62); on(s, 65); on(s, 69); on(s, 71); on(s, 72); run_ms(s, 5);
        check(dbg(s, "dbg_active") == 6, "Poly: 7 notes asked, 6 voices held (%g)", dbg(s, "dbg_active"));
        /* 3 free voices, then the released E's voice, then the oldest held voice (C) is stolen */
        const double newest[5] = {62 - 69, 65 - 69, 69 - 69, 71 - 69, 72 - 69}, c[1] = {60 - 69}, g[1] = {67 - 69};
        check(count_at(s, newest, 5) == 5 && count_at(s, c, 1) == 0 && count_at(s, g, 1) == 1,
              "Poly: the newest five sound: free voices, then the released E's, then the oldest held (C) is stolen; G stays");
        on(s, 74); run_ms(s, 5);
        const double d_on[1] = {74 - 69};
        check(count_at(s, g, 1) == 0 && count_at(s, d_on, 1) == 1, "Poly: the next note steals the next oldest held voice (G)");
        for (int k = 0; k < 128; k++) off(s, k);
        run_ms(s, 5);
        check(dbg(s, "dbg_active") == 0, "Poly: all released (%g)", dbg(s, "dbg_active"));
        cc(s, 64, 127); on(s, 60); on(s, 67); off(s, 60); off(s, 67); run_ms(s, 5);
        check(dbg(s, "dbg_active") == 2, "Poly: the sustain pedal holds released notes (%g)", dbg(s, "dbg_active"));
        cc(s, 64, 0); run_ms(s, 5);
        check(dbg(s, "dbg_active") == 0, "Poly: lifting the pedal releases them (%g)", dbg(s, "dbg_active"));
        E->destroy(s);
    }
    /* Poly + Key Only: notes gate the voices (no drone), the key sets the level */
    {
        void *s = fresh("2");
        setp(s, "key_source", "3"); setp(s, "key_mode", "1");
        cc(s, 11, 127); on(s, 60); run_ms(s, 100);
        check(rms(s, 20) > 0.02, "Poly + Key Only: a note sounds with the key open");
        off(s, 60); run_ms(s, 1500);
        check(rms(s, 20) < 1e-4, "Poly + Key Only: no drone after the note ends");
        E->destroy(s);
    }
    /* switching mode releases everything */
    {
        void *s = fresh("2");
        on(s, 60); on(s, 64); run_ms(s, 5);
        setp(s, "voice_mode", "1"); run_ms(s, 5);
        check(dbg(s, "dbg_active") == 0, "changing the voice mode releases held notes (%g)", dbg(s, "dbg_active"));
        E->destroy(s);
    }

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
