# The save blob

`SIM_BeginSave(&size, x, y)` returns a pointer to a blob the sim still owns; the game
copies `size` bytes, length-prefixes them into the save file, and calls `SIM_EndSave`.
`SIM_HandleMessage(Load, size, blob)` takes it back and returns non-null on success.

**Everything depends on this round trip.** Worldgen runs 500 simulation frames per asteroid to
settle it, then calls `Sim.Save` and stores the result; `SaveLoader` feeds each blob back
through `Sim.Load` before a new game starts. A library that cannot round-trip the format cannot
begin a new game, let alone load an old one.

Implementation: `sim/saveblob.h`, and `World::ToBlob` / `World::FromBlob` in `sim/world.h`.
Tests: `driver/src/savefmt.cpp` (`--verify` decodes and re-encodes a synthetic blob and a real
one and requires both to come back byte-identical; `--roundtrip` saves, tears the simulation
down, boots a fresh one through the load path and compares every cell), plus `worldgen_test`
for the load path.

## The game's format (version 15)

Little-endian throughout. **No padding anywhere**: the blob is byte-packed, unlike the `Pack = 4`
message payloads. No compression.

```
offset  size  field
     0     8  magic "SIMSAVE\0"
     8     4  int32   version           15
    12     4  int32   width             padded: game width + 2
    16     4  int32   height            padded: game height + 2
    20     4  int32   x                 the x passed to SIM_BeginSave
    24     4  int32   y                 the y passed to SIM_BeginSave
    28     1  uint8   unknown; zero in every blob seen
    29        cell[width * height]      16 bytes each
      +       disease[width * height]    8 bytes each
      +       backwall[width * height]  12 bytes each
```

```c
struct SaveCell {      // 16 bytes
  int32_t elementHash;   // SimHashes value, NOT a table index
  float   temperature;   // K
  float   mass;          // kg
  float   radiation;     // rads
};
struct SaveDisease {   // 8 bytes
  int32_t diseaseHash;   // 0 when the cell is clean
  int32_t count;
};
struct SaveBackwall {  // 12 bytes
  int32_t elementHash;
  float   mass;
  float   temperature;
};
```

The total size is exactly `29 + width × height × 36`. A 636 × 404 asteroid is 638 × 406 padded,
259,028 cells, 9,325,037 bytes.

Things worth knowing:

- **Elements are stored by hash, not by table index.** `elementHash` is the `SimHashes` value:
  granite is `-105943486`, oxygen `-1528777920`, vacuum `758759285`. This makes a save
  independent of the element table's order, so a mod that adds elements does not invalidate old
  saves. `diseaseHash` works the same way.
- **The grid is stored padded, border included.** Game cell `(x, y)` is at padded index
  `(y + 1) × width + (x + 1)`. `SaveBlob::Index` does the conversion.
- **Insulation, strength and cell properties are not saved.** The game re-applies them as
  buildings load.
- **Radiation is the fourth float of the cell record.** It is zero in a world without radiation.
- **The world seed and the random stream's position are not saved.**
- **Byte 28** is zero in every blob seen. It is preserved on re-encode rather than assumed.
- **A backwall with no element** (index 0xFFFF, which worldgen sends for a template cell without
  one) is kept as "no element" while the game runs and published as 0xFFFF, but the format has
  no way to say "no element", so it is saved as Vacuum and reloads as a massless Vacuum
  backwall.

## Versions

This library reads and writes version 15, the game's format for build 744825. It does not
check the version field: a blob is accepted when its size is the version-15 size for its width
and height, and refused otherwise. The game's own library also reads blobs written by older
game builds (version 13, which has no `x`/`y`, no radiation and no backwall, and version 14);
version 14 has the version-15 layout, and this library reads it as such, but it refuses a
version 13 blob, because its size does not match.

`diffsim` cross-loads blobs in both directions: the game's library accepts a blob this one
wrote, and this one accepts the game's.

