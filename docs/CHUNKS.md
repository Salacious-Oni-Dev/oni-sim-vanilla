# Element chunks

A chunk is a lump of matter the game holds **outside** the grid: a dropped rock, the contents of
a storage bin, ore on a conveyor, a suit in a dock. It has a temperature, a heat capacity and the
one cell it sits in. Once a substep it exchanges heat with that cell and, if the tile below is
solid, with that tile too. That is the whole component. It has no mass in the grid, never moves
on its own, and never triggers a state transition in the cell it warms.

Chunk messages are the busiest component traffic the game sends.

Implementation: `sim/chunks.h`, drained in `sim/simdll.cpp`. Checked by `driver/diffsim`
scenarios `chunkhot`, `chunkvac`, `chunkadj`, `chunkmelt`, `chunkenergy`, `chunkchurn` and
`chunkins`, all bit-exact against the game's own library over 50 ticks, cells and published
arrays alike. `diffsim --chunks` prints the per-chunk comparison.

## Where it runs

The components run once per substep, inside the region loop, after the fluid and disease
kernels, in this order:

1. element consumers
2. element emitters
3. radiation emitters
4. **element chunks**
5. building-to-cell exchange
6. building-to-building exchange
7. disease emitters

## Data

Per chunk, four fields are derived at registration rather than sent:

- `heat_capacity = element.specificHeatCapacity * mass`;
- `thermal_conductivity`, `low_temp` and `high_temp` come from the element table. The message
  has no say in them: a chunk's conductivity **is** its material's, where a building's is a
  multiplier on its material's;
- `max_energy_transfer_scale_factor = surfaceArea / thickness`. The message sends both; only the
  ratio is used;
- `cell` is stored as the padded index.

`SetElementChunkData` overwrites the temperature and heat capacity only.
`ModifyElementChunkEnergy` adds `deltaKJ / heat_capacity` to the temperature, floored at 0 K,
and is refused for a chunk with no heat capacity.

## The two-body solver

`CalculateTemperatureExchange` is shared by the chunk paths, and it is the one place in the
simulation that works in **double**. The arguments arrive as floats, are widened, and only the
three results are rounded back. Reproducing it in float gives different results.

```
equilibrium = (T_a*HC_a + T_b*HC_b) / (HC_a + HC_b)
reach       = (T_b - T_a) * scale
q           = min( (T_b - T_a) * conductivity * dt,   reach * HC_a,   reach * HC_b )
newT_a      = min( T_a + q/HC_a, equilibrium )
newT_b      = max( T_b - q/HC_b, equilibrium )
energy      = (T_a - (float)newT_a) * HC_a
```

- The transfer is the **smallest of three** quantities: what conduction can carry in `dt`, and
  what it would take to move each body to the other's temperature. The conduction term wins
  ties.
- Both results are clamped at the pair's equilibrium, so one substep can equalise a pair but
  never overshoot. On NaN the clamps return the equilibrium.
- `energy` is computed from the **already-rounded float** `newT_a`, not from the double. It is a
  kJ figure the game accumulates, and computing it from the double drifts.

The wrapper always passes the colder body as `a`, swapping the pair and mirroring the result
when needed. Algebraically this is a no-op; in the last bits it is not, so it is kept.

## Exchange with the cell

Four gates come before any arithmetic, and each one is a behaviour:

- a **massless cell** exchanges nothing and reports nothing;
- a chunk outside `0..10000 K` is **refused, not clamped**, and the exchange returns zero;
- a cell outside the same range is refused the same way;
- a cell whose element has the `TemperatureInsulated` state bit (`0x10`) refuses outright. This
  is what keeps a chunk in an insulated tile from heating it.

Then:

```
k    = min( chunk.thermal_conductivity,  insulation^2 / 255^2 * cell_element.thermalConductivity )
cond = k * transfer_scale * chunk.max_energy_transfer_scale_factor * 0.001
CalculateTemperatureExchange(1.0, cond, dt, cellT, cellMass * cellSHC, chunkT, chunkHC)
```

