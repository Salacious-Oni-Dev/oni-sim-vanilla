# Physics

The cell kernels: conduction, gas flow, liquid flow, the post-process pass and its random
stream, state changes, sublimation, and the building heat exchange. Every rule here is the game's
own simulation reproduced, and each is checked by running both libraries side by side.

Implementation: `sim/physics.h` (cell kernels), `sim/buildings.h` (building heat exchange),
`sim/cellmod.h` (the displacement primitives shared with `ModifyCell`). Checked by
`driver/diffsim`, which runs the game's SimDLL and this one on the same world and message stream
and compares every published array after every tick.

## The frame

One frame (`RunFrame` in `sim/simdll.cpp`):

```
ClearFrameEvents
DrainQueue                 applies the messages of the tick BEFORE this one (DRAIN.md)
StepPhysics(elapsed)       runs 0..N substeps, below
UpdateBackwallTransitions  (BACKWALL.md)
Project                    what the game will see (PROJECTION.md)
FillPropertyTextures
PromoteWorldOffsets        (CLUSTER.md)
BuildUpdate
```

`StepPhysics` derives a substep count from the elapsed time (next section) and runs the body
that many times. Inside a substep the body runs **once per region**:

```
per region:
   StepConduction
   StepStateChange
   StepGasPressure
   StepGasDisplacement        off the SAME snapshot as StepGasPressure
   StepFlow                   the liquid mover
   StepLiquidDisplacement     off the SAME snapshot as StepFlow
   StepDiseaseDiffusion       only while some cell holds germs (DISEASE.md)
   StepRadiationField         (RADIATION.md)
   StepElementConsumers       (EMITTERS.md)
   StepElementEmitters
   StepRadiationEmitters
   StepElementChunks          (CHUNKS.md)
   StepBuildingHeatExchange
   StepBuildingToBuilding
   StepDiseaseEmitters
   StepPostProcess
   ZeroMasslessCells          the WHOLE grid, inside the per-region loop
   StepDiseasePostProcess     only while some cell holds germs
```

The consumers, emitters, chunks, building exchanges and disease emitters are the game's
component list, in the game's order. The disease emitter comes last, which is why it runs after
both building exchanges rather than beside the other emitters.

## Comparing against a simulation that is not deterministic

Two byte-identical copies of the game's SimDLL, given the same world and the same messages, do not
always agree with each other. The game's simulation runs on a worker thread, and two things
depend on scheduling:

- A `PrepareGameData` can find zero, one or two frames finished. When it finds two, it runs both,
  which is a second substep of every kernel in one call. `GameDataUpdate.numFramesProcessed`
  reports it.
- Whether the first frame after `Start` or `Load` runs physics varies from run to run.

So `diffsim` aligns the two libraries by frame where it matters, reports any tick where the game's
library ran more than one frame, and judges a scenario against a recorded spread where the game's
library disagrees with itself. Most scenarios need no spread: this simulation produces the same
answer on every run, and on the canonical suite it matches the game's library to the bit.

## The timestep

The simulation runs a **fixed 0.2 s substep**, and a frame runs as many as its elapsed time
covers, carrying the remainder to the next frame. Frames of 0.05 s, 0.15 s and 0.2 s give
identical results; a 0.4 s frame gives exactly what two 0.2 s frames give; at 0.3 s the count
alternates between one and two. A frame always runs at least one substep.

The first `PrepareGameData` after `Start` or `Load` runs no physics.

## The active region

`NewGameFrame` carries one or more regions (`minX, minY, maxX, maxY`), one per active world, and
**only cells inside a region are simulated**. The region is half-open, `[min, max)`, on both axes,
and a conduction pair straddling its edge exchanges nothing. The game clamps `maxY` to the
world's height minus one, so the top row of the grid is never simulated.

Two sweeps read one row further:

- **Gas pressure** reaches the row above the region as the far end of a pair, but never drives
  from it.
- **Post-process** reads the region inclusively.

The gas and liquid displacement sweeps do not use the region mask at all; they walk the region
rectangle with their own insets (see below).

Overlapping regions are each stepped in full, so cells in an overlap are stepped once per region.

