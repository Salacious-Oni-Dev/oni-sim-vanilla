# Disease

Germs are two numbers per cell, a disease index into the disease table and a count, plus two
more the simulation owns and the game never sees. This document covers how germs spread between
cells, grow and die, and how other kernels carry them.

Nothing here creates germs. Disease emitters and `ConsumeDisease` add and remove them, and are
documented in [EMITTERS.md](EMITTERS.md). Germs of Radioactive Contaminants are also a radiation
source (see [RADIATION.md](RADIATION.md)).

Implementation: `sim/disease.h` (`AddDiseaseToCell`, `StepDiseaseDiffusion`,
`StepDiseasePostProcess`), with the germ carriers in `sim/physics.h`. Checked by
`driver/diffsim` scenarios `germdiff`, `germgrow`, `germgas`, `germthresh`, `germvac`,
`germflow`, `germliq`, `germemit` and `germcons`, all identical to the game's library.

## Per-cell state

| field | published | meaning |
| --- | --- | --- |
| `diseaseIdx` | yes | index into the disease table; `0xFF` means none |
| `diseaseCount` | yes | number of germs |
| infestation age | no | substeps since the cell's disease last changed identity |
| growth remainder | no | the fraction of a germ the last growth step could not spend |

The game writes zero into the last two (`reservedInfestationTickCount` and
`reservedAccumulatedError` in `Sim.DiseaseCell`) and only reads them back. The simulation owns
both:

- **The growth remainder** keeps decay working. A 300 s half-life against a 0.2 s substep moves
  0.15 % of a count, which every count under a few hundred would truncate to zero without it.
- **The infestation age** is what `minDiffusionInfestationTickCount` is compared against. It is
  why a single germ landing in a tile does not spread across the map on the next substep. It
  saturates at 254.

A cell clear resets all four. The save format has no field for the last two, so a load zeroes
them, as the game's library does (see [SAVE-FORMAT.md](SAVE-FORMAT.md)).

## The table

The disease table arrives as a payload of one 76-byte record per disease plus 29 bytes per
element:

```
int32   idHashCode
float   strength
float   temperatureRange[4]        // minViable, minGrowth, maxGrowth, maxViable
float   temperatureHalfLives[4]
float   pressureRange[4]
float   pressureHalfLives[4]
float   radiationKillRate
per element:
  float underPopulationDeathRate
  float populationHalfLife
  float overPopulationHalfLife
  float diffusionScale
  float minCountPerKG
  float maxCountPerKG
  int32 minDiffusionCount
  uint8 minDiffusionInfestationTickCount
```

The two pressure ranges are parsed and never read. Pressure effects on germs live entirely in the
game.

## Merging germs into a cell

`AddDiseaseToCell` is the one writer for germs moving into a cell. It merges the incoming
`(index, count)` with the resident one:

- same disease: the counts add;
- either side has none: the other side's disease and count;
- different diseases: compare `count * strength` for each.
  - If the **resident** claim is strictly larger, the survivor is
    `|incomingCount - residentCount * (residentClaim / incomingClaim)|`, with whichever disease
    the sign picks out.
  - Otherwise (a tie, or a stronger newcomer) the **incoming germs are dropped** and the cell
    is unchanged.

Every disease in the shipped table has a `strength` of 0, so both claims are 0 and in a stock
game the resident always wins.

If the merged count is zero or below, all four fields are cleared. If the disease changed
identity, the infestation age restarts at zero.

The same rule merges germs into a consumption report (see [EMITTERS.md](EMITTERS.md)).

## Spreading

Once per substep, after both liquid passes, the simulation takes a snapshot of every cell's
disease index, count and infestation age, then walks the region pairing each cell with the cell
to its right and the cell above. A pair is skipped only when both cells are clean. Amounts are
computed from the snapshot and written into the live grid through `AddDiseaseToCell`, so the
result does not depend on visiting order, and a cell can receive from all four neighbours in one
pass.

**The first gate is phase.** The two cells' elements must be in the same state. Germs never
cross between a gas and a liquid, or out of a solid into the air above it.

Then:

| the pair | moves |
| --- | --- |
| same disease | one eighth of the **difference**, at the diffusion rate of the richer cell |
| one side clean | one eighth of the infected side's count |
| different diseases | one eighth of the count of the side with the larger `count * strength` (ties go to the right or upper cell) |

The diffusion rate is `diffusionScale` for the source cell's element and disease, so germs move
quickly through a gas and slowly through rock. It is zero, and nothing moves, unless the source
held at least `minDiffusionCount` germs **and** an infestation age of at least
`minDiffusionInfestationTickCount`, both read from the snapshot. For Vacuum the threshold is
255 and the age saturates at 254, so germs never diffuse in vacuum.

