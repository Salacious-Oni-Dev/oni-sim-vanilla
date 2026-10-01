# Element and disease emitters and consumers

These components are how matter and germs cross the boundary between a building and the grid.
A pump is an element consumer; a vent, a geyser and a plant are element emitters. The two
messages `MassConsumption` and `MassEmission` are the one-shot, message-driven forms of the
same machinery. A disease emitter is the only thing in the simulation that creates germs, and
`ConsumeDisease` is how the game takes them away.

Implementation: `sim/emitters.h`; the message handlers (`ApplyMassConsumption`,
`ApplyMassEmission`, `ApplyConsumeDisease`) and registration are in `sim/simdll.cpp`. Checked by
`driver/diffsim` scenarios `econsume`, `econsumeempty`, `eemit`, `eemitdisp`, `eemitsolid`,
`massmsg`, `germemit`, `germcons`, `germthresh` and `germgas`.

Within a substep, the components run in this order: element consumers, element emitters,
radiation emitters, element chunks, building exchange, building-to-building exchange, disease
emitters. Each tests its **own cell** against the region, inclusive at both ends.

## The flood

Everything here is built on one breadth-first flood. It walks out from a cell, at most `maxDepth`
steps, refusing the phases it is told to refuse, and calls a function on each cell it reaches;
the function can stop the flood.

- **The queue is FIFO and the push order is up, left, right, down.** That decides which cell an
  emitter fills and which cell a consumer drains first when several are equally good.
- **The depth test comes first.** A node at the limit is dropped without being examined.
- **Coordinates are 16-bit.** A step left from `x == 0` wraps to 65535 and is refused by the
  width test.

`GetReachableCells` floods while skipping solids and collects every cell it reaches.
`FloodRemoved` floods and drains as it goes, and the phases it passes through are **derived from
the element being removed**: removing a solid floods through solids only; removing anything else
floods through everything except solids.

## Element consumers

Once a substep, for each consumer whose cell is in the region:

1. `amount = dt * consumptionRate`.
2. Choose the element by the `configuration` byte:
   - `0`: the element the consumer was registered with;
   - `1` (`AllLiquid`) and `2` (`AllGas`): flood, then take the first reachable cell of that
     state, starting at `offsetIdx` and wrapping, so a pump in a mixed room does not always
     sample the same cell first;
   - anything else: nothing happens, and `offsetIdx` does not advance.
3. Drain with `FloodRemoved` from the consumer's cell, `maxDepth` steps.
4. If anything was taken, report it in `consumedMassInfo` under the consumer's handle.
5. Advance `offsetIdx` by one.

A consumer is registered with a rate of zero and does nothing until a `SetElementConsumerData`
gives it one. That message sets the cell and the rate only.

The state test for `AllLiquid`/`AllGas` is `(state & 3 & wanted) == wanted`, which also accepts a
solid. It is harmless only because `GetReachableCells` has already discarded solid cells.

### Removing mass from a cell

The per-cell drain, shared by consumers and both message forms:

- germs removed are `(int)(count * (take / massBefore))`: truncated, against the mass before
  the subtraction;
- the report's temperature is a running mass-weighted mix of every cell drained:

  ```
  total = info.mass + take
  mix   = (info.mass * T_info + T_cell * take) / total
  mix   = min(mix, hi); mix = max(mix, lo)        // hi, lo: the two input temperatures
  ```

  The clamp uses `min`/`max` semantics that return the bound when the mix is NaN. That matters:
  a drain that reaches a cell holding the right element and no mass computes `0 / 0`, and the
  clamp is what turns it back into a real temperature. `econsumeempty` covers it;
- the germ merge into the report uses the same rule as merging germs into a cell (see
  [DISEASE.md](DISEASE.md)).

## Element emitters

Once a substep, for each emitter whose cell is in the region:

```
if (emitInterval <= elapsedTime) {
    if the report slot is free for this element:
        reachable = GetReachableCells(cell, maxDepth)
        room = any reachable cell with mass < maxPressure
        if room != (blockedState == 0): flip blockedState, fire the matching callback
        TryEmit(...)
    elapsedTime -= emitInterval          // a carry, not a reset
}
elapsedTime += dt
```

- **The interval is a carry.** A frame that runs past several intervals subtracts one and emits
  once, so an emitter never catches up with itself.
- **A registered emitter never fires.** Registration sets `emitInterval` to `FLT_MAX` and
  `emitMass` to zero; the emitter is inert until a `ModifyElementEmitter` arrives.
- **`blockedState` starts at `0xff`**, neither of its two values, so the first update always
  fires one of the two callbacks. `ModifyElementEmitter` resets the clock and the offset, and
  sets `blockedState` back to `0xff` when the new `emitMass` is not positive.
- An `emitTemperature` below zero means the element's default temperature.

