# Measured behaviour

Behaviour of the game's own simulation that is surprising, easy to get wrong, or contrary to
common belief, and that this simulation reproduces. Each item was measured by running the
game's SimDLL offline on worlds built for the purpose, with the game's own element table, and
is checked against this simulation by a named scenario.

Two harnesses are involved:

- `driver/experiments` runs the game's library alone and keeps a ledger of every transfer in or
  out of the grid (ore, falling particles, building consumption and emission), so that a
  conservation check reports only what is genuinely unaccounted for.
- `driver/diffsim` runs both libraries side by side and compares every published array.

```sh
driver/build/experiments --corpus <corpus.bin> --scenario <name> --ticks 1000
```

`experiments` scenarios: `conservation`, `onetile`, `liquidlock`, `pump`, `overpressure`,
`soliddrop`, `displace`, `dig`, `buildingheat`.

## A cell holds one element, and that is the gas seal

`liquidlock` joins a chamber of 980 kg of pressurised CO2 to a near-vacuum chamber through a
**single cell** of water. After 200 simulated seconds, no CO2 has crossed, and the plug has
drained to 31 grams of water. A film with almost no mass is still a perfect gas seal, because a
cell holding liquid cannot also hold gas.

So the limit is one element per cell, not cell size. A smaller cell would change how much water
a plug needs, not whether a plugged cell is impermeable.

## The liquid, gas and conduction passes conserve

In sealed worlds with liquid settling, gas moving and a 57 K gradient equalising, mass is
conserved to the gram and energy to float precision (about 0.0009 % over 200 ticks, in the
conduction-heavy case). With buildings, a pump, a vent, digging, and a building dropped into
water with no free cell anywhere, the ledger still closes:

| scenario | what it drives | unaccounted mass |
|---|---|---|
| `pump` | element consumer, element emitter, direct consumption | 0.0000 kg |
| `overpressure` | 50,000 kg emitted into one sealed cell | 0.0000 kg |
| `soliddrop` | solid emission, landing sand, replace-and-displace, dig | 0.0000 kg |
| `displace` | a building placed into liquid with no headroom | 0.014 kg |
| `dig` | 40 granite cells dug out | 0.0000 kg |

- **Digging is exact.** 48,000 kg dug hands the game 48,000 kg of ore.
- **Displaced liquid is not deleted, even with nowhere to go.** In `displace`, the water pushed
  out of the cell is accepted by neighbours already at capacity.

This does not mean the game's simulation never destroys mass or energy. Specific paths do: a
gas or liquid add with nowhere to go ([CELLMOD.md](CELLMOD.md)), the conduction pass's 1 K
floor (below), and several energy-message refusals. Those are itemised in
[LEDGERS.md](LEDGERS.md).

## `maxMass` governs flow, not storage

- Oxygen's `maxMass` is 1.8 kg. A thousand 50 kg emissions into one sealed cell are all
  accepted; the cell ends at about 50,000 kg, with nothing refused and nothing lost.
- A single cell seeded with 20,000 kg of water is accepted as is, then relaxes across the
  chamber with mass conserved.
- A settled water column's bottom cell sits at about 1010 kg against a `maxMass` of 1000.

## The same event succeeds or fails depending on the message

Putting 500 kg of sand into a full water cell:

- by `MassEmission`: **refused**, `suceeded = 0`, nothing moves;
- by `ModifyCell` with vertical solid displacement: accepted, the water is displaced;
- by `ModifyCell` with replace-and-displace: the cell is replaced, the water is displaced.

All three conserve. Which one happens depends only on the message the building uses.

## Unstable solids fall after a random countdown

An unsupported cell of an `Unstable` element (sand, for example) does not fall the moment its
support goes. Each visit draws from the world's random stream against a stable-tick countdown,
and only when the countdown expires does the cell fall. In a running game it is handed to the
game as an `unstableCellInfo` (element, mass, temperature, germs, read before the cell is
cleared) and the cell is emptied; the falling object is the game's from then on. A headless
simulation has no game to hand it to, and moves the cell down within the grid instead. Checked
by `diffsim --scenario sand --gameside`.

## A cell with mass and heat capacity is never below 1 K

The last thing the conduction pass does to each pair is clamp both cells to `[1, 10000]` K. So
writing 0.5 K into a 100 kg granite cell comes back as exactly 1 K, while 1.5 K is untouched.

