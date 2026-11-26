# Ondes: design brief

An Ondes Martenot-style monophonic instrument for MPC OS, as a VST2 on the shared `mpc-vst-plugins` wrapper.
Target device first: MPC Key 61 (Gen1, armv7). Status (2026-11-26): **M1 and M2 built and passing offline**
(`tools/test_port.sh` PASSED; `tests/run.sh`: `spectra` and `pitch` PASSED); Ondes not yet run on a device. Spike S0 is
answered on the device (section 9) with the probe in `probe/midiprobe/`.

Reference material is in `research/` (git-ignored: third-party documents with no stated licence; never commit or ship
them). The model is the **Model 7 transistor instrument (1970s-80s)**, because it is the best documented: Jean Loriod's block
diagram, overall scheme and timbre/spectrum chart (`research/m7-*.jpg`, from his 1993 book via ondes.net) and the factory
board schematics (`research/JB*.pdf`).

## 1. Goals and non-goals

**Goals**
- Model the Model 7 signal chain: the oscillator, the eight drawer switches (O 8 C G g N S T), the intensity key, the
  transposition buttons, the felt filter, the tone controls and the speakers (diffuseurs).
- Make the ondes' way of playing work from MPC controls: continuous ring glissando, keyboard vibrato and the intensity key
  as the "bow".
- Make the speakers distinctive, the Palme sympathetic strings above all.
- Ship to the catalog (`docs/PORTING.md` section 5), with a native skin, presets and a bench PASS on the Key 61.

**Non-goals (v1)**
- The vacuum-tube heterodyne models (1928 to 1960s): different circuit, no measurements.
- Sampled speaker or gong impulse responses: we have no instrument to measure. Everything is synthesized.
- Polyphony. The instrument is monophonic. A "Poly 4" mode is a possible v2 (section 10).
- Faithful haptics (ring, key). MPC controls stand in for them.

## 2. Signal chain

```
 note / ring ──► pitch logic ──► expo converter ──► triangle core (2x oversampled)
 (transposers, octave, vibrato, glide)                    │
              ┌──────────────┬──────────────┬─────────────┼──────────────┬──────────────┐
           clipper         sine shaper    rectifier      comparator     pulse former   noise (pink)
              │               │          (doubler)          │              │              │
              C               O              8 ◄─slider     G ──lowpass──► g ◄─slider      N        S
              └───────┬───────┴──────┬───────┴──────┬───────┴──────┬───────┘              │
                      └──────────────┴── drawer switches, T = C+O+8+G+N ──┘                  │
                                                │                                           │
                                        Note/Souffle balance ◄──────────────────────────────┘
                                                │
                       intensity key / pedal (VCA, with curve and smoothing)
                                                │
                       F (feutré) filter ─► Grave/Aigu tone ─► Puissance (volume)
                                                │
                                   speaker balance D1 vs D2+D3, D-switches
          ┌──────────────────┬──────────────────┼──────────────────┐
     D1 Principal      Résonance (D4)       D2 Palme         D3 Métallique
     speaker model     spring reverb       12 strings        gong modes
          └──────────────────┴────── stereo mix (spread) ─────────┘
```