## Conduction

Once per substep, each cell is paired with the cell to its right and the cell above it, in
ascending y then x. The temperatures used are those at the start of the substep.

A pair is skipped when:

- the two cells are less than **1 K** apart. This dead zone is why a block's interior stays at
  exactly its starting temperature while its edges cool, and why temperatures settle a fraction
  of a degree apart and never finish equalising;
- either cell's conductivity is not positive;
- either cell has no heat capacity (see the clamp below).

### The pair conductivity

```
f       = (insulation / 255)^2                 for each cell; 255 means not insulated
k_pair  = exp(0.5 * (log(k_b) + log(k_a)))      when both f are exactly 1
        = min(f_a * k_a, f_b * k_b)             when either f is below 1
k_pair *= SAM_a[phase of b] * SAM_b[phase of a]
```

- **The geometric mean**, computed as `exp(0.5 * (log + log))`, which is `sqrt(k_a * k_b)` to
  within the last bit and not always exactly.
- **Insulation changes the formula, not just the scale.** If either cell's factor is below 1, the
  mean is replaced by the minimum of the two scaled conductivities. One insulated cell drags the
  pair down to its weaker side. The factor is the square of the byte: an insulation of half
  passes a quarter of the heat.
- **Both cells' surface-area multipliers apply**, each chosen by the phase of the cell on the
  other side, multiplied together before being applied. Oxygen against granite runs at 25x
  (oxygen's solid multiplier); water against water at 25 x 25 = 625x.
- **No mass, area or distance term.** Mass decides only how far the temperature moves.

### The exchange

The pair is ordered so the cooler cell comes first, and the exchange is computed in **double**:

```
Teq  = (Tc*Cc + Th*Ch) / (Cc + Ch)
q    = min( dT*k*dt, (dT*0.25)*Cc, (dT*0.25)*Ch )      dT = Th - Tc,  dt = 0.2
Tc'  = min( Tc + q/Cc, Teq )
Th'  = max( Th + (-1.0/Ch)*q, Teq )
```

- **The quarter clamp:** one pair can move a cell by at most a quarter of the difference. One
  kilogram of oxygen beside a single 400 K granite cell lands on exactly 325 K; with four hot
  neighbours it lands on exactly 400 K.
- **The equilibrium clamp** stops a pair from crossing over.
- The spelling matters to the last bit: `(dT*0.25)*C`, not `dT*(0.25*C)`; three separate
  candidates in the minimum; a reciprocal and a multiply on the hot side.

The change against the start temperature is then **added to the live grid**, so a cell that has
already conducted with one neighbour keeps both contributions. Finally both cells are clamped to
**[1 K, 10000 K]**, even if the pair exchanged nothing, provided both have heat capacity. See
[QUIRKS.md](QUIRKS.md).

### Every mixing transfer clamps

Every transfer that adds mass to a cell (the liquid mover, gas displacement, sublimation, the
`ModifyCell` add paths) goes through one function that mixes temperatures by mass and then clamps
the result between the two input temperatures. Without the clamp, mixing two cells at the same
temperature does not always return that temperature in float.

## Gas pressure

The first of the two gas sweeps, over the whole region, against a snapshot of the grid taken just
before it.

```
dm = flow * (m_src - m_dst)        clamped to [-0.125 * m_dst, +0.125 * m_src]
```

`flow` is the element's (oxygen 0.12). The 0.125 cap stops a heavy cell emptying itself into a
hole.

- **Gravity plays no part.** Vertical and horizontal pairs behave identically, and a heavier gas
  on a lighter one does not sink.
- **Three neighbours per cell**, not four: `cell + hdir`, the cell above, and the upper diagonal
  `cell + width + hdir`. `hdir` is a signed 1 negated at the start of every substep; it is both
  the order columns are walked in and the horizontal neighbour paired with. Every orthogonal pair
  is still visited once; the two upward diagonals alternate by substep. A diagonal is skipped
  when the two cells around its corner, taken together, include a solid or are a gas/liquid
  pinch.
- **Direction comes from the sign of the transfer**, not from which cell is being visited.
- **A pair is two cells of the same gas, or a gas and vacuum.** Two different gases never
  exchange here.
- **A cell filled from empty is closed for the rest of the substep.** Every gate compares the
  cell's live element with its snapshot element, and an emptied destination takes the source's
  element the moment it receives. So a hole gets exactly one donor per substep, and a cell whose
  element changed earlier in the frame takes no part.
- A destination holding **Void** is zeroed rather than filled: the mass is destroyed.
- Germs: the moved fraction of the source's mass carries the same fraction of its germs. See
  [DISEASE.md](DISEASE.md).

## Gas displacement

The second gas sweep, off the **same snapshot**, and the only thing that moves two different gases
past each other. It works on a run of three cells in a line: source, destination, and the cell
beyond. The destination's whole contents are pushed into the cell beyond, the destination is
cleared, and the source moves in behind it:

```
moved = min(live.mass[src], 0.125 * snapshot.mass[src])
```

Directions are tried in the order **down, `-hdir`, `+hdir`, up**, with columns in ascending x.

Gates, in order:

1. the source is a gas;
2. the destination is a gas, of a **different** element;
3. the destination is not gas-impermeable;
4. `snapshot.mass[src] > snapshot.mass[dst]`, strictly, so equal pressures never move;
5. both cells still hold their snapshot elements, and the destination still has mass;
6. the cell beyond holds the destination's element or Vacuum, and is not gas-impermeable.

The cell beyond gets the usual clamped mass-weighted mix; the destination takes the source's
**snapshot temperature** unmixed, because it was emptied first. Germs: the evicted gas takes all
its germs into the cell beyond; then one eighth of the source's starting count follows the source.

The sweep is inset two cells from each side of the region. Its starting row is bounded with `min`
where `max` is meant, so on a full-grid region it starts at row 0; see [QUIRKS.md](QUIRKS.md).

### One element per cell, in its purest form

Oxygen at 2 kg beside carbon dioxide at 1 kg, sealed, never moves: the pressure sweep refuses
two different gases, and the displacement sweep has no cell beyond to push into. Put one empty
cell between them and it **oscillates forever**, flipping element every substep, 0.25 kg at a
time (0.125 x 2 kg).

## Liquid flow

One sweep, bottom-up and left to right, one turn per liquid cell. A turn tries **down, left,
right, up** and ends the moment the cell swaps.

### Two grids

- **Everything that decides how much to move is read from the snapshot**: the destination's mass,
  phase and properties. The source's own mass is read once and decremented as the cell spends,
  so a cell spends as it goes (500 kg pushing both ways gives 125 kg left, then 93.75 kg right),
  but never sees mass pushed into it during the sweep.
- **Everything that decides whether a transfer lands is read live.** That is the mover.

### The mover

Every transfer, in every direction, goes through one function. Against the destination's
**live** element:

1. the same element as the source: add the mass, done;
2. Void: the transfer is destroyed (see Limits);
3. a different liquid: refused;
4. a gas: displaced with `DisplaceGas`, refused if that fails;
5. vacuum, or a gas just displaced: the destination takes the source's element and the mass, and
   is announced.

A refusal costs the source nothing and does not end its turn. A transfer is priced and checked
against the element's minimum flow **before** the mover is called, so a transfer too small to
happen displaces nothing.

### Horizontal

```
dm = min(0.25 * (m_src - m_dst), viscosity)       skipped if dm < minHorizontalFlow
```

The 0.25 is a constant, not an element property. `minHorizontalFlow` is a minimum on the
transfer, not on the difference.

### Vertical

```
down:  dm = min(viscosity, m_src, 0.5 * max(0, max(m_src * 1.01, maxMass) - m_below))
       skipped if m_src > minVerticalFlow and dm < minVerticalFlow
up:    c  = max(m_above * 1.01, maxMass)
       dm = 0.5 * max(0, m_src - c), capped as down unless m_src >= 2c; skipped below 0.01
```

The downward law is a single expression, not a "pour into room, else push by pressure" branch;
the two differ exactly where settled pools live, just under `maxMass`. **The 1.01 is
load-bearing**: a settled column holds 1 % more in each cell than in the one above, which is why
the bottom cell of a water column sits near 1010 kg.

Up, left and right share four gates: a destination with a different element must be vacuum or gas;
not solid in the live grid; not liquid-impermeable; and it counts as empty unless it holds this
liquid. Down has its own: the cell below must not be solid in the live grid or liquid-impermeable.

### Falling through gas

Liquid with vacuum or gas below does not pour fractionally; it **swaps whole cells**, one cell
per substep, at any mass. Which cells swap is decided by `IsLiquidPermeable` (false for liquid or
solid) against the live grid:

```
if  permeable(left) and not permeable(down-left):  hand over as falling liquid
elif not permeable(right):                         swap with the cell below
elif permeable(down-right):                        swap with the cell below
else:                                              hand over as falling liquid
```

That is why a sheet of water over gas drops alternate columns on alternate substeps.

**Falling liquid** is handed to the game as a `spawnFallingLiquidInfo` record carrying the
cell's starting mass, temperature and germs, and the cell is cleared. The hand-off is refused
when the simulation is headless, when the cell below is liquid-impermeable, when the cell is not a
game cell, when the player cannot see it (unless debug editing), or when the temperature is
outside `[lowTemp - 3, highTemp + 3]`. A refused downward hand-off leaves the cell where it is for
the substep. Sideways, a slice pushed over an edge (the cell below the source is solid, the cell
below the destination is permeable) is offered as a falling-liquid record; if refused, the slice
goes through the ordinary mover.

The visibility mask a frame reads is **two `PrepareGameData` calls old**, as in the game's
library, so nothing is handed over in a world's first frames.

### Displaced gas: `DisplaceGas`

Liquid moving into a gas cell pushes the gas into a neighbour. The cell must hold mass and be a
gas. Six candidates, first acceptable wins:

| scan | order | accepted if |
|---|---|---|
| orthogonal | up, left, right, down, starting at `substep % 4` | same gas or the Vacuum element, and not gas-impermeable |
| upward diagonals | up-left, up-right, starting at `substep % 2` | same gas, the cell between is not solid, and not gas-impermeable |

`substep` is a counter that advances once per substep. "Vacuum" means the Vacuum element, not a
cell with no mass. On success the gas, with its temperature and all its germs, is added to the
candidate, the source is cleared, and both cells are announced.

## Liquid displacement

The second liquid sweep, off the **same snapshot** as the first, and the only thing that moves two
different liquids past each other. It runs on three-cell lines (source, destination, beyond) in
three directions: `-hdir`, `+hdir` and **up**. There is no downward direction.

Gates:

1. the destination is not liquid-impermeable;
2. its snapshot element is a liquid different from the source's (the source's element also comes
   from the snapshot);
