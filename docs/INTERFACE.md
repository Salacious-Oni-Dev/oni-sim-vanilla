# The interface

How the game and its `SimDLL.dll` talk to each other, and what this library does with each part
of that conversation. The game loads the library by P/Invoke (`[DllImport("SimDLL")]` in its
managed code) and calls a handful of exports; almost everything goes through one of them,
`SIM_HandleMessage`.

The layouts of the game's message structs are the game's own, so this repository does not
reproduce them. The first build generates them from your installed game into `abi/sim_abi.h`
(see `tools/gen_sim_abi.py`), with a size check on every struct; read that header for the exact
fields.

Implementation: `sim/simdll.cpp`. Checked by `driver/exports` (the export table) and
`driver/diffsim` (everything else).

## Exports

The game's library exports 32 functions. This library exports the same 32, plus two read-only
debug exports the offline tools use.

| export | what it does here |
|---|---|
| `SIM_Initialize(callback)` | creates the simulation and remembers the callback (below) |
| `SIM_Shutdown()` | stops the frame worker and destroys the simulation |
| `SIM_HandleMessage(id, length, payload)` | every message; see [Messages](#messages). Returns a pointer for the few messages that answer, otherwise null |
| `SIM_HandleMessages(id, length, count, payload)` | `count` messages of one id and size, back to back; handled one at a time |
| `SIM_BeginSave(&size, x, y)` / `SIM_EndSave()` | the save blob; see [SAVE-FORMAT.md](SAVE-FORMAT.md) |
| `SIM_DebugCrash()` | writes through a null pointer, as the game's does, to test crash handling |
| `SYSINFO_Acquire()` / `SYSINFO_Release()` | an information string (`"oni-sim-vanilla"`), and a no-op |
| `ConduitTemperatureManager_*` (seven) | pipe contents' heat exchange; see [CONDUITS.md](CONDUITS.md) |
| `kprofiler_*`, `kprofile_record_string` (sixteen) | the event profiler; see [KPROFILER.md](KPROFILER.md) |
| `SIM_DebugRandomState()` | this library only: the post-process random stream's position |
| `SIM_DebugLedger(out, count)` | this library only: the mass ledger; see [LEDGERS.md](LEDGERS.md) |

P/Invoke binds lazily, so an export the game declares but never calls still has to exist: a
missing one costs nothing until something calls it, and then throws. `driver/exports` checks
that every export of the game's library is present here and that every one can be resolved and
called.

## The callback

`SIM_Initialize` takes a function the library can call back into the game with. The game
handles two ids on it, an exception handler (0) and a report message (1), and sends both to its
crash reporter (`KCrashReporter.ReportSimDLLCrash`). This library calls it with id 0, carrying
one line of text where the game expects a call stack, in two kinds of case: when it refuses a
setup message (a rejected element or germ table, a rejected `SimData_InitializeFromCells`, a
grid resize, which is not implemented, or `Start` before a grid exists), and when the frame
timer that `ToggleProfiler` starts reports. All real per-frame data goes through
`GameDataUpdate`, not through the callback.

## What the game reads: `GameDataUpdate`

`Start` and `PrepareGameData` return a pointer to a `GameDataUpdate`, and the game binds its
managed `Grid` arrays straight to the arrays it points at: element, temperature, mass,
properties, insulation, strength, radiation, germ type and count, and accumulated flow, one entry
per cell, plus the five property textures the renderer reads (flow, liquid, liquid data,
material data, sunlight). It re-reads every pointer from a fresh `GameDataUpdate` on every
frame, so the library may move them between frames.

The same struct carries about thirty event lists, each a count and a pointer, refreshed every
frame: cells whose element or solidity changed, spawned ore, falling liquid, unstable cells,
melted cells, callbacks for messages that asked for one, consumed and emitted mass, building
temperatures, and more.

`elementIdx` holds **one element per cell**. That array, not the cell size, is the game's "one
element per tile" rule. How the simulation fills all of this is [PROJECTION.md](PROJECTION.md);
when it is filled, relative to the game's frame, is [THREADING.md](THREADING.md).

## Messages

A message is an id (a hash, `SimMessageHashes` in the game's code), a length and a payload.
Most payloads are one of the game's fixed-size structs; the setup messages are written field by
field and vary in length. This library handles each id in one of three ways.

**Immediate** messages take effect inside the call, after waiting for any frame in flight:

| message | what it does |
|---|---|
| `Elements_CreateTable`, `Disease_CreateTable` | upload the element and germ tables. Cells refer to elements by index into this table and to saves by hash |
| `AllocateCells` | sizes an empty grid; the path a loaded save takes ([SAVE-FORMAT.md](SAVE-FORMAT.md)) |
| `SimData_InitializeFromCells` | sizes the grid and fills it from the cells sent; the path worldgen takes, and the one that seeds the random stream |
| `Load` | loads a save blob into the grid ([SAVE-FORMAT.md](SAVE-FORMAT.md)) |
| `ClearUnoccupiedCells` | part of the load sequence |
| `DefineWorldOffsets`, `SimData_ResizeAndInitializeVacuumCells` | the cluster's worlds inside the one grid ([CLUSTER.md](CLUSTER.md)) |
| `Start` | starts the simulation and returns the first `GameDataUpdate` |
| `SimFrameManager_NewGameFrame` | the elapsed time and the active regions for the next frame |
| `PrepareGameData` | the per-tick call: hands over the visibility mask, returns the finished frame and starts the next ([THREADING.md](THREADING.md)) |
| `ToggleProfiler` | a debug message; this library starts or stops a frame timer of its own, which reports through the callback |

**Queued** messages are copied onto a queue and applied at the start of the next frame, in a
fixed category order rather than the order they were sent ([DRAIN.md](DRAIN.md)):

| messages | covered in |
|---|---|
| `ModifyCell` | [CELLMOD.md](CELLMOD.md) |
| `ModifyCellEnergy`, `SetInsulationValue`, `SetStrengthValue`, `ChangeCellProperties`, `Dig`, `ModifyBackwallData` | [DRAIN.md](DRAIN.md), [BACKWALL.md](BACKWALL.md) |
| `MassConsumption`, `MassEmission`, `ConsumeDisease`, `CellDiseaseModification` | [EMITTERS.md](EMITTERS.md), [DISEASE.md](DISEASE.md) |
| `CellRadiationModification`, `RadiationParamsModification` | [RADIATION.md](RADIATION.md) |
| `SetDebugProperties` | the two scales the building and chunk exchanges multiply every transfer by ([PHYSICS.md](PHYSICS.md)) |
| `Add`/`Modify`/`Remove` for building heat exchange, building-to-building contact and `ModifyBuildingEnergy` | [PHYSICS.md](PHYSICS.md) |
| `Add`/`Move`/`Remove` element chunk, `SetElementChunkData`, `ModifyElementChunkEnergy`, `ModifyChunkTemperatureAdjuster` | [CHUNKS.md](CHUNKS.md) |
| `Add`/`Modify`/`Remove` element consumer, element emitter and disease emitter, `SetElementConsumerData` | [EMITTERS.md](EMITTERS.md) |
| `Add`/`Modify`/`Remove` radiation emitter | [RADIATION.md](RADIATION.md) |

A queued message therefore takes effect on the tick **after** it is sent. A caller that sends a
message and reads the grid back in the same tick reads the old value.

**Everything else** is counted and dropped. That includes `SetWorldZones`,
`ModifyCellWorldZone` and `SimData_FreeCells`, which change nothing the simulation computes,
and the ids the game declares but never sends (`Elements_CreateInteractions`,
`AddDiseaseConsumer`, `ModifyDiseaseConsumer`, `RemoveDiseaseConsumer`, `RadiationSickness`,
`SetVisibleCells`).

## Message lengths

Do not infer a record count from a message's length. Some of the game's setup messages report
the capacity of the buffer they were written into rather than the bytes written, so the length
can be larger than the payload: `SimData_InitializeFromCells` by a fixed few bytes, and
`Elements_CreateTable` by about double. Read the header, take the count from it, and ignore
anything after the records.

## The boot sequences

A new world (worldgen, and the settling run worldgen does before saving):

```
SIM_Initialize
Elements_CreateTable
Disease_CreateTable
SimData_InitializeFromCells
Start
then, every tick: SimFrameManager_NewGameFrame, PrepareGameData
```

A loaded save:

```
SIM_Initialize
Elements_CreateTable
AllocateCells
Disease_CreateTable
ClearUnoccupiedCells
Load                      once per world
Start
then, every tick: SimFrameManager_NewGameFrame, PrepareGameData
```

The other messages arrive between ticks, as the game's buildings, germs, chunks and emitters
change.
