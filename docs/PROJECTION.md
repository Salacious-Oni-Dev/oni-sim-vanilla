# The projection contract

How world state becomes `GameDataUpdate`. This is the whole mod-compatibility surface:
mods reach sim state through managed `Grid.Element[]`, `Grid.Mass[]` and
`Grid.IsSolidCell`, and all three come from here.
Compatibility is decided by this file, not by whether the physics matches Klei's — which
is why it is implemented and tested before any physics exists.

Implementation: `sim/projection.h`. Verified by `driver/build/diffsim.exe`, which loads
Klei's SimDLL and the replacement into one process, seeds both identically, and steps
them in lockstep.

## The rules

The rules are written for a cell holding several phases. Today `Project` passes each cell's
single `PhaseEntry`, and with one phase rules 1, 2 and 4 reduce to that phase's own mass,
element and temperature.

**1. `mass[cell]` is the total mass of the cell**, summed over every phase present. Airlock
doors depend on this, and it is the only reading that keeps the vanilla pressure display honest.

**2. `elementIdx[cell]` is the phase with the largest volume fraction.** Volume, not mass: a
kilogram of granite and a kilogram of hydrogen do not take up the same space, and ranking by mass
would report a room of gas as sand the moment a grain fell into it. Ties go to the lower element
index. That order is stable from frame to frame, so the value cannot flicker.

**3. Solidity is thresholded with hysteresis.** A cell becomes solid at a solid volume fraction
of 0.55 or more and stops being solid at 0.45 or less; in between it keeps its previous state.
The `SolidImpermeable` cell property forces solid regardless; that is how doors and tempshift
plates work. The dead band matters: pathfinding, `UnstableGroundManager` and every `IsSolidCell`
consumer re-evaluate on each `solidInfo` event, and a cell hovering at one threshold would make
all of them thrash.

Mass is not a term in the fraction. A single-phase cell of a solid element is solid whatever it
holds, including zero mass, exactly as the game's own library reports it. A multi-phase cell
whose phases carry no mass falls back to its dominant element.

**4. `temperature[cell]` is the mass-weighted mean**, so `mass × specific heat × temperature`
still sums to the cell's internal energy. With one phase this is exact. With several phases of
different specific heats it is an approximation.

**5. Element and solidity changes are announced.** `Grid.Element[]` is a managed cache that
`Game.StepTheSim` updates only from `substanceChangeInfo`, and `Grid.SetSolid` runs only from
`solidInfo`. A cell whose element changed without an event would keep a stale `Grid.Element[]`
entry forever. `Project()` is the only writer of the game-visible buffers, so there is exactly
one place where a change could go unannounced.

The first frame is the exception: `Sim.Start` seeds the managed caches from the arrays directly,
so no events are sent for it. After that, no event means no change.

## Buffers

**The game-visible arrays are unpadded; internal storage is padded.** Storage carries a one-cell
border ring, so the save blob is a straight serialisation and neighbour loops need no bounds
test. `GameDataUpdate` arrays are indexed by unpadded game cell. Mixing the two up shifts the
whole world by a row.

**The border ring is Neutronium at 9999 kg and 0 K, except the top row, which is vacuum.** The
world is open to space at the top and sealed everywhere else. The border is not addressable from
the game, but it is part of the save blob.

**The published buffers may be reallocated between frames, but not within one.**
`Game.StepTheSim` re-reads every pointer from a fresh `GameDataUpdate*` each frame, and that
struct comes from `PrepareGameData`, not from `Start`. The pointers have to stay valid from the
frame that published them until the game has finished reading that frame.

**The buffers persist between frames, so an unwritten cell is not a stale cell.** The projection
is incremental because of this:

- Seven of the thirteen arrays are pass-throughs: properties, insulation, strength, radiation,
  the two disease fields and the three backwall fields. They arrive from the game and go back
  out unchanged unless a substep writes them. `World` records every write to one of them by cell,
  and `Project` copies exactly those cells. **Every writer must go through `World`'s `Mutable*`
  accessors.** They are named apart from the const ones so that the compiler lists every writer;
  a write that reaches the arrays another way leaves a permanently stale buffer.