- The clamp runs **even when the pair exchanges nothing**. A pair with zero conductivity between
  it still reaches the clamp, so in a fully insulated world every cell is clamped every substep.
  (`modifycell`)
- It does **not** reach a cell with no mass, because the vacuum test comes first.
- It does **not** reach a pair where either cell has zero heat capacity: such a pair is left
  exactly as it was. The witness is the Unobtanium ring around a newly opened world, which has
  zero conductivity and zero heat capacity and stays at 0 K indefinitely (`vacrect`, see
  [CLUSTER.md](CLUSTER.md)).

No kernel can take a cell below 1 K on its own; only a message can. The clamp is a source and a
sink of energy, and the ledger counts it.

## A single pair moves the lighter cell at most a quarter of the way

One kilogram of oxygen with a single 400 K granite neighbour, whose unclamped step would take it
to 442 K, lands on exactly 325 K; with four hot neighbours it lands on exactly 400 K. Each pair
may move a cell by at most a quarter of the difference, one neighbour's share of four. See
[PHYSICS.md](PHYSICS.md).

## The gas displacement sweep's y bound is clamped the wrong way

The three-cell gas displacement sweep insets its x range correctly, but bounds its starting row
with `min` where `max` is meant. In a full-grid region the sweep therefore starts at row 0. The
game's library survives this only because a world's bottom rows are solid border and the sweep's
first test is "the source is a gas"; a world with gas in its two bottom rows would read before
the start of its arrays there. This simulation reproduces the bound and skips such cells safely.

## The spawn-ore list comes back sorted and merged

`spawnOreInfo` is sorted by cell and element, and entries for the same cell and element are
merged, when the frame is published. So its order is a property of the world, not of the kernel
that filled it. Details in [EMITTERS.md](EMITTERS.md).

## A solid element emitter creates matter

A vent emitting a solid never touches the grid: it hands the game ore for every reachable cell,
and the mass comes from whatever the building holds outside the simulation. The ledger records it
as `emitore`, outside the balance. Details in [EMITTERS.md](EMITTERS.md).

## Asking for a liquid can match a solid

The state test behind "consume any liquid" and "consume any gas" is
`(state & 3 & wanted) == wanted`, which a solid (3) passes for both. The only thing that stops a
pump set to "any liquid" from consuming rock is that the reachable-cell flood has already
discarded solid cells. Removing that step makes `econsume`'s liquid consumer drain granite within
four ticks.

## Radiation

- A constant emitter's steady state is set by the field's decay, not by the emitter: the two
  share the divisor `RADIATION_LINGER_RATE`, so a cell converges on `falloff * emitRads`.
- A constant emitter scans a box that is square in `radiusX`, so a tall, narrow emitter emits a
  clipped ellipse.
- Registering and modifying an emitter clamp `emitSpeed` against different fields.
- A radioactive element's 5 x 5 stencil weights sum to 8.6, not 1.

All are described in [RADIATION.md](RADIATION.md).

## A building's operating heat is applied once per region covering it

The components run once per region per substep, so a building inside two overlapping regions
exchanges twice and has its `operatingKilowatts * dt` charged **twice** in one frame. The game's
library does the same: in `diffsim --scenario buildinglap`, an 8 kW building under two regions
gains 160 kJ against 80 kJ under one, and both libraries report the same temperature to the last
digit. A single-asteroid world sends one region, so in practice this appears only where regions
overlap.

This simulation does the same.

## Deliberate difference: a cell with no heat capacity next to a building

Unobtanium (Neutronium) has a specific heat of zero. In the game's library, a building whose
extents reach a solid Neutronium tile divides by that zero: the cell's temperature becomes NaN,
and on the next substep so does the building's, after which every temperature check on that
building silently stops working. A Steam Turbine, whose extents reach one row below itself,
does this when it stands on the world's Neutronium floor.

This simulation skips a cell with no heat capacity in the building-to-cell exchange. That is also
the physical answer: a cell that can hold no heat can neither give nor take any, and Neutronium's
conductivity is zero anyway. Every other element has a nonzero specific heat, so the test never
fires for them, and the building scenarios (`building`, `buildingrun`, `buildingcool`,
`buildingvac`, `buildinglap`, `heatblock`, `insulated`) stay identical to the game's library.

## Limits of these measurements

- The `experiments` scenarios cover a single asteroid at one grid size, with no disease, no
  radiation and no conduits. Those subsystems are compared by their own `diffsim` scenarios.
- `experiments` measures the game's library alone. The comparison against this simulation is
  `diffsim`'s.