3. both cells still hold their snapshot elements;
4. `snapshot.mass[beyond] + snapshot.mass[dst] < available`, where `available` is the source's
   snapshot mass less the four flow-accumulator slots it has written this substep. This is the
   only place the flow accumulator is read back as state;
5. for **up** only: the source's live mass is at least its element's `maxMass`;
6. `DisplaceLiquidDirectional` clears the destination.

Then **one eighth of `available`** moves into the destination at the source's snapshot
temperature, with one eighth of the source's starting germ count.

`DisplaceLiquidDirectional` empties a cell into the next one along: into gas (displaced with
`DisplaceGas`, then the two cells swap whole), or into the same liquid (merged, germs and all).
It refuses vacuum, a different liquid and a solid, which is what stops a three-liquid sandwich
from cycling forever.

The sweep is inset three cells from each side, with the same `min`/`max` slip on its starting
row as the gas displacement sweep.

## Post-process

One pass per region per substep, after the components, plain row-major order. Each cell takes
one turn according to its phase. This pass owns almost every draw from the world's random stream,
so its exact order of draws matters to everything after it.

### The random stream

```
state = state * 214013 + 2531011          (int32 wraparound)
r     = ((state >> 16) & 0x7FFF) * (1.0f / 32767)
```

The initial state is the world seed from `SimData_InitializeFromCells`. `AllocateCells`, the path a
loaded save takes, seeds from the wall clock, so **a loaded save does not reproduce**; only a
worldgen boot does. `SIM_DebugRandomState` reads the stream's position (this library only; the
game's library has no such export).