Block sizes and rates: 44.1 kHz, 128-frame blocks (MPC's period). Float internally, int16 out (wrapper).

## 3. Engine spec

### 3.1 Pitch
- **Expo converter:** `f = tune_ref * 2^((n - 69 + offsets) / 12)`, with `offsets` = transposers + octave + vibrato +
  ring + fine tune, in float semitones (so quarter tones and slides cost nothing).
- **Keyboard mode (Clavier):** last-note priority. A new key changes pitch **immediately** (as on the real instrument;
  articulation comes from the intensity key, not the keyboard). Optional `glide_ms` for players without a ring controller.
- **Ring mode (Bague):** pitch is continuous. Source: pitch bend (14-bit) over a span set by `ring_span`, centred on the
  last note played (a key sets the anchor and clears the ring offset; the bend slides from it). 72 semitones over 16384
  steps is 0.44 cent per step, fine enough. A slew (`ring_slew_ms`, shown as "Pitch Slew", default 8 ms; it also smooths
  Clavier bends) covers the ~6 ms spacing of bend messages (S0).
- **Ring latch** (`ring_latch`, default On, Bague only): a bend jump of more than 1024 straight to the centre (8191-8193)
  is the finger leaving the touch strip, which snaps back (S0); the pitch holds where it was, as the real ring stays where
  it is left. A wheel coming back passes through the values between and is followed. A finger landing on the strip again
  moves the ring to that spot (smoothed by the slew).
- **Strip Notes Up** (`strip_notes`: Off, +1, +2, +3 oct; appended as parameter 42): the Key 61 touch strip's Notes mode
  plays a C-major octave at MIDI 24-36 (device, 2026-11-26), too low to use. Notes at 36 and below sound 1-3 octaves up
  (36 included, so the strip's top step does not drop back). The plugin cannot tell strip notes from keys, so the lowest
  key (36) and keys an octave-down shift brings below it move too. Held keys are stored as received, so note-offs match.
- **Voices** (`voice_mode`, parameter 43): **Mono** re-attacks every note (a 3 ms dip, then the attack) and glides on
  every change; **Legato** (default, closest to the instrument) slides between overlapping notes with one attack, and a
  detached note jumps; **Poly** has 6 voices (free voice, else the oldest released, else the oldest held is stolen with a
  3 ms dip), no glide, a 0.5 gain for headroom, sustain per voice. The intensity key, bend/ring, vibrato and transposers
  act on all voices. In Poly, Key Only means the notes gate the voices and the key alone sets the level (no drone).
  Changing the mode releases every note. Tests: `tests/voices.c`.
- **Pitch is recomputed every 16 samples** (glide, slew, vibrato), not once per 128-frame block, so slides and vibrato have
  no block-rate steps.
- **Keyboard vibrato:** the real one is the hand rocking a key sideways, not an LFO. Two inputs, summed:
  - Manual: pitch bend in keyboard mode, ±`bend_range` semitones (default 1).
  - "Hand" vibrato: a sine at `vib_rate` (default 5.5 Hz). Its depth comes from the chosen controller (mod wheel by
    default, or channel aftertouch) up to `vib_depth` cents. `vib_human` adds slow random drift of rate and depth (±10%)
    so it doesn't sound like a synth LFO.
- **Transposition buttons (transpositeurs):** six latching toggles, values from the Model 7 drawer: −¼ tone (−0.5 st),
  +¼ (+0.5), +½ (+1), +1 tone (+2), +tierce (+4), +quinte (+7). They add up, so the most is +14.5 st.
  Cross-check: `svgeesus/ondesModulaire` `transpositeurs` uses the same values.
- **Octave switch:** −1 / 0 / +1.
- **Range:** clamp the oscillator to 16 Hz to 8.4 kHz (about C0 to C9). The real keyboard is 72 keys, C1 to C8 with the
  octave switch.

### 3.2 Oscillator and timbres
One phase accumulator drives a band-limited **triangle core**. Every timbre is derived from the same phase, so they are
phase-locked, as on the instrument. Run the core and shapers at **2x** and decimate with a 63-tap Blackman FIR (cutoff
20.5 kHz), plus polyBLEP (G and N edges) and polyBLAMP (C clip corners, 8 cusps). G and N at C8 leave an alias at
−69.5 dB below 18 kHz (the 17th harmonic folding at 88.2 kHz); 4x gives −91 dB but doubles the per-voice work.
Measured on the Key 61 (device, 2026-11-26, engine timed alone, mean % of a 128-frame block on one core):

| | 4x | 2x |
|---|---|---|
| Sine, 1 voice | 3.7 | 2.5 |
| Everything on, 1 voice | 7.9 | 4.9 |
| Sine, Poly 6 | 7.6 | 4.5 |
| Everything on, Poly 6 | 30.7 | 16.3 |

NEON flags (`-mfpu=neon-vfpv4`, even with `-O3 -ffast-math`) changed nothing that matters (the work is scalar); a
single-precision version of the per-sample code was *slower* (41% worst case at 4x). The 2x build's `tools/bench.c`
run: worst case (Poly, Tutti + g + S + Muffle + Drive) p99 16.1%, worst block 18.0% → **WARN**; shipping defaults play at
~3%, but the random Q-Link sweep reaches p99 17.1% (it switches Poly and the timbres on) → WARN. Poly voices are summed at
the oversampled rate into one shared decimator (`osc.c`).

| Switch | Name | Model 7 circuit | Implementation | Spectrum check (Loriod Fig. 2) |
|---|---|---|---|---|
| O | Onde | sine shaper | sine from phase (table or polynomial), plus a trace of 3rd harmonic (`impurity`, about −40 dB) that F suppresses ("purer with F", factory sheet) | fundamental only |
| 8 | Octaviant | "doubler" = full-wave rectifier on the sine; 5-step slider | `|sin| − 2/π`; slider `oct_level` 1-5 sets its level | even harmonics only (2, 4, 6…) |
| C | Creux | "écrêtage" (crest clipper) on the triangle | triangle clipped at ±`creux_clip` (0.9), then normalised. Harmonic n goes as sin(nπk/2)/n², so below about 0.85 the odd harmonics stop falling steadily (0.8 nulls h5, 0.7 puts h5 above h3), against Loriod's "energy concentrated low" | odd harmonics, energy low |
| G | Gambe | comparator → 50% square | band-limited square | odd harmonics, steady fall-off |
| g | Petit gambe | G through a lowpass; 5-step slider | 2-pole lowpass on G, cutoff from `g_level` 1-5 (placeholder 300, 600, 1200, 2400, 4800 Hz, **fixed in Hz**, not tracking pitch, as an RC filter would). The factory sheet says g "also changes the timbre of N": see open question Q3 | softer odd harmonics |
| N | Nasillard | "impulsions" from the detector | narrow pulse of **fixed duration** `nas_width_us` (placeholder 150 µs), duty capped at 40%, so it thins in the bass and fills in the treble | all harmonics, nasal |
| S | Souffle | noise generator | pink noise (Voss-McCartney or 3-pole filter), level from Note/Souffle balance | broadband |
| T | Tutti | sum of C O 8 G N | switch that turns on C O 8 G N together | |

- Switches are on/off; their outputs **sum** with fixed per-timbre gains (set by ear and against the chart), then a soft
  limiter (tanh-ish) catches Tutti peaks. No per-timbre level knobs beyond the two drawer sliders: that is how the
  instrument works.
- On one transistor-model sheet S means *Sourdine* (felt mute) instead. We follow Loriod (Souffle = noise) and have F as
  the mute.

### 3.3 Intensity key (touche d'intensité)
The instrument's most important control. The Sorbonne study says output depends mainly on how far the key moves, then on
pressure. The later key was a pouch of conductive powder (`Sacs.pdf` on ondes.net): resistance falls steeply as it's
squeezed.
- **Source** (`key_source`): Velocity env / Aftertouch / Mod wheel (CC1) / Expression (CC11, the Key 61's expression
  pedal) / Breath (CC2).
- **Articulation** (`key_mode`):
  - *Note-gated* (default): `amp = env(note, velocity) × key`, with `key` = 1 until the chosen controller first moves.
    Plays sensibly from pads and sequences.
  - *Key only* (authentic): a note sets pitch only; sound comes **only** from the key. With a pedal or aftertouch this is
    the real technique (silence between notes, swells, legato through pitch changes).
- **Curve:** dead zone `key_dead` (default 3%), then `y = x^key_curve` (default 2.2, about the pouch law), then the
  VCA. `key_smooth_ms` (default 2 ms) removes zipper noise but stays fast enough for the *percuté* attack.
- **Velocity env** (Velocity source, or Note-gated with no controller): attack `env_att_ms` (1-500), release
  `env_rel_ms` (5-2000). A short attack plus high velocity gives *percuté*.

### 3.4 Filters and output
- **F (feutré):** switch. A 1-pole lowpass at about 1.2 kHz (placeholder), plus O's impurity removed.
- **Grave / Aigu:** bass and treble shelves, ±12 dB (100 Hz and 3 kHz placeholders). These are the "Grave" and "Aigu"
  knobs on the Model 7 front panel.
- **Puissance:** master volume after the filters ("sets the overall level without changing the timbres", factory sheet).

### 3.5 Speakers (diffuseurs)
Four switches D1-D4, a `spk_balance` knob (D1 against D2+D3, as on Model 7), a level per speaker on the speaker page
and `spread` (stereo placement: Principal centre, Palme left, Métallique right, Résonance wide). Each speaker is a
**coloured, separate path**: they add together, they don't chain.

- **D1 Principal:** the "standard" speaker. 2nd-order highpass at 70 Hz, a broad +2 dB bump at 1 kHz, a 2nd-order rolloff
  at 7 kHz, and a little asymmetric saturation (`drive`) for the 35 W amplifier.
- **D4 Résonance:** spring/delay-line reverb (the factory note says "lignes de retard"). A simplified Parker/Välimäki
  spring: a chain of 8-12 first-order allpasses (dispersion "chirp") inside a feedback delay of about 35 ms, two springs
  detuned for stereo. `res_decay` and `res_mix`.
- **D2 Palme:** 12 strings tuned chromatically across one octave that resonate in sympathy.
  - 12 Karplus-Strong / 1-D waveguides, one per pitch class, with fractional-delay (allpass) tuning and a 1-pole loop
    damping filter. Each string's delay follows **master tune only** (the strings are fixed; transposers do not retune them).
  - Excitation: the instrument signal at gain `palme_couple`. Each string rings when the note, or one of its harmonics,
    lands on its pitch. Off-pitch notes (quarter tones) barely excite it, which is part of the sound.
  - `palme_decay` (0.5-12 s), `palme_bright` (loop damping), `palme_octave` (string register, 2-4). Stringing
    reference: `research/palme-cordes.pdf`.
- **D3 Métallique:** the speaker motor drives a gong instead of a cone.
  - 32 parallel 2-pole modal resonators at inharmonic ratios (published gong/plate mode ratios, scaled by `metal_size`),
    decay set by `metal_decay`, driven by the signal through a mild nonlinearity (energy spreads into the modes the way a
    driven gong shimmers), mixed with a band-limited "motor" direct path.
  - The hardest part to get right. Tune by ear against recordings. It's the last speaker in the milestones.

CPU estimate per output sample: oscillator about 300 ops, Palme about 150, Métallique about 200, spring about 100,
filters about 50. That's about 0.8 M ops per 128-frame block, a few % of one Cortex-A17 core. Bench target **under 15% of
one core** on the Key 61 with every speaker on (docs/BENCH.md).

## 4. Parameters (params.json, VST index = order, append only after release)

**Current order (`vst/params.json`):** M1's 25 (the 8 timbre switches, the 8 and g sliders, Note/Souffle, F, Grave, Aigu,
Puissance, Octave, Tune, the 7 intensity-key params, Drive), then M2's 16 (mode, ring span/slew/latch, glide, bend range,
vibrato source/rate/depth/human, the 6 transposers): 41 in all. Ring span is an option list (1-4 oct). Later milestones
append theirs. The table
below is the target set, not the index order; indices are final at the first release (until then `GEN_VST_RELOCK=1`).

