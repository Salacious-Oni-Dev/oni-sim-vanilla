# Radiation

The per-cell `radiation` array is not a pass-through. The game changes a cell's rads with
`CellRadiationModification` and reads the array back each frame, but the simulation owns it
between those points: it decays it, adds three sources of its own, runs the radiation emitters
that paint it, and clamps it.

Implementation: `sim/radiation.h` (the field pass `StepRadiationField` and the emitters
`StepRadiationEmitters`), with the message handlers in `sim/simdll.cpp`. Checked by
`driver/diffsim` scenarios `radiate`, `radpulse`, `radattract`, `radmsg` and `radsource`, all
identical to the game's own library for every tick.

Radiation is enabled per world by the game. Every step below is skipped when it is off.

## The field

Two whole-region sweeps run **once per substep**, immediately before the components (so before
the emitters that feed them).

**Top down** (rows from the top of the region to the bottom):

- **Occlusion.** `occlusion[cell] = (1 - absorption(cell)) * occlusion[cell above]`, set to zero
  below 0.01. The top two rows of the region read 1.0 for the cell above.
- **Decay.** `x -= x / RADIATION_LINGER_RATE`. When that quotient is exactly zero (only possible
  for zero or a denormal), 1 rad is subtracted instead and the result floored at zero.

**Bottom up**, three sources and then the clamp:

- a cell holding a **radioactive element** adds
  `mass / 1000 * radiationPer1000Mass * weight` to each cell of a 5 x 5 stencil. The weights are
  1.0 at the centre, 0.75 on the four orthogonal neighbours, 0.5 on the four diagonal
  neighbours, 0.25 two cells out orthogonally, 0.15 on the eight knight's-move cells and 0.1 on
  the four far corners. They sum to 8.6: a weight map, not a normalised kernel, so a lump
  contributes several times its own rating to the world around it;
- a cell carrying **radiation sickness** germs adds `count / 1000`;
- **cosmic radiation** adds `currentCosmicRadiationIntensity / RADIATION_LINGER_RATE` times the
  cell's occlusion. The intensity comes from each world's `NewGameFrame` entry;
- the result is clamped to `[0, 9000000]`, and anything at or below 0.01 becomes zero.

The decay gives a constant emitter a finite steady state. A constant emitter writes
`rads / LINGER_RATE` per substep and the decay removes `x / LINGER_RATE`, so `x` converges on
`rads`.

The parameters and their defaults:

| index | parameter | default |
| --- | --- | --- |
| 0 | `RADIATION_LINGER_RATE` | 1.1 |
| 2 | `RADIATION_BASE_WEIGHT` | 0.3 |
| 3 | `RADIATION_DENSITY_WEIGHT` | 0.7 |
| 4 | `RADIATION_CONSTRUCTED_FACTOR` | 0.8 |
| 5 | `RADIATION_MAX_MASS` | 2000 |

`RadiationParamsModification` overwrites one of them by index. **Index 1 is not handled** and
is dropped, as in the game's library.

## Absorption

One function, used by both the occlusion sweep and the emitter's line walk:

- start from the element's `radiationAbsorptionFactor`, `f`;
- on a **constructed tile** (cell property bit `0x80`): `f * RADIATION_CONSTRUCTED_FACTOR`;
- otherwise: `mass / RADIATION_MAX_MASS * f * DENSITY_WEIGHT + f * BASE_WEIGHT`;
- clamp to `[0, 1]`, with `min`/`max` semantics that turn a NaN into 1.0. The game can set
  `RADIATION_MAX_MASS` to zero, which makes an element that absorbs nothing produce `0 * inf`.

`RadiationAbsorptionAlongLine` walks a Bresenham line from the emitter to the target and
multiplies a running transmission by `1 - absorption` at every cell **including both
endpoints**, then clamps it to `[0, 1]`. An emitter buried in lead attenuates itself.

## Emitters

A radiation emitter is registered with `AddRadiationEmitter`, changed with
`ModifyRadiationEmitter` and removed with `RemoveRadiationEmitter`. Each holds a cell, two
radii, `emitRads`, `emitRate`, `emitSpeed`, `emitDirection`, `emitAngle`, two timers, a type and
a step. There are six types: `Constant`, `Pulsing`, `PulsingAveraged`, `SimplePulse`,
`RadialBeams` and `Attractor`.

