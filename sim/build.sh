#!/usr/bin/env bash
# Build the replacement SimDLL. This does NOT install it over the game — the harness in
# driver/ loads it by path, which is the only thing that should be running it until the
# physics exists.
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p build

# abi/sim_abi.h describes the game's own message structs, so it is generated from the player's
# install rather than distributed. Set ONI_GAME to the game directory for the first build.
if [ ! -f ../abi/sim_abi.h ]; then
  : "${ONI_GAME:?abi/sim_abi.h is missing: set ONI_GAME to your Oxygen Not Included install directory}"
  python3 ../tools/gen_sim_abi.py "$ONI_GAME" -o ../abi/sim_abi.h
fi


# Optimisation flags, chosen by measurement: five round-robined rounds of
# `bench --scenario all --ticks 150`, min of the per-run minimums (run-to-run spread on this
# machine is 5-8%, so anything smaller is not a result):
#
#   asteroid  substep 5.627 -> 5.169 ms (-8.1%)   frame 7.548 -> 6.845 ms (-9.3%)
#   granite   substep 0.936 -> 0.931 ms (-0.5%)   frame 1.383 -> 1.181 ms (-14.6%)
#
#   -O3                  inlining and scheduling, not vectorisation — GCC emits zero packed FP
#                        arithmetic here at any -O level, so there is no SIMD to gain or lose.
#                        Project -51.7% on granite, StepPostProcess -24.9% and StepFlow -16.0%
#                        on asteroid.
#   -march=x86-64-v2     for exactly one instruction: SSE4.1's blendv, which turns a branch in
#                        StepGasPressure's inner loop into a branchless select (-16.0% on that
#                        kernel). v2 is a 2008-era baseline.
#   -ffp-contract=off    A GUARD, not an optimisation. It is a no-op at v2, which has no FMA.
#                        At v3 or -march=native GCC contracts a*b+c into an FMA and the rounding
#                        moves. Proved on purpose here: a v3 build WITHOUT this flag fails three
#                        scenarios (worst 39.608765 K against a 0.0100 K envelope) and differs
#                        on 297 transcript lines. Anyone widening -march must keep this flag.
#
# Two kernels get slower and that is bought, not free: StepLiquidDisplacement +22 to +27% and
# StepGasDisplacement +8 to +20%, together about +0.065 ms against -0.70 ms on the frame.
#
# The whole diffsim suite is byte-identical across the change: 98 scenarios, 0 FAILED, the same
# 634-line normalised transcript and the same md5.
x86_64-w64-mingw32-g++ -O3 -march=x86-64-v2 -ffp-contract=off -std=c++17 -shared \
  -static-libgcc -static-libstdc++ \
  -Wall -Wextra \
  -o build/SimDLL.dll simdll.cpp \
  -Wl,--out-implib,build/libSimDLL.a \
  -lws2_32

echo "built sim/build/SimDLL.dll"