| # | key | name | range / options | default |
|---|---|---|---|---|
| 0 | `t_o` | O Onde | Off/On | On |
| 1 | `t_8` | 8 Octaviant | Off/On | Off |
| 2 | `t_c` | C Creux | Off/On | Off |
| 3 | `t_g` | G Gambe | Off/On | Off |
| 4 | `t_gp` | g Petit gambe | Off/On | Off |
| 5 | `t_n` | N Nasillard | Off/On | Off |
| 6 | `t_s` | S Souffle | Off/On | Off |
| 7 | `t_t` | T Tutti | Off/On | Off |
| 8 | `oct_level` | 8 slider | 1-5 int | 3 |
| 9 | `g_level` | g slider | 1-5 int | 3 |
| 10 | `souffle_bal` | Note/Souffle | 0-100 % | 0 |
| 11 | `felt` | F Feutré | Off/On | Off |
| 12 | `grave` | Grave | −12…+12 dB | 0 |
| 13 | `aigu` | Aigu | −12…+12 dB | 0 |
| 14 | `volume` | Puissance | −60…+6 dB | −6 |
| 15 | `mode` | Clavier/Bague | Clavier, Bague | Clavier |
| 16 | `ring_span` | Ring span | ±1, ±2, ±4 oct | ±2 oct |
| 17 | `ring_slew_ms` | Ring slew | 0-50 ms | 5 |
| 18 | `glide_ms` | Glide | 0-1000 ms | 0 |
| 19 | `bend_range` | Bend range | 0-12 st | 1 |
| 20 | `vib_src` | Vibrato source | Mod wheel, Aftertouch, Off | Mod wheel |
| 21 | `vib_rate` | Vibrato rate | 3-9 Hz | 5.5 |
| 22 | `vib_depth` | Vibrato depth | 0-100 cents | 30 |
| 23 | `vib_human` | Vibrato human | 0-100 % | 40 |
| 24-29 | `tr_m14` `tr_p14` `tr_p12` `tr_p1` `tr_p3` `tr_p5` | −¼ +¼ +½ +1 +3ce +5te | Off/On each | Off |
| 30 | `octave` | Octave | −1, 0, +1 | 0 |
| 31 | `tune` | Tune | −100…+100 cents | 0 |
| 32 | `key_source` | Key source | Velocity, Aftertouch, Mod wheel, Expression, Breath | Velocity |
| 33 | `key_mode` | Articulation | Note-gated, Key only | Note-gated |
| 34 | `key_curve` | Key curve | 0.5-4 | 2.2 |
| 35 | `key_dead` | Key dead zone | 0-20 % | 3 |
| 36 | `key_smooth_ms` | Key smoothing | 0-30 ms | 2 |
| 37 | `env_att_ms` | Attack | 1-500 ms | 8 |
| 38 | `env_rel_ms` | Release | 5-2000 ms | 120 |
| 39-42 | `d1` `d2` `d3` `d4` | Principal, Palme, Métallique, Résonance | Off/On each | D1 On |
| 43 | `spk_balance` | D1 ↔ D2+D3 | 0-100 % | 50 |
| 44-47 | `lvl_d1`…`lvl_d4` | Speaker levels | −24…+6 dB | 0 |
| 48 | `spread` | Spread | 0-100 % | 50 |
| 49 | `drive` | Principal drive | 0-100 % | 15 |
| 50 | `res_decay` | Résonance decay | 0.3-5 s | 1.8 |
| 51 | `res_mix` | Résonance mix | 0-100 % | 35 |
| 52 | `palme_couple` | Palme coupling | 0-100 % | 50 |
| 53 | `palme_decay` | Palme decay | 0.5-12 s | 4 |
| 54 | `palme_bright` | Palme brightness | 0-100 % | 50 |
| 55 | `palme_octave` | Palme register | 2, 3, 4 | 3 |
| 56 | `metal_size` | Métallique size | 50-200 % | 100 |
| 57 | `metal_decay` | Métallique decay | 0.3-8 s | 2.5 |
| 58 | `metal_drive` | Métallique drive | 0-100 % | 30 |