### A gas cell's turn

1. **The wisp gate.** Under 1e-9 kg the cell is evaporated (cleared to Vacuum at 0 K). Under
   0.001 kg it is evaporated if **any** orthogonal neighbour is a gas holding at least 1 kg, of
   any element. The mass is destroyed, not merged. Neither case ends the turn: the cell goes on
   to take its turn as the gas it was.
2. **Density displacement**, only when the cell below has the same state byte (Unstable flag
   included). One draw; it acts only when the draw exceeds 0.99. The two cells swap when the
   upper one has the larger molar mass, or when they hold the same gas and the cell below is
   hotter. A swap ends the turn.
3. **Partial melt**, unless the cell is liquid-impermeable: four calls, below, left, right, above,
   until one acts. One that acts skips the shuffle.
4. **The shuffle.** The stride `dir` is negated on every gas cell visited. One draw: only above
   0.9 does the cell try to move. Candidates `cell + dir`, `cell - dir`, the cell below, in that
   order. Each is rejected if hotter than the best so far (this test comes before its draw), then
   draws and needs above 0.5, then must be a gas, and the downward one also needs the source's
   molar mass to be strictly greater. The last survivor swaps whole with the cell. Two equal
   elements always fail the molar-mass test, which is why gases never density-sort.
5. **Sublimation** of solid neighbours (below).