`TryEmit` walks the reachable list from the emitter's `offsetIdx`, takes the first cell with
`mass < maxPressure`, and branches on the phase of the **emitted** element:

- **Vacuum:** refused.
- **Gas:** a different gas in the way is moved with `DisplaceGas`; if that fails, the cell is
  skipped and the walk continues.
- **Liquid:** `DisplaceLiquid` first; if that fails, `DisplaceGas` on whatever the cell holds
  after the failed attempt.
- **Solid:** see below.

The emission mixes temperature by mass (`CalculateCombinedTemperature`), adds the mass, merges
the germs, and announces a substance change **only when the cell was Vacuum**. An emitter topping
up its own gas is not a substance change. The report carries the emitter's mass and temperature,
not what the cell gained.

### A solid emitter creates ore, not cells

The solid branch puts nothing in the grid. It records a `SpawnOreInfo` for the cell and carries
on walking, so it can drop ore in **every** reachable cell in one substep, and its
`emittedMassInfo` slot stays empty. The ore is produced only for cells visible to the player,
unless the game is in debug editing. The mass is created: nothing leaves the grid. It is counted
in the mass ledger as `emitore`, outside the balance (see [LEDGERS.md](LEDGERS.md)).

## Report lists

- `consumedMassInfo` is appended during the frame and handed over whole; the simulation's copy
  is then cleared.
- `emittedMassInfo` is **indexed by handle slot**. When the frame is handed over, each slot's
  element is set to Vacuum and its mass and temperature to zero; its disease fields carry over.
  On a tick with no frame, every slot is set to `0xffff` and zeroes instead.

## The spawn-ore list is sorted and merged

When the frame is published, `spawnOreInfo` is **sorted** by cell, then element, and entries for
the same cell and element are **merged**: masses summed, temperature combined with the cooler
entry passed first. The disease fields of the first entry of each run are kept; the rest are
discarded. This applies to ore from every source, phase-change ore included, so the order of the
list is a property of the world, not of whichever kernel filled it. `digInfo` is not sorted.

## `MassConsumption`

Not a single-cell operation. The message carries a `radius` and a `height`:

- `height == 0` floods outward `radius` steps, with the same phase rule as `FloodRemoved`;
- `height != 0` walks a `radius x height` rectangle whose **top-left corner** is the message's
  cell, with no flood and no phase test.

The callback reports the mass-weighted temperature of everything drained, and the merged germs.

## `MassEmission`

A gas emission into a cell holding a different gas displaces it with `DisplaceGas`; a liquid
emission uses `DisplaceLiquid`. The emission goes ahead if the cell held the same element,
Vacuum, or was cleared. A solid in the way, or a displacement with nowhere to go, is a refusal,
and a refusal reports **nothing**: element `0xffff`, zero mass and temperature, and
`suceeded = 0`.

Both message callbacks are reported when `callbackIdx != -1`.

## Disease emitters

`AddDiseaseEmitter` registers an emitter with no cell and no disease, so it does nothing until a
`ModifyDiseaseEmitter` sets its cell, disease, range, count and interval. Unlike the element
emitter's, this `Modify` does **not** reset the clock.

Once a substep, for each emitter with a disease whose cell is in the region:

- the clock advances only while the emitter is in the region, so an emitter outside it does not
  build up a backlog;
- it fires **once** per update however far behind it is: `elapsed -= interval`, then
  `elapsed += dt` whether it fired or not;
- the reach is `GetReachableCells`, so germs do not pass through solids;
- a positive `emitCount` is added to every reachable cell;
- a **negative** `emitCount` is a removal, and touches only reachable cells that already carry
  the emitter's disease.

`diseaseEmittedInfos` is indexed by handle slot, and **the simulation never reports an
emission in it**: every slot reads `{0xFF, 0}`. This matches the game's library.

## `ConsumeDisease`

There is no disease-consumer component. Consumption is a per-frame message, drained immediately
before `CellDiseaseModification` (see [DRAIN.md](DRAIN.md)). Each message names a cell, a
callback, a fraction and a cap:

- the amount is `(int)(count * percentToConsume + 0.5)`, **rounded**, then capped at
  `maxToConsume`;
- if fewer than one germ is left, all of the cell's disease fields are cleared;
- the callback reports the disease index the cell has **afterwards** (so `0xFF` both when the
  cell was already empty and when this call emptied it) and the amount taken. It goes to
  `diseaseConsumptionCallbacks`.

## Limits

- The rotation of `offsetIdx` is implemented but not separately observed: making it visible
  needs a pocket of several gas cells, which brings in the game's random gas shuffle.
- The solid emitter's visibility gate is not covered; the harness sends an all-visible mask.
- An emitter whose reach extends into a second region emits there, because only its own cell is
  tested against the region. No scenario covers this.
