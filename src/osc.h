/* The Model 7 tone generator: one phase drives every timbre (O 8 C G g N), so they are phase-locked as on the
 * instrument (Loriod's block diagram, docs/DESIGN.md 3.2). Runs at 2x the output rate and decimates with a FIR; the
 * discontinuities are band-limited with polyBLEP (steps) and polyBLAMP (corners).
 * Several voices (Poly) are summed at the oversampled rate, each with its own gain, and share one decimator: an extra
 * voice costs only its waveform maths. */
#pragma once

#ifndef OSC_OS
#define OSC_OS 2                 /* oversampling factor. 4x halves nothing audible (C8 alias -91 vs -69.5 dB) and costs twice the
                                    per-voice work: worst case (Tutti, Poly 6) 31% of a block at 4x, 16% at 2x (Key 61, 2026-11-26) */
#endif
#ifndef OSC_FIR_TAPS
#define OSC_FIR_TAPS (OSC_OS == 2 ? 63 : 127)   /* decimator length (odd): the same transition band in Hz at either rate */
#endif
#define OSC_VOICES 6

enum { T_O, T_8, T_C, T_G, T_GP, T_N, T_COUNT };

typedef struct {
    double phase;                /* 0..1 */
    float freq;                  /* Hz, set by the caller each chunk */
    float amp0, amp1;            /* gain at the start and end of the chunk (linear ramp); both 0 = silent, skipped */
    float g_z1, g_z2;            /* g lowpass state */
} osc_voice_t;

typedef struct {
    osc_voice_t v[OSC_VOICES];
    int on[T_COUNT];             /* drawer switches (the caller resolves T) */
    float oct_gain;              /* 8 slider as a gain */
    float g_cut;                 /* g lowpass cutoff, Hz */
    float impurity;              /* O's 3rd harmonic, linear (0 with F) */
    float creux_clip;            /* C clip level on the +-1 triangle, 0..1 */
    float nas_width_s;           /* N pulse duration, seconds */
    float g_b0, g_b1, g_b2, g_a1, g_a2, g_cut_set;   /* g lowpass coefficients (biquad at the oversampled rate) */
    /* decimator */
    float fir[OSC_FIR_TAPS];
    float hist[OSC_FIR_TAPS * 2];
    int hpos;
} osc_t;

void osc_init(osc_t *o, float out_rate);
/* Render n output-rate samples of every voice's note timbres, each scaled by its gain ramp, summed into out (overwrites). */
void osc_render(osc_t *o, float *out, int n);