Notes: Off/On and option params are option lists (stepped by the wrapper). The 1-5 sliders are `"display": "int"`.
UI plumbing (page pickers) gets `"automatable": false`. Engine-only constants (per-timbre gains, `impurity`,
`creux_clip`, `nas_width_us`, filter cutoffs) stay in code until calibration settles them. Promote one to a param only by
**appending**.

## 5. Skin (layout.conf, `"art": "html"` for our own look)

**Built 2026-11-26** (`vst/layout.conf`, `vst/skin.css`), replacing the auto-layout after a device look ("fonts tiny, French
options, hard to use"): three pages, SOUND (8 timbre buttons 290x104, shape and tone knobs, Muffle), PLAY (Keyboard/Ribbon,
mode panels for glide/bend or ribbon range/hold, Strip Notes Up, octave, fine tune, smoothing, 6 transposer buttons 190x150),
EXPRESSION (intensity source and articulation, curve/dead zone/smoothing, vibrato, envelope). English names and options;
baked text 22-32 px, live names/values `label_scale=1.3` + `scale_names=1`; lit buttons amber with dark text (a `skin.css`
rule on the on-image's fill). The table below was the plan; the built pages above are what ships.

Look: a cream-and-walnut 1970s instrument with lit square drawer keys labelled with the instrument's own letters (O 8 C
G g N S T, D1-D4) in bold, like the Model 7 drawer. Our own artwork; no "Martenot" wordmark or panel copy.