## The load path

Booting from a blob is a different message sequence from booting from cells. From
`SaveLoader.Load`:

```
SIM_Initialize
Elements_CreateTable
AllocateCells(width, height)      <- unpadded game dimensions
Disease_CreateTable
ClearUnoccupiedCells
Load(blob)                        <- once per asteroid
Start
```

Against `SimData_InitializeFromCells`, which needs no `AllocateCells` and no
`ClearUnoccupiedCells`. `savefmt.exe --roundtrip` drives exactly this sequence: it saves a
world, tears the sim down, boots a fresh one through the load path, and compares element,
mass, temperature and disease across every cell. The real sim round-trips with zero cells
wrong, so a replacement only has to be self-consistent to boot a new game.

## What `Load` does with a blob (measured)

All of this was measured by driving Klei's DLL offline (`driver/src/worldgen_test.cpp` and
smaller one-off probes), and none of it is visible to a saved game's own traffic.

**Where a blob goes.** It is written at its own header `x, y` (the arguments given to
`SIM_BeginSave`) into the grid `AllocateCells` made, border ring and all. A fresh cluster sends
one blob per world, each saved by worldgen at its world's offset. `DefineWorldOffsets` plays no
part in placement.

**What it rewrites.** Every cell taken from the blob, on both paths, in this order. Klei's
`CellRead` does the first three, and the rest of `Load`'s per-cell loop
does the others. Every row is a check in `worldgen_test`:

| in the blob | after `Load` |
| --- | --- |
| hash `0x0280cf79` / `0x2391c22b` | read as `0x987dac06` / `0x4c76ae31`, two renamed elements |
| temperature NaN or ±infinite | 293 K |
| mass NaN | 100 kg |
| radiation not finite | 0 |
| a hash the element table does not hold | Vacuum, 0 kg, 0 K, 0 rads (a load, not a refusal) |
| any element at or below 0 K, with or without mass | exactly 293 K |
| radiation at or below 0 | 0 |
| Vacuum or Void, any mass and temperature | 0 kg, 0 K, 0 rads |
| 0.5 K | unchanged |
| out of its element's range | **one** transition, below |

`SimData_InitializeFromCells` rewrites nothing.

**The load-time transition** is `DoLoadTimeStateTransition`. It is not the frame's
`DoStateTransition`, although it uses the same 3 K margin and 1.5 K overshoot:

* `T < lowTemp - 3` with a low transition: the low element, at `T + 1.5`. Tested first.
* otherwise `T > highTemp + 3` with a high transition: the high element, at `T - 1.5`.
* **No mass gate.** 0 kg of Granite at 5000 K loads as 0 kg of Magma.
* **No transition ore.** The whole mass stays in the cell and nothing is announced.
* **Once.** Ice at 5000 K loads as Water at 4998.5 K, not as Steam.
* It runs after the 293 K rewrite, so Water at 0 K loads as Water at 293 K, not as Ice.

A worldgen blob settled for 500 frames still has out-of-range cells: loading Klei's own 8-world
retail cluster transitions **14** of them, all trace masses (6e-9 to 0.06 kg) of Chlorine and CO2
in the bottom 36 rows: gas to liquid, liquid to solid, and one liquid Chlorine cell at 246.5 K to
gas. With the transition, both DLLs give element, mass, temperature and radiation bitwise
identical in all 202,808 cells; without it, exactly those 14 differ. This matters because a world's border ring is
0 K Neutronium under 9999 kg of 0 K Vacuum, and on the cluster path it lands inside the grid.

**A backwall with no element.** Backwall index 0xFFFF (what worldgen sends for a template cell
with no backwall) is kept as "no element", published as 0xFFFF and **saved as Vacuum**. Index 0 is
a real element (Crushed Ice) and is saved as itself. This format has no way to say "no element",
so a reload turns it into a massless Vacuum backwall.
