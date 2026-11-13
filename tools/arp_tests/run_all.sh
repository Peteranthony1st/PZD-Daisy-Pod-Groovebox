#!/bin/bash
# The arp test suite: the real Arpeggiator, MidiSequencer and TempoClock
# (with main.cpp's glue copied into harness.cpp), built for this computer
# and driven by 64-sample audio blocks and a 2 kHz control ISR. Run after
# any arp, sequencer or clock change:   tools/arp_tests/run_all.sh
# harness.cpp copies main.cpp's ArpTick()/SeqNote()/KeyOn() -- keep them
# in step when those change.
set -u
cd "$(dirname "$0")"
SRC=../../src
OUT=$(mktemp -d)
mkdir -p "$OUT/tc"
# tempo_clock.cpp includes itcm.h from its own folder: build a copy beside
# a stub that drops the ITCM/DTCM placement.
cp "$SRC/tempo_clock.cpp" "$SRC/tempo_clock.h" "$OUT/tc/"
printf '#pragma once\n#define DSY_ITCM_TEXT\n#define DSY_DTCM_BSS\n' > "$OUT/tc/itcm.h"
B=(-std=c++17 -O1 -include host_stubs.h -I"$OUT/tc" -I"$SRC" -I. harness.cpp "$SRC/arpeggiator.cpp" "$SRC/sequencer.cpp" "$OUT/tc/tempo_clock.cpp")
fail=0
for t in t_restart t_switch t_suspects t_timing t_loops2 t_phrase t_inversions t_changes t_race; do
  c++ "${B[@]}" $t.cpp -o "$OUT/$t" || { echo "BUILD FAILED: $t"; fail=1; continue; }
  echo "== $t"; "$OUT/$t" > "$OUT/$t.out"; rc=$?; grep -vE "^   [A-G]" "$OUT/$t.out"
  [ $rc -ne 0 ] && { echo "FAILED: $t"; fail=1; }
done
for seed in 1 2; do
  for t in t_render t_print; do
    c++ "${B[@]}" -DSEED=$seed $t.cpp -o "$OUT/$t" || { echo "BUILD FAILED: $t"; fail=1; }
    "$OUT/$t" > "$OUT/r.out"; rc=$?; echo "$t seed $seed: $(tail -1 "$OUT/r.out")"
    [ $rc -ne 0 ] && { echo "FAILED: $t"; fail=1; }
  done
done
for seed in 1 2; do
  c++ "${B[@]}" -DSEED=$seed t_load.cpp -o "$OUT/t_load" || { echo "BUILD FAILED"; fail=1; }
  echo "loads seed $seed: $("$OUT/t_load" | tail -1)"
done
for seed in 1 2 3; do
  c++ "${B[@]}" -DSEED=$seed t_restarts.cpp -o "$OUT/t_restarts" || { echo "BUILD FAILED"; fail=1; }
  echo "restarts seed $seed: $("$OUT/t_restarts" | tail -1)"
done
for seed in 1 2 3 4 5 6; do
  c++ "${B[@]}" -g -DSEED=$seed -fsanitize=undefined,bounds -fno-sanitize-recover=all t_fuzz.cpp -o "$OUT/t_fuzz" || { echo "BUILD FAILED"; fail=1; }
  echo "fuzz seed $seed: $("$OUT/t_fuzz" 2>&1 | grep -E 'two notes|left sounding|runtime error' | sed 's/   on-on.*//' | tr '\n' ' ')"
done
rm -rf "$OUT"
exit $fail