About 7.5 % of gas cells start a swap each substep. Swaps are horizontal or downward, and
together with gas displacement they are how two different gases interpenetrate in the vanilla
model; at equal pressure, the shuffle is the only way.

### A liquid cell's turn

1. Under 0.01 kg the cell is evaporated and the turn ends.
2. **Density displacement**, as for gas but acting when the draw exceeds 0.30. Two liquids swap
   when the upper one has the larger molar mass. The same liquid with a hotter cell below does not
   swap: both cells get one clamped mass-weighted temperature.
3. **Over `maxMass`**: the pressure break, then the over-full displacement (below).
4. **Off-gassing** (below).

### Pressure break

A liquid cell holding more than its element's `maxMass` tests the walls around it with
`p = mass / maxMass`, below, left, right and above, until one breaks:

- the walk covers up to three cells in that direction and stops at the first non-solid;
- a solid that is unbreakable (element strength 0, or the cell's `Unbreakable` property) refuses
  the whole call;
- each wall cell adds to a resistance that starts at 1:

  ```
  term = ((m / maxMass) * hi + (1 - hi)) * strength * lo * 0.25   (+ about 0.1 when hi == 0)
  ```

  where `hi` is bit 7 of the cell's strength byte and `lo` its low seven bits (see
  [DRAIN.md](DRAIN.md) for how the byte is set);
- only walls one or two cells thick can break. If `p >= resistance`, liquid past the wall pushes
  back by its own `m / maxMass` and the test is repeated.

A break publishes one `worldDamageInfo` record per wall cell, nearest first, and ends the cell's
turn. Nothing in the grid changes; the game applies the damage, leaks 1 kg through, and destroys
the tile at full damage.

### Over-full displacement

When no wall broke, a liquid cell holding more than `1.5 * maxMass`, under a lighter liquid cell,
and holding more than `max(maxMass, 1.01 * above)`, has the cell above emptied with
`DisplaceLiquid` (see [CELLMOD.md](CELLMOD.md)), then moves `1/2.01` of itself, with the same
share of its germs, into the cell above. The turn ends.

### Off-gassing

A liquid with a `sublimateIndex` tests only the cell above, in this order: the cell above holds
less than 1.8 kg; the liquid has a sublimate target; the cell above is gas or vacuum; **a draw**;
the cell above is not gas-impermeable; a draw against `sublimateProbability`. The amount is
`offGasProbability * mass`, capped at 1 kg, taken from the cell below instead when that holds
more of the same liquid. If too little would be left, the cell converts in place. The source pays
the full amount and the gas receives `amount * sublimateEfficiency`: the difference is destroyed,
deliberately. Germs follow only when the gas cell holds none or the same disease.

