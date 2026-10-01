#!/usr/bin/env bash
# Cross-compile the offline tools to Windows x64. They run directly from WSL via
# binfmt interop, so no Windows shell is needed to use them.
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p build

# abi/sim_abi.h describes the game's own message structs, so it is generated from the player's
# install rather than distributed. Set ONI_GAME to the game directory for the first build.
if [ ! -f ../abi/sim_abi.h ]; then
  : "${ONI_GAME:?abi/sim_abi.h is missing: set ONI_GAME to your Oxygen Not Included install directory}"
  python3 ../tools/gen_sim_abi.py "$ONI_GAME" -o ../abi/sim_abi.h
fi


FLAGS=(-O1 -std=c++17 -static-libgcc -static-libstdc++
       -Wall -Wextra -Wno-cast-function-type)

x86_64-w64-mingw32-g++ "${FLAGS[@]}" -o build/driver.exe src/driver.cpp
x86_64-w64-mingw32-g++ "${FLAGS[@]}" -o build/replay.exe src/replay.cpp
x86_64-w64-mingw32-g++ "${FLAGS[@]}" -o build/experiments.exe src/experiments.cpp
x86_64-w64-mingw32-g++ "${FLAGS[@]}" -o build/savefmt.exe src/savefmt.cpp
x86_64-w64-mingw32-g++ "${FLAGS[@]}" -o build/diffsim.exe src/diffsim.cpp
x86_64-w64-mingw32-g++ "${FLAGS[@]}" -o build/exports.exe src/exports.cpp
x86_64-w64-mingw32-g++ "${FLAGS[@]}" -o build/worldgen_test.exe src/worldgen_test.cpp
# Winsock: kproftest drives the kprofiler HTTP listener and runs a /datablock sink of its own.
x86_64-w64-mingw32-g++ "${FLAGS[@]}" -o build/kproftest.exe src/kproftest.cpp -lws2_32

# bench compiles the sim's kernels into itself, so it must use the flags sim/build.sh uses
# — timing -O1 code would measure something that never ships, and changing them here alone
# makes bench measure a binary the game will never run. sim/build.sh documents why each of
# these three is there.
SIMFLAGS=(-O3 -march=x86-64-v2 -ffp-contract=off)
x86_64-w64-mingw32-g++ "${SIMFLAGS[@]}" -std=c++17 -static-libgcc -static-libstdc++ \
  -Wall -Wextra -o build/bench.exe src/bench.cpp

echo "built build/driver.exe build/replay.exe build/experiments.exe build/savefmt.exe build/diffsim.exe build/exports.exe build/worldgen_test.exe build/kproftest.exe build/bench.exe"
