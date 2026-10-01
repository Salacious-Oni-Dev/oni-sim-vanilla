# `ModifyCell`

`ModifyCell` is how anything outside the simulation puts matter into a cell or takes it out: a
building's output, a duplicant exhaling, a pump, a dig filling back in, a debug spawn. It is
drained **last** of the cell messages (see [DRAIN.md](DRAIN.md)).

It is not one handler. There are two replace paths, three add paths dispatched on phase that
share a name and little else, a remove path, and two displacement primitives they use.

Implementation: `sim/cellmod.h`; the dispatcher is `ApplyModifyCell` in `sim/simdll.cpp`.
Checked by `driver/diffsim` scenarios `modifycell`, `cellmodgas`, `cellmodsolid`, `cellmodliq`
and `msgorder`, with `--ledger` reporting zero mass drift on all of them.

## The payload

```c
struct ModifyCellMessage {   // 28 bytes
    int32_t  cellIdx;
    int32_t  callbackIdx;
    float    temperature;
    float    mass;
    int32_t  diseaseCount;
    uint16_t elementIdx;
    uint8_t  replaceType;    // 0 add/remove, 1 replace, 2 replace and displace
    uint8_t  diseaseIdx;
    uint8_t  addSubType;     // read by the solid add path only
};
```

The callback is reported when `callbackIdx != -1`, not when it is `>= 0`. Any other negative
index is handed back to the game.

## The clamp in front of everything

It fires only when the message is already out of range, so an in-range removal keeps its
temperature untouched:

```
if ((T <= 0 && M > 0) || T > 10000) {
    if (T >= 10000) T = 10000;
    if (T <= 0)     T = 0;
    if (M <= 0)   { M = 0; T = 0; }
}
```

## `replaceType`

### 1: replace

Overwrites the cell's element, mass, temperature and disease.

- The substance change is **announced unconditionally**, even when the cell is replaced with
  the element it already held.
- Writing Vacuum forces both the mass and the temperature to zero, whatever the message asked.
- All disease fields are written (`diseaseIdx = 0xFF` included), and the infestation age and
  growth remainder are reset.
- No temperature clamp beyond the one above.

### 2: replace and displace

The same, with the gas or liquid in the cell pushed out first. Whether the displacement found
anywhere to go is **ignored**: the cell is overwritten either way. This path also clamps the
temperature to `[0, 10000]` unconditionally.

### 0: add or remove

Dispatched on the phase of the element **in the message**, not the one in the cell.

A negative mass is a **removal**, and acts only when the cell's current phase matches the
message's element phase. The entry conditions are not symmetric: gas removes on `mass <= 0`,
solid and liquid on `mass < 0`. So a zero-mass gas message clears an already-empty gas cell,
and a zero-mass solid message does nothing. A removal takes `-mass` from the cell, floors at
zero, and clears the cell when what is left is at or below the smallest normal float.

Adding Vacuum is dropped.

## Adding a gas

1. **Same element already there:** a plain merge, with no announcement. Nearly every emitter in
   a real game takes this path every frame.
2. **Otherwise displace what is there:** a gas with `DisplaceGas`, a liquid with
   `DisplaceLiquid`, nothing for vacuum. A solid is never displaced by a gas. If the
   displacement worked, the cell takes the new gas.
3. **Blocked:** scan **left, right, up**, never down. A vacuum neighbour is overwritten with the
   new gas; a neighbour already holding the same gas is merged into.
4. **Nothing took it:** the deletion tail below. If the cell ends up empty it is fully cleared,
   disease included.

## Adding a liquid

Everything the gas path does, with a step in front and a different scan.

- **Retarget.** A liquid aimed at a **solid** cell moves to the first non-solid neighbour,
  scanning **left, right, down, up**, and everything after that happens at the new cell.
- **Void** is tested by element identity, not phase. Its phase is vacuum, and a phase test
  would let liquid in.
- When a gas in the way cannot be displaced and there is liquid directly above, the two cells
  trade places before the scan.
- The blocked scan is **right, left, up, down**, and only a neighbour holding the same liquid
  counts; vacuum does not.
- The deletion tail writes Vacuum over an emptied cell but does **not** clear it, so germs the
  add delivered stay behind. The gas path clears. The difference is the game's.

## Adding a solid

The only path that reads `addSubType`:

| value | name | effect |
|---|---|---|
| 0 | `DoVerticalDisplacement` | top up, then place in the first open cell upward |
| 1 | `OnlyIfSameElement` | refuse a different solid; otherwise add in place |
| anything else | | the whole message is ignored |

**`OnlyIfSameElement`:** a different solid refuses the message. A gas or liquid in the cell is
displaced. Then the mass is added in place, and the element is **not written**. In a gas cell
the displacement has just left Vacuum behind, so the cell becomes Vacuum holding mass until a
later transition resolves it. This is the game's behaviour, and `cellmodsolid` checks it.

**`DoVerticalDisplacement`:**

1. If the cell below holds the same element, fill it up to the element's `maxMass`; then do the
   same for the cell itself. Germs go with each part in proportion to its mass.
2. Put whatever is left into the first non-solid cell scanning upward from the target row,
   displacing its gas or liquid first, as an **overwrite** of element, mass, temperature and
   disease. If the scan reaches Void, the remainder is destroyed.

## The deletion tail

The gas and liquid add paths both end here when there is nowhere for the matter to go. It is the
only place in the game's simulation that destroys mass on purpose:

- **incoming lighter** than what is in the way: the incoming matter is dropped and the blocker
  is thinned by exactly that much. Nothing is added or announced.
- **incoming heavier or equal:** the blocker is emptied, the cell takes the new element, and
  only the **remainder** is added, with germs scaled by `remaining / mass`.

Both are charged to the `modified` line of the mass ledger (see [LEDGERS.md](LEDGERS.md)), so
the deletion is visible.

## Displacing a liquid

`DisplaceLiquidSimple` splits a liquid cell evenly between whichever candidate neighbours will
take it, then empties the source:

- a source holding less than **0.01 kg** cannot be displaced at all;
- a candidate qualifies by holding the **same element or Vacuum** (an element test, not a mass
  test) and having its `LiquidImpermeable` property clear;
- germs split with a **ceiling**, `(count - 1 + n) / n`, so one germ split four ways gives one
  germ to each of four;
- each receiving cell is announced as a substance change and has its unstable countdown reset.

`DisplaceLiquid` tries the four neighbours **right, left, up, down**. If none will take the
liquid, it walks the same four, starting at an offset set by the substep counter, for a gas
neighbour that `DisplaceGas` can clear, and then tries once more.

The same primitive serves the over-full liquid displacement in the liquid pass (see
[PHYSICS.md](PHYSICS.md)).

## The 1 K floor

A `ModifyCell` can write a temperature anywhere in `(0, 10000]`, but a cell with mass and heat
capacity does not stay below 1 K: the conduction pass clamps both cells of every pair it
handles. That is conduction's rule, not this message's. See [QUIRKS.md](QUIRKS.md).

## Limits

- No scenario sends a negative `callbackIdx` other than -1.
- The liquid path's swap with liquid above is implemented, but no scenario reaches it: it needs
  liquid resting on gas at drain time, which the liquid pass undoes every substep.
- The zero branch of the mass-and-temperature merge (temperature zeroed and disease cleared,
  mass untouched) cannot be reached: every caller adds a non-negative amount, so it fires only
  on a cell that is already empty and cold.