The **minimum**, not the product: the shape cell-to-cell conduction uses when insulation is
involved, and the opposite of what buildings do. The 0.001 is a constant of the chunk path, not
the building transfer scale `SetDebugProperties` carries, so the game can scale building
transfer but not chunk transfer. The multiply order is left to right, in float. `chunkins`
makes both arms of the `min` fire.

**No transition test follows.** The cell's new temperature is written and nothing else. A chunk
can heat the water it sits in past boiling, and the cell does not become steam until another
kernel looks at it. Buildings test for a transition in their own sweep; chunks do not.

### The ground

If the cell **below** the chunk's cell is solid, the chunk exchanges a second time against it,
with `transfer_scale = ground_transfer_scale`. This is why a hot rock on a metal tile cools
faster than one in mid-air. `chunkvac` separates the two: a chunk in vacuum over vacuum does
not change at all, and one in vacuum over a single granite tile sees only the ground transfer.

## The adjuster

`ModifyChunkTemperatureAdjuster` installs a fictitious second body that the chunk exchanges with
**instead of** the world: how the game drives a chunk's temperature directly. A chunk whose
adjuster has positive heat capacity does not touch the grid and reports no energy. The
adjuster's own new temperature is discarded, because it is a source rather than a body. Setting
its heat capacity to zero retires it; `chunkadj` does that mid-run.

## The region test

Building components test their extents. A chunk is tested by its **own cell**, in padded
coordinates, **inclusive at both ends**, and it must also have positive heat capacity:

```
px = cell % width;  py = cell / width;
px >= region.minX && py >= region.minY && px <= region.maxX && py <= region.maxY
```

A chunk inside two overlapping regions exchanges twice; one inside none does not exchange, but
still reports its temperature and still announces melting.

## Published arrays

**`elementChunkInfos`** is indexed by **handle slot**, not appended. Each substep sizes it to the
slot count and writes each live chunk into its own slot, so a chunk that did not exchange keeps
what it last reported. `temperature` is assigned; `deltaKJ` is **accumulated** across the
frame's substeps.

**`deltaKJ` is reset by publication, not by the frame.** When the frame is handed to the game,
the list is copied out and every `deltaKJ` behind it is zeroed. So the game sees one frame's
energy, however many substeps the frame ran.

On a tick that runs no frame (or a frame with no elapsed time), the list is republished with
each chunk's current temperature and `deltaKJ` set to zero.

**`elementChunkMeltedInfos`** is a bare handle per chunk outside its range by **three kelvin**:

```
temperature >= high_temp + 3  ||  temperature < low_temp - 3
```

The test sits outside the region and heat-capacity gates, and it is **level-triggered**: a chunk
is re-announced every substep for as long as it stays out of range.

## Messages

Chunk messages are drained **by type**, not in arrival order:

```
Add  ->  Move  ->  SetData  ->  Energy  ->  Adjuster  ->  Remove
```

A chunk registered and moved in one frame starts where the move puts it. A chunk moved and then
removed in one frame never exchanges from its new cell.

Every `Add` reports the handle it allocated through `componentStateChangedMessages`, and every
`Remove` reports `-1` on the same channel; that is how the game learns a handle is dead. Neither
is reported when `callbackIdx` is -1.

Like every queued message, a chunk message takes effect on the tick **after** it is sent (see
[DRAIN.md](DRAIN.md)). `chunkenergy` measures it twice: energy sent before tick 3 first moves
the chunk at tick 4, and a withdrawal sent before tick 6 first moves it at tick 7.

## Comparing it

On the frame a chunk first appears, the game's library publishes an uninitialised `deltaKJ` for
the new slot; it is uninitialised data in the shipped game, and it cannot be reproduced.
`diffsim` therefore skips the `deltaKJ` comparison on any tick where `numElementChunkInfos`
changed, and compares it on every other tick. `temperature` is compared throughout.

## Limits

- `AddElementChunkMessage`'s two padding fields are ignored.
- The game library's diagnostic logging for out-of-range temperatures is not reproduced; the
  zero return is.
- The `0x100000` slot ceiling on handle validity is not modelled.
- The "frame with no elapsed time" arm of the list republish is implemented but not reached by
  any scenario, and the "no frame" arm has only been reached before any chunk exists.