- `FillPropertyTextures` skips any cell whose element has not changed and which holds no liquid,
  because three of its four buffers depend on the element alone and the fourth is zero without
  liquid.

Neither shortcut is an approximation. `bench --verify` recomputes both from scratch into shadow
buffers every frame and compares them byte for byte.

## The property textures

`GameDataUpdate` ends with five pointers the game passes straight to
`Texture2D.LoadRawTextureData`. **They are raw buffers, not handles**: a null pointer is not "no
texture", it is a crash inside Unity.

| Texture | Format | Bytes per cell |
|---|---|---|
| Flow | RGFloat | 8 (two floats) |
| Liquid | RGBA32 | 4 |
| LiquidData | RGBA32 | 4 |
| MaterialData | RGBA32 | 4 |
| ExposedToSunlight | Alpha8 | 1 |

The textures are not filled by `Start`; they fill on the first `PrepareGameData`. The sunlight
texture is filled after the liquid texture, which reads the previous frame's sunlight.

### Liquid

- **Fill** is `powf(min(mass / 1000, 1), 0.45)`. The divisor is a literal 1000, not the
  element's `maxMass`.
- **Alpha** is that fill for a cell whose top is open, and 255 for a covered one. A cell counts as
  covered when the cell above holds liquid or solid, or has its `LiquidImpermeable` property set.
- **Gradient position** is 0 when the cell above is gas or vacuum. Otherwise it is
  `min(1 - sunlight / 255, 1) × fill`, capped by the position of the cell above, computed one
  level deep and not recursively.
- **RGB** walks the element's `gradientColours`. Each entry packs an RGB colour in its low three
  bytes and a key in its high byte. The walk keeps the last stop whose key (over 255) is at or
  below the position, and interpolates towards the next one with a truncated delta:
  `(int)((float)(next - cur) * t) + cur`.

### LiquidData

- **RGB** is the colour of the element this one would change into, picked by which half of its
  temperature range the cell is in, or white when the element has no transition.
- **Alpha** is `255 × (T - (lowTemp - 3)) / ((highTemp + 3) - (lowTemp - 3))`, truncated. Note the
  three-degree widening at each end.

### MaterialData

`materialProperties & 0xff00000f` of the element, unpacked little-endian, for liquid cells. Zero
for every other cell.

### ExposedToSunlight

Filled per world, and only once `DefineWorldOffsets` has said where the worlds are. Each world's
top row starts at full exposure (1.0), whatever it holds. Light then descends column by column.
Each cell's byte is written from the exposure that reaches it, and the cell then takes its share
off the exposure passed further down. Absorption is **subtracted** from the exposure, not
multiplied into it. The mass and element read are the projection's, not the live grid's.

- A **solid** subtracts its whole `lightAbsorptionFactor`, whatever it weighs. Granite (1.0)
  ends the beam at any mass. Glass and diamond (0.1) take a tenth of full exposure off it, and
  ice (0.33333) a third. The cell's `Transparent` bit plays no part.
- A **gas or liquid** subtracts `min(mass, maxMass) / maxMass × lightAbsorptionFactor`, and
  nothing when its `maxMass`, its factor or its mass is zero. Oxygen's factor is 0, so it is
  transparent at any mass. Carbon dioxide's is 0.1 and every gas has a `maxMass` of 1.8, so
  1.8 kg of it takes off a tenth. Water (0.25, `maxMass` 1000) takes a quarter off for a full
  cell.

The published byte is `exposure × 255`, truncated. Once exposure reaches zero, the rest of the
column stays dark (0).
`NewGameFrame.currentSunlightIntensity` is not an input to this texture.

Solar panels and plants read this texture through `Grid.exposedToSunlight`, so it has gameplay
consumers, not only rendering ones.