## Growing and dying

Last thing in each region's substep, after the components and the post-process pass, so the
growth step sees each cell at the temperature and mass the substep left it.

For every infected cell:

```
delta  = count * temperatureFactor - count        // temperature
       + crowding term                            // one of three rules
       - radiation * radiationKillRate            // only when radiation is enabled
       + growth remainder

whole            = (int)delta
growth remainder = delta - (float)whole
count           += whole
```

A half-life `h` gives a factor of `2^(-0.2 / h)` per substep. A negative half-life makes a factor
above 1, which is how germs multiply. A half-life of `+inf` gives exactly 1.

**Temperature** finds where the cell's temperature sits in `temperatureRange`:

| band | factor |
| --- | --- |
| below `minViable` | `temperatureHalfLives[0]` |
| `minViable` to `minGrowth` | interpolated between half-lives 0 and 1 |
| `minGrowth` to `maxGrowth` | exactly 1 |
| `maxGrowth` to `maxViable` | interpolated between half-lives 2 and 3 |
| above `maxViable` | `temperatureHalfLives[3]` |

Radioactive Contaminants have `+inf` in all four slots and ignore temperature.

**Crowding** depends on the cell's **mass**, not only its count:

| | rule |
| --- | --- |
| `count < mass * minCountPerKG` | starving: `underPopulationDeathRate * 0.2` germs are removed |
| in between | `populationHalfLife` |
| `count > mass * maxCountPerKG` | crowded: `overPopulationHalfLife` |

So the same count can thrive in a wisp of gas and die in a dense one. The comparisons are written
so that a NaN (for example `0 * inf` at zero mass) takes the middle branch.

A second pass over every cell in the region, infected or not, clears any cell whose count is zero
or below and advances the infestation age by one.

## Germs carried by other kernels

Germs move with matter. Each of these is the game's behaviour and is compared by a scenario:

| kernel | what the germs do |
| --- | --- |
| liquid flow | each transfer carries a share of the source's germs, priced against the source's starting germ type and a running count |
| liquid pressure displacement | the displaced cell's germs go with it; then one eighth of the source's starting count follows the source's liquid |
| gas pressure | the moved fraction of the source's mass carries the same fraction of its germs |
| gas displacement | the displaced gas takes all its germs; then one eighth of the source's starting count follows the source's gas |
| `DisplaceGas` | the displaced gas takes all its germs |
| sublimation | the gas produced takes the solid's germ share, in proportion to the mass taken |
| liquid off-gas | the gas produced takes the liquid's germ share, but only when the cell above holds no germs or the same disease |
| partial melt | the gas cell receives the solid's germ share for the 5 kg melted |
| state transitions | transition ore takes its share of the cell's germs |
| falling liquid | the particle handed to the game carries the cell's germs |
| unstable solids | the falling cell is handed to the game with its germs |
| cell swaps | all four disease fields move with the cell |

Details and exact share rules for each are in [PHYSICS.md](PHYSICS.md). The liquid passes price
against their own disease snapshot, taken before the liquid sweeps; the diffusion pass retakes
its snapshot afterwards.

### Germs in a cell with no mass

The gas pressure pass prices its germ share as the mass moved over the source cell's mass, and
clamps it to `[0, 1]`
with `min`/`max` semantics that turn a NaN into 1. For a source cell with no mass, `0 / 0` is
NaN, so the clamp makes it "move the whole count". The source's count is read from the snapshot,
but its disease index is read **live**: the first transfer drains the source and clears its
index to `0xFF`, and every later transfer from that source in the same substep carries no
disease and changes nothing. So germs seeded in vacuum relocate whole to one neighbour per
substep instead of dying. `germvac` checks this cell by cell. A real game does not put germs in
a cell with no mass; the case matters for exactness, not play.

## Cost

Every disease pass is skipped when no cell in the world holds germs. That is decided once a frame
by one sequential pass over one byte per cell. Nothing creates germs inside a frame except
emitters registered before it, so the answer holds for the whole frame.

## Limits

- The `radiationKillRate` term is implemented, but no scenario has shown it change a result:
  removing it leaves `radsource` identical.
- Whether the infestation age saturates at 254 or 255 cannot be observed: every element and
  disease pair with a nonzero diffusion scale in the shipped table has a threshold of 1.
- Because every shipped disease has zero strength, the ratio branch of the merge is not reached
  in a stock game.