### Partial melt

A gas cell beside a solid can melt 5 kg off it when all of these hold: the solid holds more than
5 kg; it is neither unbreakable nor marked to report melting; `T_gas > highTemp + 3` and
`T_solid < highTemp - 3`; the solid's high transition is a liquid; and the gas can pay,
`(T_gas - (gas.lowTemp + 6)) * C_gas >= (highTemp + 3 - T_solid) * c_liquid * 5`.

- **In play**, 5 kg of the liquid at `highTemp + 3` is handed to the game as falling liquid at the
  gas cell, with the solid's germ share; the gas cools by what it paid, and the solid loses 5 kg.
- **Headless**, or when the hand-off is refused: the gas is displaced, and the gas cell becomes
  5 kg of the liquid at `highTemp + 3`. The gas's heat is not spent on this branch.

Neither branch conserves energy.

### Sublimation

A gas or vacuum cell's turn ends by checking each solid neighbour (up, right, left, down) with a
sublimate target, only when the cell holds less than 1.8 kg, with one draw per such neighbour:

- the amount is `sublimateRate * 0.2` per free neighbour, independent of the solid's mass;
- the gas receives `amount * sublimateEfficiency`; the rest is destroyed, deliberately (it is how
  a kilogram of Oxylite becomes half a kilogram of oxygen);
- a gas of a different element in the free cell is displaced first; into the same gas the product
  merges, capped at 1.8 kg;
- the write into an emptied cell is a plain add and a temperature copy, with no mixing;
- when the solid would be left with less than one more portion, **the tile itself becomes the
  gas**, at `efficiency` of its mass;
- an emptied solid is evaporated.

Whether the free cell is treated as gas is decided by the cell's phase at the start of its turn,
not its live element.

### Unstable solids

A solid with the `Unstable` flag tests only the cell below. Supported (solid, Void, or
solid-impermeable) resets its countdown and stops. Otherwise it reads its per-cell stable-tick
countdown: a fresh one is rolled from the random stream as 3 to 6 substeps, and the cell falls
when it reaches zero. Any substance written into a cell resets its countdown, so every unstable
solid in a freshly loaded world rolls before it moves.

- **In play**, the cell is handed to the game as an `unstableCellInfo` (element, mass,
  temperature, germs) and cleared.
- **Headless**, the cell is moved down within the grid in one call until a solid stops it.

The diagonal falling-sand variant is disabled in the shipped game and is not modelled.

### Clearing massless cells

Nothing clears a cell just because a kernel emptied it. A gas cell drained to zero keeps its
element through post-process, and takes its turn (negating the shuffle stride and spending its
draws) before being cleared. Massless cells are zeroed to Vacuum **after** post-process, over the
whole grid. Doing it earlier changes the random stream.

## State changes

Once per substep, after conduction, every cell is checked against its element's range:

```
T > highTemp + 3   ->  highTempTransition,  new temperature T - 1.5
T < lowTemp  - 3   ->  lowTempTransition,   new temperature T + 1.5
```

- **The 3 K margin is fixed**, independent of the threshold, the mass and the element. After a
  transition the cell sits 1.5 K past the threshold in its new phase, half of what it would need
  to cross back, so a cell on a boundary does not flip every substep.
- **Temperature carries; energy does not.** The specific heat changes and the cell's internal
  energy changes with it. The 1.5 K nudge is all the latent heat the game's model has.
- **One transition per cell per substep.**
- Buildings and `ModifyCellEnergy` also test the cells they heat, immediately (see
  [DRAIN.md](DRAIN.md)).

**Transition ore.** Some transitions name a second element. A fraction of the mass
(`...OreMassConversion`) becomes ore at the post-transition temperature, with the same share of
the cell's germs, announced in `spawnOreInfo`, and the rest stays as the product. The ore is
produced only when the share is more than 0.001 kg.

