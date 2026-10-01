# The backwall

The backwall is a second material behind a cell: the tile you see through a dug-out room. The
game sends one `SimBackwall` per cell (an element index, a mass and a temperature), and the
simulation publishes all three back every frame in `GameDataUpdate` as `backwallElement`,
`backwallMass` and `backwallTemperature`.

Implementation: `UpdateBackwallTransitions` and `BackwallShouldTransition` in `sim/simdll.cpp`,
storage in `sim/world.h`. Checked by `driver/diffsim --scenario backwall`, which compares the
three backwall arrays cell by cell and the transition list frame by frame against the game's
own SimDLL.

## What the simulation does with it

Almost nothing. No kernel conducts heat into a backwall or out of one, moves its mass, or
changes its element, and the cells in front of a backwall are not affected by it. Its element,
mass and temperature leave the simulation exactly as the game put them in.

The one thing the simulation does is tell the game which backwalls have left their own
element's temperature range:

```
numBackwallShouldTransitionInfos / backwallShouldTransitionInfos    { int32 gameCell }
```

The swap itself is the game's to make. It comes back as a `ModifyBackwallData` message.

## The rule

A cell is announced when all three of these hold:

| condition | test |
| --- | --- |
| the backwall has mass | `mass > 0` |
| its temperature is outside its element's range | `temperature > highTemp`, or `temperature < lowTemp` |
| there is somewhere for it to go | the matching `highTempTransitionIdx` / `lowTempTransitionIdx` is neither `0xFFFF` nor the element itself |

The third condition is what separates this rule from a plain range test. Vacuum's `highTemp` is
0, so a Vacuum backwall at any temperature is out of range, but its high transition points back
at Vacuum, so it is never announced.

Examples, all covered by the `backwall` scenario:

- a granite backwall at 1000 K is announced (granite's `highTemp` is 942 K); the same backwall
  at 500 K or 100 K is not;
- a water backwall is announced on both sides of its range, at 400 K and at 200 K;
- a massless granite backwall at 1000 K is never announced;
- a Vacuum backwall carrying mass at 20000 K is never announced.

## Level-triggered

The announcement is repeated **every frame** for as long as the cell stays out of range. It is
a property of the world, not an event.

It is stored that way. `World` records every cell whose backwall was written since the last
frame (a load marks all of them, a `ModifyBackwallData` marks one). `UpdateBackwallTransitions`
folds those writes into a sorted list of out-of-range game cells, and the frame publishes the
whole list. Nothing rescans the grid unless a backwall was written. The list is in ascending
game-cell order, the order the game's library announces them in.

## Not published at `Start`

The `GameDataUpdate` returned by `Start` carries `0xFFFF` for every backwall element and zero
for every backwall mass and temperature. The real values appear from the first
`PrepareGameData` onwards. This matches the game's own library, and it is the same shape as the
property textures, which are also empty after `Start` (see [PROJECTION.md](PROJECTION.md)).

## No element

A backwall element index past the end of the element table means "no element". Worldgen sends
`0xFFFF` for every template cell whose `backwallElement` is unset. The simulation keeps it as
no element, publishes `0xFFFF`, and writes it to the save blob as Vacuum. It is never clamped
to element 0, which would put a real material behind the cell.

A backwall with no element and nonzero mass is not a state the game produces.

## Limits

- `backwallElementChangedInfos` is published empty. The simulation never changes a backwall
  element itself, so there is nothing to report.
- This simulation holds the backwall thermally inert. One measurement against the game's own
  library showed a backwall drifting by about 1 K over nine ticks, which would mean some
  conduction does reach it there. That has not been confirmed or explained, and the
  simulation does not model it.

## How it is compared

The announcement list cannot be compared tick by tick. The game's simulation runs on a worker
thread, and a `PrepareGameData` can find zero, one or two frames finished, so over the same
number of ticks the two libraries run slightly different numbers of frames. `diffsim` divides
each tick's list by the frame count it reports, checks that every frame's list is identical,
and scores one frame's list.
