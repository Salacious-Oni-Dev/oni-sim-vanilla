# oni-sim-vanilla

A replacement for `SimDLL.dll`, the native simulation library of Oxygen Not Included, that
behaves like the game's own and adds nothing.

It is an independently implemented simulation library focused on behavioral compatibility with
the game's simulation interfaces and behavior. Its implementation is informed by analysis of the
shipped game's observable behavior and public interfaces. This repository contains its own
source code and does not include or distribute Klei's source code, game binaries, or
proprietary game assets.

Where a comment names one of the game's own functions, types or globals (written as Klei's
`UpdateLiquid`, for example), the name is the one in `SimDLL.pdb`, the symbol file Klei ships
with the game beside `SimDLL.dll`, or in the game's managed code.

**Frozen.** This is the library as it stood before the SDK's extensions were added. It is kept
for anyone who wants to build on a plain, readable simulation library rather than on the SDK's
extended one. It receives fixes for differences from the game's own `SimDLL.dll` and nothing
else. Development continues in
[oni-sim-replacement](https://github.com/Salacious-Oni-Dev/oni-sim-replacement), which has the
extension surface that the SDK's
[framework](https://github.com/Salacious-Oni-Dev/oni-framework-api) and
[mods](https://github.com/Salacious-Oni-Dev/oni-flagship-mods) need: they use that library,
not this one.

**Status: alpha.** **Windows only:** the simulation library is a Windows DLL, so it needs the
game's Windows version.

## What it does

- **Runs the game's simulation.** It accepts every message the game sends to its own SimDLL and
  publishes the same data back. It exports every function the game's library exports, plus two
  read-only debug exports the offline tools use.
- **Runs the frame on a worker thread**, overlapped with the game's managed code and rendering,
  as the game's own library does (see [Threading](docs/THREADING.md)).
- **Carries the game's event profiler**, KProfiler, which the game's library implements and the
  shipped game never switches on (see [KProfiler](docs/KPROFILER.md)).

It differs from the game's library in one deliberate place: a building next to a cell that has
no heat capacity (Neutronium) skips that cell, where the game's library divides by zero and
turns the cell's and then the building's temperature into NaN. See
[Quirks](docs/QUIRKS.md#deliberate-difference-a-cell-with-no-heat-capacity-next-to-a-building).

`driver/diffsim` compares it with the game's own library: both load into one process, run the
same worlds in lockstep, and every array the game reads is compared after every tick. At
release, every scenario passed against the game's library for build 744825, with a corpus
recorded from that build.

## What gets replaced, and how to undo it

Installing replaces one file in the game:

    <install>/OxygenNotIncluded_Data/Plugins/x86_64/SimDLL.dll

No other game file is changed. To go back to the original, put Klei's `SimDLL.dll` back, or use
Steam's **Verify integrity of game files**, which restores it. A game update also restores it.

Because you are replacing a native library, you should be able to check what you run. The full
source is here, and you can build the DLL yourself.

## Installing

A release carries a built `SimDLL.dll` and a `SHA256SUMS` file. Check the DLL's SHA-256 against
`SHA256SUMS` before installing it:

```powershell
Get-FileHash SimDLL.dll -Algorithm SHA256
```

Then:

1. Close the game.
2. In `<install>/OxygenNotIncluded_Data/Plugins/x86_64/`, rename `SimDLL.dll` to
   `SimDLL.dll.vanilla`.
3. Copy the new `SimDLL.dll` into that folder.

To uninstall, delete the copied `SimDLL.dll` and rename `SimDLL.dll.vanilla` back, or verify the
game files in Steam.

The release DLL is built for game build 744825. After a game update, rebuild from source (see
below): the build checks the game's message layouts and fails if they changed.

## Building

Requirements:
- mingw-w64 (`x86_64-w64-mingw32-g++`)
- bash, or Windows PowerShell (see Building on Windows below)
- Python 3.8 or later
- an installed copy of the game

On Linux or WSL:

```sh
ONI_GAME=/path/to/OxygenNotIncluded ./sim/build.sh
```

The output is `sim/build/SimDLL.dll`. `ONI_GAME` is needed only for the first build.

The game's own message structs are part of the game, so this repository does not carry them.
On the first build, `tools/gen_sim_abi.py` reads their layouts from your installed
`Assembly-CSharp.dll` and writes `abi/sim_abi.h`. It reads only .NET metadata: type names,
field types, packing and enum constants. Every generated struct carries a size check, so a game
update that changes a layout fails the build instead of corrupting messages at run time. To
regenerate after an update, delete `abi/sim_abi.h` and build again with `ONI_GAME` set.

Two builds of the same source differ in a few bytes of the DLL's header (its timestamps and
checksum), so compare a build of your own with the release by running it, not by its hash.

### Building on Windows

`sim\build.ps1` does what `sim/build.sh` does, in Windows PowerShell, without WSL or bash. It
builds `sim\build\SimDLL.dll`, and on the first build it generates `abi\sim_abi.h` from your
game install.

Install these once, from PowerShell, then open a new PowerShell window so they are on `PATH`:

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT   # g++ (MinGW-w64)
winget install Python.Python.3.12                 # Python 3, for the first build only
```

If `python` opens the Microsoft Store instead of running, that is the Store placeholder, not
Python: install Python with the `winget` line above.

Then:

```powershell
git clone https://github.com/Salacious-Oni-Dev/oni-sim-vanilla.git
cd oni-sim-vanilla
powershell -ExecutionPolicy Bypass -File sim\build.ps1 -OniGame "C:\Program Files (x86)\Steam\steamapps\common\OxygenNotIncluded"
```

`-OniGame` is the folder that contains `OxygenNotIncluded_Data`. It is only needed while
`abi\sim_abi.h` does not exist yet.

The Windows build adds `-static` to the compiler flags so that `SimDLL.dll` does not depend on
`libwinpthread-1.dll`, which the WinLibs compiler would otherwise link dynamically and which the
game does not ship. The other flags are those of `sim/build.sh`.

Releases of `SimDLL.dll` are built with `sim/build.sh`. A Windows build links a different C
runtime (UCRT rather than msvcrt), and its results have not been compared with the release
build's tick for tick, so treat it as a development build.

## Testing

`driver/` holds the offline test tools. They run the DLL with no game process, and most of them
replay a corpus of real game messages, which you record from your own game with the passthrough
shim in `shim/`. See `driver/README.md`.

## Documentation

Every part of the library is described in `docs/`, as the game's own simulation behaves and as
this library reproduces it. Each page names the source file that implements it and the
`diffsim` scenarios that check it against the game's library.

| document | covers |
|---|---|
| [Interface](docs/INTERFACE.md) | the exports, the callback, what `GameDataUpdate` hands the game, every message and what happens to it, and the boot sequences |
| [Message drain](docs/DRAIN.md) | when a message takes effect, the order categories drain in, and the cell-energy, insulation and strength handlers |
| [Physics](docs/PHYSICS.md) | the frame and substep order, conduction, gas and liquid flow, the post-process pass and its random stream, state changes, sublimation, and building heat exchange |
| [ModifyCell](docs/CELLMOD.md) | the add, remove and replace paths, the deletion tail, and the liquid displacement primitives |
| [Emitters and consumers](docs/EMITTERS.md) | element and disease emitters and consumers, `MassConsumption`, `MassEmission` and `ConsumeDisease` |
| [Element chunks](docs/CHUNKS.md) | heat exchange between off-grid matter and the cell it sits in |
| [Disease](docs/DISEASE.md) | how germs spread, grow, die, and travel with the matter that carries them |
| [Radiation](docs/RADIATION.md) | the radiation field, its decay and sources, and radiation emitters |
| [Conduit temperatures](docs/CONDUITS.md) | pipe contents against their building |
| [Cluster messages](docs/CLUSTER.md) | world zones, world offsets, the sunlight texture, and opening a new world in the grid |
| [Backwall](docs/BACKWALL.md) | the material behind a cell and when the simulation reports it out of range |
| [Projection](docs/PROJECTION.md) | how simulation state becomes the arrays and textures the game reads each frame |
| [Save format](docs/SAVE-FORMAT.md) | the save blob, its versions, and what a load does with it |
| [Threading](docs/THREADING.md) | the frame worker, which calls wait for it, and why it does not change results |
| [Quirks](docs/QUIRKS.md) | measured behaviour of the game's simulation that is surprising or easy to get wrong, and the one deliberate difference |
| [Mass ledger](docs/LEDGERS.md) | the mass books this library keeps, and what they do not count |
| [KProfiler](docs/KPROFILER.md) | the in-DLL event profiler, its byte stream, and how to take and read a capture |

The source files carry the detail behind each page: the comments on every kernel describe the
behaviour it reproduces and how that was measured.

## Layout

| path | contents |
|---|---|
| `sim/` | the simulation library |
| `abi/` | `sim_abi.h`, generated by the first build and not committed |
| `shim/` | a passthrough SimDLL that records the game's messages into a test corpus |
| `driver/` | offline test tools |
| `tools/` | the header generator and a KProfiler capture reader |

## License

Mozilla Public License 2.0, see `LICENSE`. If you distribute a modified build of this DLL, you
must publish the source of the files you changed. The license applies file by file, so a mod
that loads or calls the DLL is not affected by it.

## Credits

Oxygen Not Included is developed and published by Klei Entertainment. This project is not
affiliated with or endorsed by Klei.

Development of this project uses AI coding assistants. All changes are reviewed and released
by the maintainer.