**Small freezes become ore.** On the low branch, when the target is a solid and
`mass / target.defaultMass <= 0.8`, the whole cell is offered to the game as ore of the solid
(with all its germs) and cleared. If the game refuses (the cell is not visible, for instance) it
freezes in place. Ice and Brine Ice default to 1000 kg, so a water freeze up to 800 kg becomes
debris, not a tile.

**Condensation drops.** On the low branch, when the target is not a solid and the simulation is
not headless, the condensate is handed to the game whole as falling liquid and the cell is
cleared, provided the cell below is not liquid-impermeable, the cell is visible (or debug
editing), and the new temperature is within the target's `[lowTemp - 3, highTemp + 3]`. This is
condensation rain. Headless, the cell condenses in place.

**Melting tiles are reported.** On the high branch, a cell with the `NotifyOnMelt` property is
reported in `cellMeltedInfos`. The low branch never reports.

**Events.** `substanceChangeInfo` for the cell; `solidSubstanceChangeInfo` when either side is a
solid; `liquidChangeInfo` when either side is a liquid; `solidInfo` only when solidity actually
flipped.

**At load**, every cell already outside its range is transitioned once, without reporting melts.

## `substanceChangeInfo` is a touch list

Every other event list is derived by comparing this frame's projection with the last. This one
is not. A kernel that **writes a substance into a cell** announces the cell, whether or not the
element changed, and the element indices are filled in at publish time from the previous and
current element arrays. So a gas shuffle swapping two oxygen cells reports `oxygen -> oxygen`,
and a sealed vacuum pocket reports every cell every frame (the gas pressure sweep announces a
vacuum-state destination even when nothing moves).

The list is published sorted by cell with duplicates removed, so the order kernels touch cells
in never reaches the output. Two asymmetries are the game's and are reproduced: density
displacement announces its molar-mass swap but not its temperature swap; and the liquid
displacement's same-liquid merge announces only the cell it emptied.

Every announcement also resets the cell's unstable countdown.

## The flow texture

The per-cell flow accumulator is four floats a cell, published through the flow property
texture:

```
x = (accum[0] - accum[1]) * scale
y = (accum[3] - accum[2]) * scale         scale = 1 / max(mass, 1)
```

and zero for a cell whose element changed after the liquid section. It holds one frame's
transfers. Writers: the gas pressure sweep (horizontal and vertical pairs, both ends; the
diagonal writes nothing), the gas displacement sweep (the y pair, for every direction), the
liquid mover (one slot, the giving cell only) and the liquid displacement sweep.

## Buildings

A building is a rectangle of cells plus a body of heat outside the grid: one temperature, one heat
capacity, one conductivity. Two components carry it: building-to-cell exchange and
building-to-building exchange.

### Registration

`AddBuildingHeatExchange` derives three fields:

- `heatCapacity = mass * element.specificHeatCapacity`, and `perCellHeatCapacity` is that over the
  cell count;
- `thermalConductivity = message.thermalConductivity * element.thermalConductivity`;
- `meltTemperature = element.highTemp`: a building melts at its material's threshold.

`ModifyBuildingHeatExchange` replaces the record wholesale and keeps only the fact that it was
overheated. `ModifyBuildingEnergy` adds `deltaKJ / heatCapacity`, within the message's bounds
widened to include the current temperature; a result outside 0..10000 K is refused, not clamped.
Building messages drain by type: Add, then Modify and energy, then Remove. Handles are
`slot | (version << 24)`, and removal swaps the last entry into the gap, which changes iteration
order.

The two building-to-building message names are crossed relative to what they do; that is the
game's naming.

### Building to cells

A building is skipped unless its whole extent lies inside the region; also if its per-cell heat
capacity is not positive or its temperature is negative. Per cell under it, skipping cells with
no mass or no heat capacity:

```
Q = srcHC * (T_cell - T_building)
      * (element.thermalConductivity * building.thermalConductivity)
      * insulation^2 * (0.005 / 255^2)
      * (dt * buildingTemperatureScale)
```

- `srcHC` is the heat capacity of whichever side is **hotter**: the cell's if the cell is warmer,
  the building's per-cell capacity if the building is. That is why a machine in vacuum barely
  cools while the same machine in water sheds heat at once.