Each substep, per emitter:

- An emitter whose cell is outside the region, whose `emitRads` is not positive, or whose radii
  are both zero is skipped **before** its timers move, so it is frozen, not merely silent.
- `emitTimer += dt`. With `emitRate` zero the emitter always fires; otherwise it fires when
  `emitRate <= emitTimer`. Inside the sweep window (`emitTimer <= emitSpeed`) firing resets the
  timer and the step. Past the end of the window, an emitter that does not fire does nothing at
  all this substep.
- A step lasts `emitSpeed / max(radiusX, radiusY)` seconds, and the substep takes `dt / step`
  steps. `emitStepTimer` carries the remainder, so a substep too short for a step banks its
  time.
- `emitStep` advances and wraps at `max(radiusX, radiusY)`.

What each type paints:

- **`Constant`** writes every cell of its ellipse, each attenuated along the line from the
  emitter. The falloff is a bilinear tent, `(1 - |dx|/rx) - (|dy| - |dy·dx|/rx) / ry`, not a cone.
- **`Pulsing`** draws an anti-aliased ring that grows with the step, attenuated per ray.
- **`PulsingAveraged`** is the same, with the rads divided by `(rx - 1) * (ry - 1)`, so a pulse
  spreads a fixed budget.
- **`SimplePulse`** draws the same ring with **no** attenuation, and writes its own cell like
  any other.
- **`Attractor`** draws the ring and removes rads from it rather than adding them.
- **`RadialBeams`** paints nothing. Its step and timers still advance.

The ring is rasterised twice by `SetCircleAA`, once stepping x and once stepping y, and each step
splits its value between two adjacent pixels by the rounded fractional part of the crossing.
Each pixel is mirrored four ways.

### Rules a plausible rewrite gets wrong

- **`Constant`'s scan box is square in `radiusX`.** Its rows run from `cy - radiusY` for
  `2 * radiusX + 1` rows. An emitter wider than it is tall scans extra rows and discards them; an
  emitter taller than it is wide never scans part of its ellipse, so it emits a clipped shape.
- **`AddRadiationEmitter` stores `min(emitSpeed, emitRads)`; `ModifyRadiationEmitter` stores
  `min(emitRate, emitSpeed)`.** The two clamps are against different fields. Neither makes
  physical sense; both are what the game's library does.
- **Noise is drawn only for the outer three quarters.** `Constant` advances the world's random
  stream only where the falloff is below 0.25, adding `r * v * scale - v * 0.125` with `scale`
  just above 2^-17. So an emitter's geometry decides how many random draws it takes, and every
  later consumer of the stream, the gas shuffle included, moves with it.
- **The attractor reports only the last cell it drained**, not the sum, and that one amount is
  added back to the attractor's own cell.
- **The cone.** An `emitAngle` of exactly 360 means no cone and skips the angle test; the
  emitter's own cell is always in range. A cone whose end has wrapped below its start is tested
  as `a >= lo || a <= hi`.

## `CellRadiationModification`

Adds `radiationDelta` to the cell. If the result is at or below zero, the cell is set to zero.

If the message carries a callback, the callback is reported in `radiationConsumedCallbacks` as
`{callbackIdx, gameCell, radiation}`, not in the general `callbackInfo` list. The reported
amount is the delta applied, except when the result was clamped to zero, where it is the rads
the cell had: a consumer that asks for more than is there is told what it got.

Per-cell changes are applied before parameter changes in the same frame. See
[DRAIN.md](DRAIN.md) for where both sit in the drain.

## Saving and restoring

Radiation is the fourth float of each cell in the save blob. `Load` clears radiation in Vacuum
and Void cells, as the game's library does, although a running world can hold radiation there.
See [SAVE-FORMAT.md](SAVE-FORMAT.md).

## Limits

- The decay's zero branch cannot be told apart from plain decay except on a denormal, and no
  scenario reaches one.
- The order of the four mirrored pixel writes is observable only through the attractor's single
  output, where it cannot be distinguished.
- The scenarios cover radioactive solids only. Radioactive liquids and gases use the same code
  path, but no scenario compares them.
- A frame with no physics does not step the emitters.
