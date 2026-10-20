#!/usr/bin/env bash
# Walks through the whole toolchain on the bundled examples.
set -e
cd "$(dirname "$0")"
M=./miniptx
[ -x $M ] || ./build.sh
mkdir -p out

echo "== 1. compile -> PTX text (with optimizer report) =="
$M compile examples/vecadd.kc -o out/vecadd.ptx --stats
cat out/vecadd.ptx

echo; echo "== 2. assemble -> relocatable object, 3. link -> executable =="
$M asm out/vecadd.ptx -o out/vecadd.mpo
$M link out/vecadd.mpo -o out/vecadd.mpx
$M objdump out/vecadd.mpx | head -30

echo; echo "== 4. run on the SIMT simulator (3 blocks x 50 threads, n=130) =="
$M run out/vecadd.mpx --grid 3 --block 50 --arg buf:f32:130:iota --arg buf:f32:130:ones --arg buf:f32:130:zeros --arg i32:130 --stats

echo; echo "== 5. optimizer: -O0 vs -O1 on fold_demo.kc =="
$M compile examples/fold_demo.kc -O0 --stats -o out/fold0.ptx
$M compile examples/fold_demo.kc -O1 --stats -o out/fold1.ptx

echo; echo "== 6. multi-module link (relocations): mathlib.kc + use_mathlib.kc =="
$M build examples/mathlib.kc examples/use_mathlib.kc -o out/apply.mpx
$M run out/apply.mpx --grid 1 --block 8 --arg buf:f32:8:iota --arg buf:f32:8:zeros --arg i32:8

echo; echo "== 7. warp divergence: collatz =="
$M build examples/collatz.kc -o out/collatz.mpx
$M run out/collatz.mpx --grid 1 --block 32 --arg buf:i32:32:zeros --arg i32:32 --print 32 --stats

echo; echo "== 8. debugger (scripted) =="
$M build examples/saxpy.kc -O0 -o out/saxpy.mpx
printf 'break 6\nrun\nlocals\nthread 3\np i\nstep\nlocals\nquit\n' | \
  $M run out/saxpy.mpx --grid 1 --block 40 --arg f32:2 --arg buf:f32:40:iota --arg buf:f32:40:ones --arg i32:36 --debug --print 4