- `insulation` is the raw byte, squared.
- `buildingTemperatureScale` comes from `SetDebugProperties`, which the game sends every frame.

Both sides move by `Q` over their own heat capacity, clamped into the interval the two
temperatures spanned; if they would cross over, the pair goes to equilibrium. The cell's state
transition is checked immediately.

Then the building:

```
T' = clamp(T + sum(Q) / heatCapacity, coldest_seen, hottest_seen)
     + (dt * operatingKilowatts) / heatCapacity
```

**The operating heat is added outside the clamp.** That is how a machine heats itself above
everything it touches. A building covered by two overlapping regions exchanges, and is charged
its operating heat, twice; see [QUIRKS.md](QUIRKS.md).

Events: `buildingOverheatInfos` while at or above the overheat temperature,
`buildingNoLongerOverheatedInfos` on the substep it drops below, and `buildingMeltedInfos` while
at or above the melt temperature.

A cell with no heat capacity (Neutronium) is skipped. In the game's library it produces a NaN
building temperature; see [QUIRKS.md](QUIRKS.md).

### Building to building

The same shape, with three differences: it uses each building's whole heat capacity; there is no
insulation term, and the rate is `0.005 * buildingToBuildingTemperatureScale`; and the clamp
interval is computed once over every building in the contact group, so a chain of touching
buildings cannot pass heat beyond its hottest and coldest members. The energy is also limited to
how far both sides could move. `cellsInContact` is stored and never read.

## Verification

`diffsim` runs each scenario against the game's library for 50 ticks. Its canonical suite has no
known-bad entries. Besides cell mass, element and temperature, it compares every event list in
full and in order (`substanceChangeInfo`, `spawnOreInfo`, `unstableCellInfo`,
`spawnFallingLiquidInfo`, `cellMeltedInfos`, `worldDamageInfo` and the building events), germ
type and count on the last tick, the property textures and the random stream position
(`--draws`). Scenarios built for single kernels include:

| area | scenarios |
|---|---|
| conduction | `heatblock`, `insulated`, `equilibrium`, `boiling` |
| gas | `gaspockets`, `gasmix`, `gasroom`, `tracer`, `tracerslab`, `tracermix`, `toprow`, `tracerow`, `traceflow`, `tracetall`, `tracewall` |
| liquid | `drop`, `pair`, `pool`, `pour`, `squeeze`, `liquid`, `falling`, `liqvgate`, `hang` |
| post-process | `sand`, `sandgas`, `sublimate`, `subldisp`, `sublvac`, `offgas`, `melt`, `meltfall`, `meltcold`, `pbreak` |
| state change | `freeze`, `tilemelt` |
| germs | `germflow`, `germliq`, `germdisp`, `germsubl`, `germoffgas`, `germore` |
| buildings | `building`, `buildingrun`, `buildingcool`, `buildingvac`, `building2`, `buildinglap`, `b2bzerohc` |

Scenarios that hand matter to the game (falling liquid, unstable cells) are also run with
`--gameside`, which sends the world non-headless.

## Limits

- **The Void element in the liquid mover.** The game's mover empties a transfer into Void and
  reports success; this simulation does not model it. No scenario has one.
- **Partial heat transition.** The liquid branch's boiling twin of partial melt (a liquid beside a
  much hotter neighbour boils 5 kg) is not modelled.
- **Element interactions.** The `ElementInteraction` table is not modelled.
- **Property bit `0x10` in conduction.** The game's conduction skips a pair when either cell has
  this bit; this simulation does not test it. Nothing in the game is known to set it.
- **Diagonal falling sand** is disabled in the shipped game and not modelled.
- **The game library's one spared massless cell.** The game's library sometimes leaves one
  emptied cell per connected region holding its old temperature instead of 0 K. Which cell
  survives varies from run to run, so it is not modelled. It affects only the temperature of a
  cell with no mass.
- **Sublimation side effects.** The per-cell sublimated-mass accumulator and the sublimation
  effect id the game's library records are not modelled; nothing published reads them.