| Page | Contents | Q-Links (column 1 first: it is CC 20-23 and the Key-series' first knobs) |
|---|---|---|
| **TIROIR** (drawer, default) | 8 timbre keys, 8 and g sliders, Note/Souffle dial, F, 6 transposer keys, D1-D4 keys, speaker balance | 1 Note/Souffle · 2 8 slider · 3 g slider · 4 Puissance · 5 Balance · 6 Grave · 7 Aigu · 8 Vibrato depth |
| **JEU** (playing) | Clavier/Bague, ring span/slew, glide, bend range, vibrato source/rate/depth/human, octave, tune | 1 Ring span · 2 Vib depth · 3 Vib rate · 4 Glide · 5 Bend range · 6 Vib human · 7 Ring slew · 8 Tune |
| **TOUCHE** (intensity) | key source, articulation, curve (drawn as a graph picture), dead zone, smoothing, attack/release | 1 Curve · 2 Attack · 3 Release · 4 Smoothing · 5 Dead zone |
| **DIFFUSEURS** | per speaker: on/level and its own controls, spread | bank 1: four levels + spread; bank 2: Palme decay/bright/couple, Résonance decay/mix, Métallique size/decay/drive |

- Drawer keys are toggle `Button`s with lit artwork. Choice lists are `enum_h` (never `menu`), or `popup` for Key source.
- `when=mode:Bague` shows the ring controls in place of glide/bend.
- Instruments-browser tile: `art/tile.png` (270x110).
- Offline preview before the device (`tools/studio.py preview`).

## 6. Presets

**Built 2026-11-26** as VST programs (vst.json `"programs": {"param": "preset"}`, parameter 44, 40 programs):
0-9 built in (Pure Sine, Lyric, Hollow Clarinet, Bowed Strings, Nasal Bassoon, Breath, Tutti, Ribbon Glissando, Expressive
Pedal, Poly Organ; each = the defaults plus its own settings, `BUILTIN[]` in `src/ondes.c`), 10-39 the user's saved
presets. Why: MPC saves a plugin preset as `.xpl` in `<drive>/MPC Documents/Plugin Presets/Instruments/<plugin folder>/`
and lists it in the Browser there, but the plugin's PRESET field lists only VST programs (device, Key 61, 2026-11-26).
A thread reads those `.xpl` files when the plugin is created (JUCE base64 -> fxb chunk set, uid `OndM`, chunk at 0xA0 ->
our state text), sorted by name; a preset saved later appears after the plugin is inserted again. A saved preset's own
`preset=` value is ignored on load; a project's state restore only notes the program number. Empty slots show "-" (the
program count is fixed at build time). Tests: `tests/presets.c` (and the device's real `.xpl` files decoded offline).

Original plan (presets.json, every param set in each):

1. **Onde pure**: O + F, D1. The default.
2. **Lyrique**: O + 8, vibrato 35 c, D1 + Palme.
3. **Creux clarinette**: C, Grave +3, D1.
4. **Gambe cordes**: G + g (3), vibrato, D1 + Résonance.
5. **Nasillard basson**: N + g, octave −1, D1.
6. **Souffle**: O + S (bal 40%), D1 + Résonance.
7. **Tutti**: T, D1 + Métallique.
8. **Halo métallique**: O + 8, D3 dominant.
9. **Ruban glissando**: O, Bague, span ±2 oct, Palme.
10. **Quarts de ton**: O, −¼/+¼ ready on the drawer page, Key only.

## 7. MIDI

| Input | Use |
|---|---|
| Note on/off | pitch (and gate in Note-gated mode), velocity env |
| Pitch bend | ring (Bague) or manual vibrato (Clavier) |
| CC1 mod wheel | vibrato depth, or key source |
| Channel aftertouch | vibrato depth, or key source |
| CC11 expression / CC2 breath | key source |
| CC64 sustain | hold last pitch and gate (lets the right hand leave the key) |
| CC 20-35, NRPN | taken by the wrapper (Q-Links, parameters). The engine never reads them |

## 8. Repo, build and tests

```
mpc-vst-ondes/
  vst/vst.json  vst/params.json  vst/params.lock  vst/layout.conf  vst/presets.json  vst/art/
  src/ondes.c (mpc_engine) src/osc.c src/key.c src/diffuseurs.c
  tests/spectra.c  tools/calibrate.py
  docs/DESIGN.md (this)  research/ (ignored)
  LICENSE (GPL-3.0-or-later or MIT, our choice: all-original DSP, nothing vendored)
```

- Build: `tools/build_port.sh vst/vst.json` (armv7). Add `aarch64` later for Gen2 if wanted.
- `tools/test_port.sh vst/vst.json` must print PASSED (ASan).
- **Port-specific offline tests (`tests/spectra.c`), against the Loriod chart:**
  - O at A3: every harmonic ≥ 2 below −35 dB (−60 with F).
  - 8: odd harmonics below −50 dB; first partial at 2f.
  - C and G: even harmonics below −50 dB.
  - N: duty follows the fixed-width rule across C2 to C7.
  - Aliasing: N and G at C8, nothing that isn't a harmonic above −70 dB below 18 kHz.
  - Transposers: each one and all six together hit the expected cents within ±0.5.
  - Ring: a bend sweep has no steps larger than 1 cent after slew.
  - Palme: a note on a string pitch class rings at least 10 dB louder 1 s after release than a quarter tone off.
  - Key only mode: no output without key input.
- Bench on the Key 61 (`tools/bench.sh`) with Tutti and every speaker on.
- Catalog: `release.py --repo <owner>/mpc-vst-ondes --license <SPDX>` then `catalog_check.py --catalog` OK, then a
  catalog PR.

## 9. Spikes and open questions

- **S0 — answered 2026-11-26 (device, Key 61, MPC OS 3.10.1; mpc-vst-plugins `docs/NOTES.md`):** everything arrives, on
  channel 1. Pitch bend is real 14-bit (0-16383, about 170 messages a second), the mod wheel is CC 1, the expression pedal
  CC 11, sustain CC 64, aftertouch is channel pressure (no poly AT). The touch strip in a bend mode sends the same 14-bit bend
  as an **absolute position that snaps back to the centre on release**; in a mod mode it sends CC 1; some modes send nothing.
  Consequence for M2: the ring needs a **latch** (a jump back to exactly the centre is the finger lifting, so hold the last
  pitch, as the real ring stays where it is left), and `ring_slew_ms` should cover the ~6 ms message spacing.
  Original question: what MIDI reaches a VST2 instrument on the Key 61? Pitch bend, channel aftertouch, CC1, CC11 (pedal
  jack), CC64, and what the **touch strip** sends (pitch bend or a CC, configurable?). Nothing in `docs/NOTES.md` covers
  this yet. Use a probe engine that logs `midi()` bytes. The ring and intensity-key design depend on it. Record the result
  in NOTES.md with the date.
- **Q1:** read `research/JB4-wave-shapers.pdf`, `JB3-switching.pdf`, `JB1-osc-1979.pdf` for the real values: C clip
  level, N pulse width, g filter cutoffs per slider step, 8 slider law, F filter, and how T is mixed. These replace the
  placeholders in 3.2 and 3.4.
- **Q2:** intensity-key curve data: the Sorbonne paper (HAL hal-01587642) blocks automated fetches. Download it by hand
  into `research/`.
- **Q3:** "g also changes N": does the g filter process N when both are on (topology), or does it only add to it? JB3/JB4
  will tell. Until then: g filters only G.
- **Q4: name.** The catalog bars other makers' names in the plugin name. "Ondes Martenot-style" can go in the summary, not
  the name. Candidates: *Bague*, *Ruban*, *Tiroir*, *Ondes Nouvelles*.
- **Q5:** reference audio for tuning by ear: commercial recordings (Loriod's Messiaen and Jolivet), video demos. Listen
  only; no samples go into the plugin or the repo.

## 10. Milestones

1. **M1 Core (offline):** oscillator, 8 timbres, Note/Souffle, intensity key (all modes), F/Grave/Aigu, Principal only,
   auto-layout skin. `test_port.sh` and `tests/spectra.c` green. Spike S0 on the Key 61 in parallel.
2. **M2 Playing:** ring (with latch), vibrato, transposers, octave, sustain, from S0's findings. **Done offline
   2026-11-26** (`tests/pitch.c`). Still to do: first Key 61 install and a playing test; Q1 calibration.
3. **M3 Speakers:** Résonance, Palme, speaker balance and spread. Bench on the Key 61.
4. **M4 Finish:** Métallique, skin artwork (4 pages, tile), presets, `tested.json`, release and catalog PR.
5. **v2 ideas:** Poly 4 (one speaker bank shared by the voices), MPE/poly-aftertouch per-note key, aarch64 build, an
   MPC Sample page.
