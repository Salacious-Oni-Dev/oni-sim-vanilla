#!/usr/bin/env bash
# Cross-compile the passthrough shim to a Windows x64 DLL.
#
# Install: rename the game's SimDLL.dll to SimDLL_orig.dll, then drop the built
# SimDLL.dll next to it in
#   <install>/OxygenNotIncluded_Data/Plugins/x86_64/
# The shim writes sim_shim.log into that same directory.
set -euo pipefail

cd "$(dirname "$0")"
out=build
mkdir -p "$out"

x86_64-w64-mingw32-g++ \
  -shared -O2 -std=c++17 \
  -static-libgcc -static-libstdc++ \
  -Wall -Wextra -Wno-cast-function-type \
  -o "$out/SimDLL.dll" \
  src/shim.cpp \
  -Wl,--kill-at,--enable-stdcall-fixup

echo "built $out/SimDLL.dll"
# `awk NR<=30` and not `head -30`, and the difference is this script's EXIT STATUS. objdump
# emits about 2,500 lines here; `head` closes the pipe after thirty of them, objdump takes
# SIGPIPE, and under `set -o pipefail` this pipeline — the last command in the file — makes the
# script exit 141 with the DLL sitting there built correctly. It is a race on whether objdump is
# still writing when head leaves, so it fires perhaps once in fifty runs: invisible by hand, and
# exactly the kind of intermittent red that makes a build step stop being believed. awk drains
# its input to EOF, so there is no signal to race with.
x86_64-w64-mingw32-objdump -p "$out/SimDLL.dll" |
  sed -n '/\[Ordinal\/Name Pointer\] Table/,$p' | awk 'NR <= 30'
