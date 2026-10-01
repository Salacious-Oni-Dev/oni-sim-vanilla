# The message drain

Everything the game tells the simulation arrives through `SIM_HandleMessage`, and a cell message
does not take effect at the point it is sent. This document covers what happens to a message
between the game handing it over and the grid changing, and the exact rules of the small
handlers that have no document of their own.

Implementation: `DrainQueue`, `QueueDeferredMessage` and the `Apply*` handlers in
`sim/simdll.cpp`; the cell-energy arithmetic is `ApplyCellEnergy` there.
Checked by `driver/diffsim` scenarios `msgorder`, `cellenergy`, `cellenergyphase` and
`setinsul`.

## Three rules

- A queued message takes effect on the **tick after** it is sent. `SIM_HandleMessage` copies it
  onto a queue owned by the game thread; the queue is handed to the simulation at the next
  `PrepareGameData` and drained at the start of that frame. [CHUNKS.md](CHUNKS.md) has the
  consequence for element chunks.
- Messages are drained in **category order**, not arrival order.
- Every category the game's library drains is drained here, including `ModifyCellEnergy`, the
  third busiest message the game sends.

## Category order

The game's library does not keep one queue. It sorts each message into a per-category list and
then walks the lists in a fixed order. This simulation drains in the same order:

```
SetInsulationValue
SetStrengthValue
ModifyCellEnergy
ChangeCellProperties
MassConsumption
MassEmission
ConsumeDisease
CellDiseaseModification
CellRadiationModification
RadiationParamsModification
Dig
ModifyCell
ModifyBackwallData
then the components: buildings, element chunks, element emitters and consumers, radiation emitters
```

So a `ModifyCellEnergy` always lands **before** a `ModifyCell` sent in the same frame, whichever
order the game sent them in, and a `ModifyCell` lands after everything else that touches a
cell. In live play the game sends thousands of messages a frame across a dozen categories, so
this is the ordinary case, not a corner case. The `msgorder` scenario sends a `ModifyCell` and a
`ModifyCellEnergy` at one cell, in both orders, and fails by hundreds of kelvin if the drain
runs in arrival order.

`ModifyBackwallData` is drained after `ModifyCell`. Nothing else in the frame reads the
backwall, so its position cannot change a result.

Draining costs one linear pass over the frame's messages per category. Sorting on arrival would
be cheaper to drain but would move the work onto the game's thread, inside `SIM_HandleMessage`.

## `ModifyCellEnergy`

This is how everything on the game side that heats or cools a cell without moving mass reaches
the grid: a running machine's waste heat, a duplicant's body heat, a radiant pipe, a thermo
regulator.

```c
struct ModifyCellEnergyMessage { int32_t cellIdx; float kilojoules; float maxTemperature; int32_t id; };
```

`id` identifies the sender for error messages. Nothing in the simulation reads it.

The message is refused, with no effect, when:

- the cell's element is Vacuum (`element.state & 3 == 0`). The phase is tested before the mass,
  so energy sent into empty space is discarded;
- the cell's mass is not strictly above 0.001 kg;
- `maxTemperature` is not strictly above 0. Zero does not mean "no limit"; it means "do
  nothing".

Otherwise the new temperature is:

```
ceiling = max(T, maxTemperature)
next    = min(T + kilojoules / (mass * specificHeatCapacity), ceiling)
```

The message can only push a cell towards the ceiling from below. A cell already hotter than the
ceiling it is handed stays where it is; it is not cooled down to it. Negative kilojoules have no
floor of their own.

A result outside `(0, 10000]` is clamped into `[1, 10000]`. If the result is still not
positive, the write is skipped.

After the write, the cell is given its **state transition inside the drain** (`DoStateTransition`),
before any kernel of the frame has looked at it. A cell the message boils is already steam when
the first substep runs, and conducts as steam for that substep. A transition taken here still
reports its spawned ore in the frame's `spawnOreInfo`, because the frame's event lists are
cleared before the drain, not after it.

Energy that a refusal discards is simply gone: the game's library keeps no record of it.

The `cellenergy` scenario covers the ceiling from below and from above, `maxTemperature = 0`,
cooling, the Vacuum refusal, the mass refusal, the 10000 K clamp and two messages at one cell in
one frame. `cellenergyphase` covers transitions taken in the drain, including one that spawns
ore; it runs with conduction on, because with conduction off a transition in the drain and one
in the phase-change pass land in the same place.

## `SetInsulationValue` and `SetStrengthValue`

Both write one byte per cell, by **truncation toward zero into the low byte**, with no rounding
and no clamp. Only insulation is scaled:

```
insulation[cell] = (uint8_t)(int32_t)(value * 255.0f)
strength[cell]   = (uint8_t)(int32_t)(value)
```

So an insulation of 0.5 stores 127, not 128; 1.5 stores 126 (382 in the low byte), not 255; and
-0.5 stores 129 (-127 in the low byte), not 0. The `setinsul` scenario sends those values.

Strength is published in `GameDataUpdate.strengthInfo` and is read by the liquid pressure-break
test (see [PHYSICS.md](PHYSICS.md)).

## Messages that are not handled

`SetWorldZones`, `ModifyCellWorldZone` and `SimData_FreeCells` are accepted and ignored. Any
message id the simulation does not recognise is counted and dropped; it never reaches the
grid.

The element emitters and consumers (`MassConsumption`, `MassEmission` and the emitter and
consumer components) are documented in [EMITTERS.md](EMITTERS.md), and `ModifyCell` in
[CELLMOD.md](CELLMOD.md).
