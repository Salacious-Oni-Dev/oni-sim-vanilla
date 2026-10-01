# Offline test tools

These tools drive the SimDLL with no game process running. They are cross-compiled for
Windows x64 with mingw-w64. On WSL they run directly through Windows interop; on other Linux
systems, run them with Wine.

```sh
cd driver
ONI_GAME=/path/to/OxygenNotIncluded ./build.sh   # ONI_GAME is only needed for the first build
```

The first build generates `abi/sim_abi.h` from your own game install (see `tools/gen_sim_abi.py`).

## The tools

| tool | needs | what it checks |
|---|---|---|
| `diffsim` | a corpus, the game's `SimDLL.dll`, a built `sim/build/SimDLL.dll` | Loads the game's SimDLL and this one into one process, seeds both identically, steps them in lockstep and compares every game-visible array after every tick. It also cross-loads save blobs in both directions. This is the main gate. |
| `worldgen_test` | a corpus, a SimDLL | Checks the world-load path step by step: where a blob lands, how each cell is normalised, and the load-time phase transition. It must pass against the game's DLL as well as this one. |
| `exports` | a built SimDLL, the game's `SimDLL.dll` | Checks that this library exports every function the game's DLL exports, and that every export resolves and can be called. |
| `kproftest` | a SimDLL | Runs one scripted capture through the sixteen profiler exports and prints every record, so the game's DLL and this one can be diffed line for line. See `docs/KPROFILER.md`. |
| `bench` | a corpus | Times the sim's kernels on recorded worlds, digests the resulting world state, and runs the game's own per-cell validity check over it. Built with the same optimisation flags as the DLL. |
| `replay`, `experiments`, `savefmt`, `driver` | a corpus | Smaller investigation tools: replay a recorded boot, measure individual behaviours, inspect the save-blob format (`docs/SAVE-FORMAT.md`), run a hand-built world. |

A tool that needs arguments prints its usage when run without them.

Tools that load the game's own SimDLL look for it as `SimDLL_orig.dll` in the current
directory. Copy it there from `<install>/OxygenNotIncluded_Data/Plugins/x86_64/SimDLL.dll`, or
pass its path: `--klei <path>` for `diffsim` and `exports`, `--dll <path>` for `worldgen_test`,
`replay`, `experiments` and `savefmt`, and as the first argument for `driver` and `kproftest`.

## Recording a corpus

Most tools replay a *corpus*: the real messages the game sent to its SimDLL, recorded
from your own game. The passthrough shim in `shim/` records one:

1. Build it with `shim/build.sh`.
2. In `<install>/OxygenNotIncluded_Data/Plugins/x86_64/`, rename the game's `SimDLL.dll` to
   `SimDLL_orig.dll` and put the built shim there as `SimDLL.dll`.
3. Start the game and load a save as the first thing you do; do not start a new game first.
   The shim records the first simulation session, and a new game's first session is world
   generation. The shim forwards every call to the original and writes `sim_corpus.bin` beside
   it, containing the complete boot sequence plus one sample of every message seen after it.
4. Put the original `SimDLL.dll` back.
5. Move `sim_corpus.bin` out of the folder, and delete `sim_notes.log`, `sim_shim.log` and
   `sim_timing.log`.

Record with no mods installed. A corpus recorded with mods contains messages a stock game
never sends.

## Running the gate

```sh
./build/diffsim.exe --corpus sim_corpus.bin --scenario all > out.txt
```

Each scenario prints its result, and any divergence beyond the scenario's envelope prints a
`FAILED` line saying what diverged. The run ends with `done`, or with `FAILURES` and
exit code 1.

The game's DLL runs its frame on a thread of its own, so a small part of the output (frame and
spin counts) depends on thread scheduling and can differ from run to run with no change to
either DLL. A `FAILED` line is a real divergence; a changed frame count alone is not. See
`docs/THREADING.md`.

This repository pins no golden output. Results depend on the corpus, and a corpus is recorded
from your own game, so take a reference run of your own before changing anything and compare
against it afterwards.

## The game's own validity check

The game carries a per-cell validator, the `SimCheckErrorMap` overlay in its debug tools. It
classifies every cell by whether its element, mass and temperature are consistent with one
another. `driver/src/simcheck.h` is that check, run offline:

- **`bench`** classifies the world each scenario leaves behind, prints the counts, and fails
  on a vacuum cell that holds a temperature or a mass. It needs no game DLL.
- **`diffsim`** classifies both published worlds, and fails when this library's world has a
  fault where the game's does not.

This catches what a comparison cannot: a NaN that both sims produce, or one that has been
stable since the code was written, compares equal and digests the same every run.

## A pitfall

`diffsim` loads a prebuilt `sim/build/SimDLL.dll`. After editing `sim/*.h`, rebuild the DLL
with `sim/build.sh`, or pass `--mine <dll>`, before trusting a `diffsim` result. Otherwise it
tests the DLL that was already there, and passes.
