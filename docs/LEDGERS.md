# The mass ledger

The simulation keeps a set of books for mass: a list of cumulative counters, one per place in
the code where mass crosses the boundary of what the grid holds. Subtracting everything the
counters explain from the actual total leaves the **drift**: whatever nothing accounts for.
Drift is the check; the counters exist to be subtracted.

The ledger does not compare against the game's own library, so it keeps working where a
cell-by-cell comparison cannot, such as a live game with no reference to compare with. The game's
library has nothing like it; it is this library's own instrument and changes nothing the
simulation computes.

Implementation: `World::Ledger` in `sim/world.h`, charged by the `World::Note*` functions. Read
through `SIM_DebugLedger`. Checked by `diffsim --ledger`.

## How the check works

```
drift = (total_now - net_now) - (total_then - net_then)
```

`net` is everything the counters say came in minus everything they say went out. The starting
total cancels, so only the unexplained part is left.

`diffsim --ledger` prints each scenario's drift and marks it `UNEXPLAINED` when it exceeds
**1e-6 of the grid's total mass**. The mark is a report, not a failure: it does not change the
exit code.
Transfers are single-precision while the counters accumulate in double, so the books do not
balance to the last bit and zero is the wrong bar.

Three rules make the counters trustworthy:

- **One counter per call site, not per cause.** A single "something was deleted" counter would
  balance the books by construction and prove nothing. Split by site, a counter that moves in a
  scenario with no business moving it is a finding in itself.
- **Charge what the cell actually changed by, not what the code meant to move.** Every charge is
  measured across the write, before and after. `(a + b) - a` is not `b` once the two differ
  enough to round.
- **A counter that counts a deletion is not part of the sum.** Adding it would cancel the
  deletion and report conservation exactly where there is none.

The check is off by default in `diffsim`, because reading the total walks the whole grid once
per tick.

## Reading it

```c
int SIM_DebugLedger(double* out, int count);   // kg
```

It fills up to `count` doubles and returns the number of fields the library knows about. The
field order is a contract: fields are appended, never reordered. `kLedgerFields` and
`LedgerNet` in `driver/src/diffsim.cpp` are the other half of it and show exactly how each field
enters the sum. The export waits for the frame worker (see [THREADING.md](THREADING.md)), so it
can be called at any time.

A file named `sim_digest.on` in the working directory makes the library append one line per
frame to `sim_digest.log`, beside the marker, with a digest of the published world, the grid total, the net, the drift, and
the counters that moved that frame. It is meant for a live game, where nothing else checks
conservation.

## The fields

Field 0 is the grid's total mass. Every other field is a cumulative counter in kg.

| # | field | site | meaning | in the sum |
|---|---|---|---|---|
| 0 | `grid` | | total mass of every game cell | total |
| 1 | `emitted` | `ApplyMassEmission` | the game added mass | + |
| 2 | `modified` | `ModifyCell` ([CELLMOD.md](CELLMOD.md)) | the game overwrote a cell (signed) | + |
| 3 | `consumed` | `ApplyMassConsumption` | the game removed mass | − |
| 4 | `dug` | `ApplyDig` | a cell's contents became an entity | − |
| 5 | `ore` | state change | transition ore, reported as `SpawnOreInfo` | − |
| 6 | `unstable` | post-process | a falling solid, handed to the game or dropped | − |
| 7 | `sublimated` | sublimation and off-gassing | the share lost to `sublimateEfficiency` | − |
| 8 | `wisp` | post-process | gas under 0.001 kg, deleted where it stands | − |
| 9 | `thinliq` | post-process | liquid under 0.01 kg, deleted likewise | − |
| 10 | `cleared` | `ClearCell` | reached a cell that still held mass | − |
| 11 | `compconsumed` | element consumers | a consumer component drained cells | − |
| 12 | `compemitted` | element emitters | an emitter component filled a cell | + |
| 13 | `emitore` | element emitters, solid branch | ore **created** for the game; see below | excluded |

- **`emitore` is outside the balance.** A solid element emitter puts nothing into the grid and
  takes nothing out. It makes a lump of ore out of mass the building holds outside the
  simulation and hands it straight to the game ([EMITTERS.md](EMITTERS.md)). The counter
  exists so that "the simulation created this much ore" can be seen.
- Several of the deletions (`wisp`, `thinliq`, the share lost to sublimation efficiency, a
  falling solid with nowhere to go, a `ModifyCell` add with nowhere to go) are the game's own
  behaviour and are reproduced, not fixed. The ledger makes them visible; it does not remove
  them.

## What it does not count

Run over the whole suite, the ledger closes on every scenario except five. Each of the five
matches the game's library in `diffsim`, so these are gaps in the books, not in the simulation:

- **Opening a world** (`vacrect`). `SimData_ResizeAndInitializeVacuumCells` overwrites a
  rectangle and its border ring, and no counter records what left or what arrived
  ([CLUSTER.md](CLUSTER.md)).
- **A liquid cell absorbing another whole** (`germliq`, `liqvgate`, `sunliquid`). The liquid
  mover's same-liquid merge (`DisplaceLiquidDirectional`) moves the source's mass into the
  destination and then clears the source. Clearing charges `cleared`, as mass leaving the grid,
  but no counter records the destination's gain, so the books show mass appearing.
- **`rain`** shows 80 kg leaving that no counter records. Its cause has not been traced.

There is no energy ledger in this library. Where the game's own physics does not conserve
energy (phase change carries temperature rather than energy across a transition, the conduction
clamp, sublimation, deletions), [PHYSICS.md](PHYSICS.md) and [QUIRKS.md](QUIRKS.md) describe the
behaviour.
