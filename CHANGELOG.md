# Changelog

Notable changes to this repository, newest first. This library is frozen: after the first
release it receives only fixes for differences from the game's own `SimDLL.dll`.

## 0.1.0-alpha.1 (2026-10-01)

First public release. Supported game build: 744825.

- A replacement `SimDLL.dll` that accepts every message the game sends to its own library and
  publishes the same data back. It exports every function the game's library exports, plus
  two read-only debug exports the offline tools use (`SIM_DebugRandomState` and
  `SIM_DebugLedger`), and nothing a mod could use to change the simulation.
- One deliberate difference from the game's library: a building next to a cell with no heat
  capacity skips that cell instead of turning both temperatures into NaN.
- Documentation of every part of the library in `docs/`: the interface and every message, the
  message drain, the physics kernels, `ModifyCell`, emitters and consumers, element chunks,
  disease, radiation, conduits, cluster messages, the backwall, projection, the save format,
  threading, measured quirks, the mass ledger and KProfiler.
- The simulation frame runs on a worker thread of its own, overlapped with the game's managed
  code and rendering, as the game's own library does.
- The game's in-library event profiler, KProfiler, with the game's capture format and HTTP
  control routes on `127.0.0.1`. It also records the simulation's own kernels.
  `tools/kprofile2chrome.py` converts a capture for Chrome's trace viewer.
- The build reads the game's message layouts from an installed copy and checks the size of
  every one, so a game update that changes a layout fails the build.
- Offline tools in `driver/` that run the library with no game process (`diffsim`,
  `worldgen_test`, `exports`, `kproftest`, `bench` and others), and a passthrough library in
  `shim/` that records the game's messages into a test corpus.
- `sim\build.ps1` builds the library on Windows in PowerShell, without WSL or bash. Releases
  are built with `sim/build.sh`; a Windows build is a development build (see the README).
