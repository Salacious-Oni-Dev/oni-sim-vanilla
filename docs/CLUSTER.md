# The cluster messages

A cluster map puts several asteroids on one grid, and each cell belongs to at most one of them.
Five messages describe that layout to the simulation: `SetWorldZones`, `DefineWorldOffsets`,
`ModifyCellWorldZone`, `SimData_ResizeAndInitializeVacuumCells` and `SimData_FreeCells`. The
game sends the first three on every load, and `ModifyCellWorldZone` hundreds of times a frame
after that.

Implementation: the message cases in `SIM_HandleMessage` (`sim/simdll.cpp`), the world list in
`sim/world.h` (`SetWorldOffsets`, `AddWorldOffset`, `PromoteWorldOffsets`) and the sunlight
sweep `ComputeSunlight` in `sim/textures.h`. Checked by `driver/diffsim` scenarios `zones`,
`sunlight`, `sunliquid`, `sunglass` and `vacrect`.

## World zones: inert

`SetWorldZones` sends one byte per game cell, row-major (`width * height` bytes, where width
and height are the game's, not the padded grid's). `ModifyCellWorldZone` edits one cell:

```c
struct CellWorldZoneModification { int32_t cell; uint8_t zoneID; /* padded to 8 bytes */ };
```

Neither changes anything the simulation computes. With a real two-zone split and per-cell
edits every frame, the game's own library produces output bit-identical to the same world with
no zone messages at all: mass, element, temperature and every texture. This simulation accepts
both messages and ignores them. The `zones` scenario exists to catch the day that stops being
true.

Two things matter to anything that replays these messages:

- **Order is load-bearing in the game's library.** A `ModifyCellWorldZone` sent before any
  `SetWorldZones` crashes it, and `DefineWorldOffsets` followed by `ModifyCellWorldZone` with no
  zones hangs it. The game always sends `SetWorldZones` first. This simulation survives either
  sequence.
- **The game's library does not check the `SetWorldZones` payload length in a release build.**
  A payload sized as `(width - 2) * (height - 2)` is short, and it reads past the end of the
  caller's buffer. A hang in a harness that sends this message is a short payload until proven
  otherwise.

## `DefineWorldOffsets`

An int32 count, then four int32s per world: `offsetX, offsetY, sizeX, sizeY`, in game cells.
The list replaces any previous one.

This is not inert. It switches on the **sunlight texture**, which is computed per world and is
zero everywhere until at least one world is defined.

The offsets **lag by one published frame**. The frame that receives the message still publishes
the old geometry; the new one appears from the next frame. `World::PromoteWorldOffsets` stages
the offsets and promotes them after the frame has published.

## The sunlight texture

`propertyTextureExposedToSunlight` is one byte per cell. Solar panels and plants read it through
`Grid.exposedToSunlight`, so it has gameplay consumers, not only rendering ones.

For each world, for each column in its x-span:

- The world's **top row** (`y = offsetY + sizeY - 1`) starts at full exposure, whatever the cell
  holds. Two worlds of different heights light two different rows.
- Light descends one cell at a time. Each cell is published **before** it absorbs:
  `byte = (uint8_t)(exposure * 255)`.
- A **solid** cell absorbs its element's whole `lightAbsorptionFactor`, whatever its mass.
  Granite (factor 1.0) blocks completely even at a quarter of its `maxMass`. Glass and diamond
  (0.1) let most light through, and ice (0.33) about two thirds. The `Transparent` cell property
  is not an input.
- A **fluid** cell absorbs `min(mass, maxMass) / maxMass * lightAbsorptionFactor`. The factor
  is the element's own: oxygen's is 0, so oxygen is transparent in any amount; carbon dioxide's
  is 0.1, and water's is 0.25. Every gas has a `maxMass` of 1.8 kg, so gas follows this rule
  with nothing special of its own.
- When exposure reaches zero, the rest of the column is dark.

The mass and element read are the **projected** values of the frame being published, not the
live grid, so the texture agrees with the arrays published beside it.

`NewGameFrame.currentSunlightIntensity` is not an input.

The sunlight texture is filled **before** the liquid texture, because the liquid texture uses
`1 - sunlight / 255` as a factor on each cell's gradient position. Filling it afterwards would
make the liquid colour read the previous frame's light. See [PROJECTION.md](PROJECTION.md) for
the liquid texture.

Cost: the column walk stops at the first cell that brings exposure to zero, so buried columns
are almost free. Open space is what costs; on a 512 x 768 asteroid the texture adds about
0.04 ms a frame.

## `SimData_ResizeAndInitializeVacuumCells`

Six int32s: `gridSizeX, gridSizeY, width, height, xOffset, yOffset`. When the grid size equals
the current grid, the message **opens a new world** in it: a rectangle of `width x height`
cells at `(xOffset, yOffset)`, in game coordinates.

It writes, in order:

- **the ring**, one cell outside the rectangle on every side: element Unobtanium, a flat
  9999 kg, 0 K, both disease fields cleared, and its radiation **left as it was**;
- **the rectangle**: element Vacuum, mass 0, 0 K, both disease fields cleared, and radiation
  **zeroed**. The infestation counter and growth accumulator are not touched, which is what
  separates this from a normal cell clear;
- **the backwall** over ring and rectangle together: Vacuum, mass 0, 0 K.

Three further effects:

- **The rectangle is registered as a world**, appended to the world list (whether the game's
  library appends or replaces is not established; no scenario sends both this and
  `DefineWorldOffsets`), and it takes effect
  **immediately**, not a frame later as `DefineWorldOffsets` does. The rectangle reads back
  fully lit from the message's own frame, and a solid dropped into it later shadows the column
  below it.
- **The projection's record of solidity is reset** over the ring (solid, with mass) and the
  rectangle (not solid, no mass), so the game receives no `solidInfo` announcements for the
  ring. Element changes are announced as usual through `substanceChangeInfo`.
- **The mass ledger has no line for it**: the overwrite removes whatever the cells held and
  adds the ring's mass, and the change shows as drift. See [LEDGERS.md](LEDGERS.md).

The ring's temperature stays at exactly 0 K afterwards. Unobtanium has zero conductivity and
zero heat capacity, and a conduction pair where either cell has no heat capacity is skipped
before the 1 K floor is applied (see [QUIRKS.md](QUIRKS.md)).

A message whose grid size differs from the current grid would resize the grid. That is not
implemented: the simulation reports it and does nothing.

## `SimData_FreeCells`

Accepted and ignored. No scenario has shown the game's library doing anything with it.
