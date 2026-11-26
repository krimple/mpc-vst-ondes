#!/usr/bin/env bash
# Offline engine checks with the host compiler, under ASan/UBSan:
#   tests/spectra.c  timbres against Loriod's chart, aliasing, intensity key (M1)
#   tests/pitch.c    transposers, octave, bend, glide, ring and latch, vibrato (M2)
#   tests/voices.c   Mono, Legato and Poly voice modes
#   tests/presets.c  the PRESET field: built-in programs and saved .xpl presets
#   MPC_VST=<mpc-vst-plugins checkout> tests/run.sh      (default: ../mpc-vst-plugins next to this repo)
set -euo pipefail
cd "$(dirname "$0")/.."
MV="${MPC_VST:-../mpc-vst-plugins}"
python3 "$MV/tools/gen_vst.py" vst/vst.json --params-h >/dev/null
CC="${CC:-cc}"
status=0
for t in spectra pitch voices presets; do
  echo "== $t"
  $CC -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra \
    -Isrc -Ivst/build -I"$MV/wrapper" "tests/$t.c" src/ondes.c src/osc.c -lm -lpthread -o "vst/build/$t"
  "vst/build/$t" || status=1
done
exit $status
