// Per-kernel timing for the replacement sim.
//
// The scored scenarios elsewhere are 384 cells against a real asteroid's 98,304; this times
// the kernels at real scale, so tuning starts from numbers.
//
// It calls the kernels directly out of `sim/` rather than going through the DLL. That is
// deliberate: the kernels are header-only, the DLL boundary is one call per *frame* and
// would hide the per-substep split entirely, and timing them in-process means the sim's
// own sources stay untouched by this pass. The flags match `sim/build.sh`, so the codegen
// is the codegen that ships.
//
// Usage:
//   bench.exe --corpus <corpus.bin> [--scenario asteroid|granite|all]
//             [--width W --height H] [--ticks N] [--dt SECONDS]
//             [--regions full|clamped|quarter|cluster:N|rows:N] [--settle N]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../sim/physics.h"
#include "../../sim/projection.h"
#include "../../sim/textures.h"
#include "../../sim/world.h"
#include "harness.h"
#include "simcheck.h"

using namespace oni_sim;
using oni_bench::SeedWorld;
using oni_bench::Tables;

namespace {

using Clock = std::chrono::steady_clock;

// std::chrono::steady_clock on mingw is QueryPerformanceCounter, which is sub-microsecond
// — fine for a kernel that is expected to land in the hundreds of microseconds, and the
// reason the per-substep samples are kept individually rather than summed. The interesting
// number is the *worst* substep, not the mean one: a mean that fits in a frame and a
// worst case that does not still hitches the game once every N frames.
struct Samples {
  explicit Samples(const char* n) : name(n) {}
  const char* name;
  std::vector<double> ms;

  void Add(double v) { ms.push_back(v); }
  double Total() const {
    double s = 0;
    for (double v : ms) s += v;
    return s;
  }
  double Quantile(double q) const {
    if (ms.empty()) return 0;
    std::vector<double> s = ms;
    std::sort(s.begin(), s.end());
    size_t i = static_cast<size_t>(q * (s.size() - 1) + 0.5);
    return s[i];
  }
  double Min() const { return ms.empty() ? 0 : *std::min_element(ms.begin(), ms.end()); }
  double Max() const { return ms.empty() ? 0 : *std::max_element(ms.begin(), ms.end()); }
};

template <typename F>
double TimeIt(F&& f) {
  const auto a = Clock::now();
  f();
  const auto b = Clock::now();
  return std::chrono::duration<double, std::milli>(b - a).count();
}

// What is actually in the world. A timing without this is unreadable: a number from a
// world that is 99 % solid says nothing about a world with water in it, and "realistic"
// is a claim that has to be checkable.
struct Composition {
  size_t solid = 0, liquid = 0, gas = 0, vacuum = 0, massless = 0;
};

Composition Describe(const World& w, const ElementTable& t) {
  Composition c;
  for (size_t i = 0; i < w.GameCount(); ++i) {
    const PhaseEntry& e = w.Phase(w.Padded(i));
    if (e.mass <= 0.0f) ++c.massless;
    switch (t.Phase(e.element)) {
      case kStateSolid: ++c.solid; break;
      case kStateLiquid: ++c.liquid; break;
      case kStateGas: ++c.gas; break;
      default: ++c.vacuum; break;
    }
  }
  return c;
}

// A digest of the whole grid, printed beside the composition at the end of a run.
//
// The composition is four counts, so two different worlds agree on it constantly. This is
// every game cell's element, mass and temperature, bit for bit, through FNV-1a — which makes
// `bench` a detector for a change that is supposed to leave the world alone, at a region
// count `diffsim` cannot reach. It earned its place immediately: the per-region snapshot
// rectangles were verified at cluster:12 against the whole-grid build here, and the margin
// ablation that proves the rectangle is big enough is measured with this digest, because the
// offline suite reports the same md5 whether the margin is 3 or 0.
//
// Float bits rather than the values, because a digest that rounds is a digest that agrees
// when the sim does not.
uint64_t WorldDigest(const World& w) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](const void* p, size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
  };
  for (size_t i = 0; i < w.GameCount(); ++i) {
    const PhaseEntry& e = w.Phase(w.Padded(i));
    mix(&e.element, sizeof(e.element));
    mix(&e.mass, sizeof(e.mass));
    mix(&e.temperature, sizeof(e.temperature));
  }
  return h;
}

// Klei's own cell validator, run over the world a scenario left behind. `simcheck.h` holds
// the transcription and what each class means; this is the walk.
//
// IT ANSWERS A QUESTION THE DIGEST ABOVE CANNOT. `WorldDigest` and the composition are both
// *relative*: they say that something moved, never that what is there is impossible. A NaN
// temperature that has been in a kernel since the day it was written is a stable digest, and
// the key goes green on it forever. The worked example is an `ElementConsumer` publishing a
// NaN through a whole green gate, found by a human opening the DevTools overlay in a running
// game. This walk is that overlay, offline, over every game cell, with no game and no Klei DLL
// in the room. It needs no game content at all, so unlike `diffsim` it is a check public CI
// could run.
simcheck::Counts SimCheckCounts(const World& w, const ElementTable& t) {
  simcheck::Counts c;
  for (size_t i = 0; i < w.GameCount(); ++i) {
    const PhaseEntry& e = w.Phase(w.Padded(i));
    c.Add(t.At(e.element), e.mass, e.temperature);
  }
  return c;
}

// ------------------------------------------------------------------- active regions

// The game sends region bounds per asteroid every frame and clamps maxY to
// HeightInCells - 1, so `clamped` is what a real single-asteroid map actually looks like
// and `full` is the state a sim that has never been told otherwise is in. `quarter` is not
// realistic; it is here to price the active-region mechanism itself, which has never been
// quantified.
std::vector<int32_t> Regions(const char* mode, int w, int h) {
  if (!strcmp(mode, "full")) return {0, 0, w, h};
  if (!strcmp(mode, "quarter")) return {w / 4, h / 4, w / 2, h / 2};
  // `cluster:N` is the shape Spaced Out actually sends. `Game.UnsafeSim200ms` builds one
  // `SimActiveRegion` per DISCOVERED WORLD, so the region count is the asteroid count: 1 in
  // the base game and 5-12 in a late cluster save. Every other mode here sends exactly one,
  // which is why a per-region cost that scales with the region COUNT rather than with the
  // region's area could not be seen by anything in this repository.
  //
  // The strips are disjoint and side by side because the cluster grid is: each world owns a
  // rectangle at its own offset and no two overlap. Disjoint is the case that matters — a
  // pair never crosses a region edge, so the per-cell results are the same however many
  // strips one grid is cut into, and any cost that moves with N is overhead by construction
  // rather than work.
  if (!strncmp(mode, "cluster:", 8)) {
    int n = atoi(mode + 8);
    if (n < 1) n = 1;
    std::vector<int32_t> out;
    for (int i = 0; i < n; ++i) {
      const int x0 = static_cast<int>(static_cast<int64_t>(w) * i / n);
      const int x1 = static_cast<int>(static_cast<int64_t>(w) * (i + 1) / n);
      out.push_back(x0);
      out.push_back(0);
      out.push_back(x1);
      out.push_back(h - 1);
    }
    return out;
  }
  // The same cut the other way. `cluster:N` splits in x, so every strip spans the full height
  // and nothing it does can exercise a per-region bound in y. The cluster grid is
  // two-dimensional — worlds sit above and beside each other — so this is a real shape too,
  // and it is the one that puts a region edge across the row a sweep reads past.
  if (!strncmp(mode, "rows:", 5)) {
    int n = atoi(mode + 5);
    if (n < 1) n = 1;
    std::vector<int32_t> out;
    for (int i = 0; i < n; ++i) {
      const int y0 = static_cast<int>(static_cast<int64_t>(h - 1) * i / n);
      const int y1 = static_cast<int>(static_cast<int64_t>(h - 1) * (i + 1) / n);
      out.push_back(0);
      out.push_back(y0);
      out.push_back(w);
      out.push_back(y1);
    }
    return out;
  }
  return {0, 0, w, h - 1};  // clamped: what the game sends
}

// ------------------------------------------------------------------- the run

struct Result {
  Samples conduction{"StepConduction"};
  Samples state{"StepStateChange"};
  Samples sublimation{"StepPostProcess"};
  Samples pressure{"StepGasPressure"};
  Samples displace{"StepGasDisplacement"};
  Samples flow{"StepFlow"};
  Samples liqdisp{"StepLiquidDisplacement"};
  Samples zeromassless{"ZeroMasslessCells"};
  Samples project{"Project"};
  Samples textures{"FillPropertyTextures"};
  Samples substep{"substep (8 kernels)"};
  Samples frame{"frame (substeps + projection)"};
  Composition before, after;
  // What the projection actually announced, summed over the run. `Project` publishes one
  // `SubstanceChangeInfo` per set bit in the substance-touched bitmap, so this is the number
  // the game would have to consume — and the number that goes wrong the moment the bitmap is
  // not cleared per frame. It is a count, not a time; see the clear at the end of the frame.
  size_t published = 0;
  size_t published_frames = 0;
};

// `--verify`: recompute the projection and the property textures from scratch every frame
// and compare, byte for byte, against the incremental ones. Both are caches now — `Project`
// copies only the pass-through cells that were written, `FillPropertyTextures` only the
// cells whose element moved or which hold liquid — and `diffsim` cannot check either on its
// own: it does not compare four of the projection arrays at all, and the texture diffs it
// does report against Klei move run to run with Klei's frame pacing. The question a cache
// needs answered is not "does this match Klei", it is "does this match what the code would
// have written without the cache", and that needs no Klei in the loop.
//
// Roughly triples the frame, so it is a correctness switch and never on while timing.
bool g_verify = false;
bool g_sunlight = false;
int g_verify_fails = 0;
// Cells Klei's own validator says cannot exist: a vacuum cell holding a temperature or mass.
// Counted across every scenario in the run and reported by `main`, because a fault this
// arm finds is a fault whether or not `--verify` was asked for.
int g_simcheck_fails = 0;
// `--settle N`: ticks run before the measured ones, to put the world in the regime the
// measurement is about rather than the transient it starts in. A freshly seeded asteroid
// spends its first thousands of ticks equilibrating -- seeded gas finding the vacuum,
// temperatures finding each other -- and every number taken over that stretch is an average
// of two different worlds, of which an idle colony only ever runs the second.
//
// It is a FIXED COUNT, not a convergence test: "settled" means exactly "N ticks have run".
// Nothing at run time checks that the world has stopped changing, so the count has to be
// chosen by measuring the curve.
int g_settle = 0;

void RunScenario(const char* label, const SeedWorld& seed, const Tables& tables,
                 const char* region_mode, int ticks, float dt) {
  ElementTable elements;
  DiseaseTable diseases;
  if (!elements.Load(tables.elements.data(), tables.elements.size()) ||
      !diseases.Load(tables.diseases.data(), tables.diseases.size())) {
    printf("failed to load tables\n");
    return;
  }

  World world;
  const std::vector<uint8_t> payload = oni_bench::WorldPayload(seed);
  if (!world.InitializeFromCells(payload.data(), payload.size(), elements, diseases)) {
    printf("failed to seed world\n");
    return;
  }
  const std::vector<int32_t> region = Regions(region_mode, seed.width, seed.height);
  world.SetActiveRegions(region.data(), region.size() / 4);
  // `--sunlight` declares one world covering the grid, which is what a real game sends and
  // what switches the sunlight texture on. Without it `ComputeSunlight` returns on its first
  // line, so the texture number here would not include a pass the live game always pays for.
  if (g_sunlight) {
    std::vector<World::WorldOffset> worlds(1);
    worlds[0].x = 0;
    worlds[0].y = 0;
    worlds[0].w = world.GameWidth();
    worlds[0].h = world.GameHeight();
    world.SetWorldOffsets(std::move(worlds));
    world.PromoteWorldOffsets();
  }

  ProjectionBuffers buffers;
  buffers.Allocate(world.GameCount());
  PropertyTextureBuffers textures;
  textures.Allocate(world.GameCount());
  ProjectionEvents events;
  std::vector<StateChangeOre> ores;
  ProjectionBuffers ref_buffers;
  PropertyTextureBuffers ref_textures;
  if (g_verify) {
    ref_buffers.Allocate(world.GameCount());
    ref_textures.Allocate(world.GameCount());
  }

  Result r;
  r.before = Describe(world, elements);

  // The very first Project is the one that emits a change notification for every cell in
  // the world, so it is not comparable with the ones that follow; run it outside the
  // measurement the way the real first frame does.
  Project(world, elements, &buffers, &events, true);
  events.Clear();

  float carry = 0.0f;
  size_t substeps_run = 0;
  uint16_t rotation = 0;
  int32_t pressure_dir = -1;
  Composition settled = r.before;
  // NEGATIVE TICKS ARE THE SETTLE, and they run the same body rather than a copy of it.
  // `bench` already has to repeat `StepPhysics`'s frame by hand, and a second loop would be a
  // second place for the two to drift, so the settle is this loop with its recording switched
  // off. `carry`, `rotation` and `pressure_dir` run straight through, which is what makes
  // `--settle S --ticks T` end on exactly the world `--ticks S+T` ends on.
  for (int tick = -g_settle; tick < ticks; ++tick) {
    const bool measuring = tick >= 0;
    if (tick == 0) settled = Describe(world, elements);
    const double frame_ms = TimeIt([&] {
      // `StepPhysics` clears the flow accumulator once a frame, outside the substep loop,
      // and a bench that skips it does not just mis-time the texture pass — it lets the
      // touched-cell list grow for the whole run and prices `FillPropertyTextures` at three
      // hundred times what it costs. This is the first line of the frame for the same
      // reason it is there.
      world.ClearFlow();
      const int substeps = SubstepsForFrame(dt, &carry);
      for (int s = 0; s < substeps; ++s) {
        // Same order as `StepPhysics`: gas pressure before liquid flow, and post-process
        // after both. The order is not free even for timing — each kernel sees a different
        // grid depending on where it sits, and on this world that changes how much work
        // its early-outs skip.
        pressure_dir = -pressure_dir;
        // The region loop `StepPhysics` runs around this whole body. Every mode but
        // `cluster:N` sends exactly one region, so this is a single iteration and the
        // measurement is the one it always was; `--regions cluster:12` is the only way
        // anything here can price a cost that scales with the region COUNT rather than with
        // the area swept.
        //
        // The other thing `StepPhysics` does around this body that has to be repeated by
        // hand: the region's projection mark. Without it the only mark is the one
        // `StepGasDisplacement` posts for itself, which happens to cover the region on this
        // world — so `Project` would still verify clean while being timed against a bound
        // narrower than the one the DLL uses.
        double c = 0.0, t = 0.0, g = 0.0, d = 0.0, f = 0.0, l = 0.0, u = 0.0, z = 0.0;
        for (size_t ri = 0; ri < world.RegionCount(); ++ri) {
          const World::PaddedRect& pr = world.PaddedRegionInclusive(ri);
          world.MarkProjectDirtyRect(pr.x0, pr.y0, pr.x1, pr.y1);
          c += TimeIt([&] { StepConduction(&world, elements, ri); });
          t += TimeIt([&] { StepStateChange(&world, elements, &ores, ri); });
          g += TimeIt([&] { StepGasPressure(&world, elements, diseases, pressure_dir, ri); });
          d += TimeIt([&] {
            StepGasDisplacement(&world, elements, pressure_dir, ri, &diseases);
          });
          // The germ table as well, so the liquid sweeps price the germs they carry.
          f += TimeIt([&] {
            StepFlow(&world, elements, s == 0, rotation, ri, nullptr, nullptr, false, &diseases);
          });
          // The second liquid sweep and the flow-texture snapshot that closes the liquid
          // section. `bench` does not call `StepPhysics`, so every one of these has to be
          // repeated here by hand or the A/B prices a kernel that never ran.
          l += TimeIt([&] {
            StepLiquidDisplacement(&world, elements, pressure_dir, rotation, ri, &diseases);
            world.SnapshotFlowElements();
          });
          u += TimeIt([&] {
            StepPostProcess(&world, elements, 0, nullptr, ri, nullptr, nullptr, false, nullptr,
                            {}, &diseases);
          });
          // THE WHOLE-GRID PASS `bench` NEVER CALLED. `SimData::UpdateData` (sim/simdll.cpp)
          // runs it exactly here — inside the region loop, immediately after `StepPostProcess`
          // and before the disease sweep, unconditionally, over the entire padded grid.
          // `bench` omitted it from the day it was written, so every substep this harness has
          // ever timed was missing a full-grid kernel.
          //
          // It changes no state on either scenario: a settled world has no massless non-solid
          // cells left for it to find. That is the point rather than a reprieve — a whole-grid
          // scan that almost never does anything is exactly the kind of cost that hides when
          // it is not being run. It cannot be narrowed to the region either: the function's own
          // note in sim/simdll.cpp says a mover may empty a cell outside the rectangle it is
          // sweeping, which is why the DLL runs the whole grid once per region rather than once
          // per substep.
          z += TimeIt([&] { ZeroMasslessCells(&world, elements); });
        }
        if (measuring) {
          r.conduction.Add(c);
          r.state.Add(t);
          r.sublimation.Add(u);
          r.pressure.Add(g);
          r.displace.Add(d);
          r.flow.Add(f);
          r.liqdisp.Add(l);
          r.zeromassless.Add(z);
          r.substep.Add(c + t + g + d + u + f + l + z);
          ++substeps_run;
        }
        ++rotation;
        ores.clear();
      }
      if (g_verify && measuring) {
        // Not during the settle: this is a write into the world, and a settle that wrote
        // would hand the measured ticks a different world from the one it claims to settle.
        //
        // A cell whose element and mass never move but whose impermeable bit does is the
        // one input to the projection's solidity fast path that does not arrive with the
        // cell — it arrives as a `CellPropertiesMessage`, which no scenario in the suite
        // sends, so `kPrevRecheck` would otherwise go untested. Toggling one cell per tick
        // puts it under the full-recompute oracle below. Only under --verify: it is a write
        // into the world, and the timed runs must not have one.
        // Four cells a tick, spread across the grid, because most of an asteroid is
        // already solid and a cell that was solid anyway proves nothing.
        const size_t cells = world.GameCount();
        for (int k = 0; k < 4; ++k) {
          const size_t game = (static_cast<size_t>(tick) * 7919 + k * (cells / 4)) % cells;
          world.MutableProperties(world.Padded(game)) ^= kSolidImpermeable;
        }
      }
      const double pj = TimeIt([&] { Project(world, elements, &buffers, &events, false); });
      const double tx =
          TimeIt([&] { FillPropertyTextures(world, elements, buffers, &textures); });
      if (measuring) {
        r.project.Add(pj);
        r.textures.Add(tx);
      }
      if (g_verify && measuring) {
        ProjectionEvents ref_events;
        Project(world, elements, &ref_buffers, &ref_events, true);
        ref_textures.primed = false;
        FillPropertyTextures(world, elements, ref_buffers, &ref_textures);
        auto same = [&](const char* what, const void* a, const void* b, size_t bytes) {
          if (memcmp(a, b, bytes) == 0) return;
          printf("  VERIFY tick %d: %s differs from a full recompute\n", tick, what);
          ++g_verify_fails;
        };
        same("element", buffers.element.data(), ref_buffers.element.data(),
             buffers.element.size() * 2);
        same("mass", buffers.mass.data(), ref_buffers.mass.data(),
             buffers.mass.size() * 4);
        same("temperature", buffers.temperature.data(), ref_buffers.temperature.data(),
             buffers.temperature.size() * 4);
        same("properties", buffers.properties.data(), ref_buffers.properties.data(),
             buffers.properties.size());
        same("insulation", buffers.insulation.data(), ref_buffers.insulation.data(),
             buffers.insulation.size());
        same("strength", buffers.strength.data(), ref_buffers.strength.data(),
             buffers.strength.size());
        same("radiation", buffers.radiation.data(), ref_buffers.radiation.data(),
             buffers.radiation.size() * 4);
        same("disease_idx", buffers.disease_idx.data(), ref_buffers.disease_idx.data(),
             buffers.disease_idx.size());
        same("disease_count", buffers.disease_count.data(),
             ref_buffers.disease_count.data(), buffers.disease_count.size() * 4);
        same("backwall_element", buffers.backwall_element.data(),
             ref_buffers.backwall_element.data(), buffers.backwall_element.size() * 2);
        same("backwall_mass", buffers.backwall_mass.data(),
             ref_buffers.backwall_mass.data(), buffers.backwall_mass.size() * 4);
        same("backwall_temperature", buffers.backwall_temperature.data(),
             ref_buffers.backwall_temperature.data(),
             buffers.backwall_temperature.size() * 4);
        // Solidity appears in none of the arrays above — it leaves the sim only as
        // `solidInfo` events — so without this line the projection's solidity fast path
        // has no oracle here at all. `previous` is where it is decided and kept, and a
        // full recompute writes exactly the same byte for every cell.
        same("previous", buffers.previous.data(), ref_buffers.previous.data(),
             buffers.previous.size());
        same("tex.flow", textures.flow.data(), ref_textures.flow.data(),
             textures.flow.size() * 4);
        same("tex.liquid", textures.liquid.data(), ref_textures.liquid.data(),
             textures.liquid.size());
        same("tex.liquidData", textures.liquid_data.data(), ref_textures.liquid_data.data(),
             textures.liquid_data.size());
        same("tex.materialData", textures.material_data.data(),
             ref_textures.material_data.data(), textures.material_data.size());
        same("tex.sunlight", textures.exposed_to_sun.data(),
             ref_textures.exposed_to_sun.data(), textures.exposed_to_sun.size());
      }
      if (measuring) {
        r.published += events.substance.size();
        ++r.published_frames;
      }
      events.Clear();
      // AND THE TOUCH BITMAP WITH THEM. `ClearFrameEvents` (sim/simdll.cpp) clears
      // `projection_events` and calls `World::ClearSubstanceTouched()` on consecutive lines,
      // once a frame, because a cell's "a substance was written here" flag is a fact about ONE
      // frame: `CopySimDataToGame` drains it and the next frame has to earn it again. `bench`
      // repeats that frame by hand and cleared the first of those two and not the second, from
      // the day it was written.
      //
      // `Project` reads the bitmap per cell and publishes a `SubstanceChangeInfo` for every set
      // bit, so without this the projection republished the CUMULATIVE set of cells the run had
      // ever written a substance into, every frame — a count that climbs towards the size of
      // the grid and then looks like a steady state. The `projection publishes` line above is
      // what it costs: see the commit that added this clear for the before and after.
      world.ClearSubstanceTouched();
    });
    if (measuring) r.frame.Add(frame_ms);
  }
  r.after = Describe(world, elements);
  const uint64_t digest = WorldDigest(world);

  const size_t cells = world.GameCount();
  char settle_note[32] = "";
  if (g_settle > 0) snprintf(settle_note, sizeof(settle_note), "  settle=%d", g_settle);
  printf("\n=== %s  %dx%d = %zu cells  regions=%s%s  %d ticks @ dt=%.2f (%zu substeps) ===\n",
         label, seed.width, seed.height, cells, region_mode, settle_note, ticks, dt,
         substeps_run);
  printf("  composition  solid %zu  liquid %zu  gas %zu  vacuum %zu  (massless %zu)"
         "  ->  solid %zu  liquid %zu  gas %zu  vacuum %zu\n",
         r.before.solid, r.before.liquid, r.before.gas, r.before.vacuum, r.before.massless,
         r.after.solid, r.after.liquid, r.after.gas, r.after.vacuum);
  // With a settle in the middle, "seed -> finish" hides the point: the measured ticks started
  // from this world, not from the seed.
  if (g_settle > 0) {
    printf("  settled      solid %zu  liquid %zu  gas %zu  vacuum %zu  (massless %zu)"
           "  -- where the %d measured ticks began, after %d untimed\n",
           settled.solid, settled.liquid, settled.gas, settled.vacuum, settled.massless, ticks,
           g_settle);
  }
  printf("  world digest  %016llx  (element, mass and temperature of every game cell)\n",
         static_cast<unsigned long long>(digest));
  const simcheck::Counts check = SimCheckCounts(world, elements);
  printf("\n  Klei's SimCheckErrorMap, over the world this run left behind:\n");
  check.Print("simcheck");
  for (int i = 0; i < simcheck::kClassCount; ++i) {
    const simcheck::Class cl = static_cast<simcheck::Class>(i);
    if (check.n[i]) printf("    %-8s %s\n", simcheck::Name(cl), simcheck::Meaning(cl));
  }
  // Yellow and blue are the two classes no world this harness builds has any business
  // showing: a vacuum cell holding a temperature, or holding mass. They are a hard fail
  // rather than a printed count, because unlike red they are not lit by the world border.
  if (check.VacuumFaults() != 0) {
    printf("  SIMCHECK FAIL: %llu vacuum cells hold a temperature or mass (yellow %llu, "
           "blue %llu).\n",
           static_cast<unsigned long long>(check.VacuumFaults()),
           static_cast<unsigned long long>(check.n[simcheck::kYellow]),
           static_cast<unsigned long long>(check.n[simcheck::kBlue]));
    ++g_simcheck_fails;
  }

  printf("  projection publishes  %.1f substance changes a frame  (%.2f%% of the grid)\n",
         r.published_frames ? static_cast<double>(r.published) / r.published_frames : 0.0,
         r.published_frames && cells
             ? 100.0 * static_cast<double>(r.published) / r.published_frames / cells
             : 0.0);

  const double total = r.substep.Total() + r.project.Total() + r.textures.Total();
  printf("  %-22s %8s %9s %9s %9s %9s %8s\n", "kernel", "calls", "min ms", "med ms",
         "p95 ms", "max ms", "share");
  auto row = [&](const Samples& s, bool share) {
    printf("  %-22s %8zu %9.3f %9.3f %9.3f %9.3f %7.1f%%\n", s.name, s.ms.size(), s.Min(),
           s.Quantile(0.5), s.Quantile(0.95), s.Max(),
           share && total > 0 ? 100.0 * s.Total() / total : 0.0);
  };
  row(r.conduction, true);
  row(r.state, true);
  row(r.sublimation, true);
  row(r.pressure, true);
  row(r.displace, true);
  row(r.flow, true);
  row(r.liqdisp, true);
  row(r.zeromassless, true);
  row(r.project, true);
  row(r.textures, true);
  printf("  %-22s %8s %9s %9s %9s %9s\n", "-", "", "", "", "", "");
  row(r.substep, false);
  row(r.frame, false);

  // The budget: the DLL runs on the calling thread, so a substep's cost
  // lands inside the game's frame. At 1x a 0.2 s substep fires about every twelfth frame;
  // the constraint is that when it fires it fits inside one 16.7 ms frame.
  const double worst = r.frame.Max();
  printf("  worst frame %.3f ms against a 16.67 ms budget (%.1f%%)%s\n", worst,
         100.0 * worst / 16.67, worst > 16.67 ? "  ** OVER **" : "");
  printf("  cost per substep per 1000 cells: %.4f ms (median)\n",
         cells ? r.substep.Quantile(0.5) * 1000.0 / cells : 0.0);
}

// The scratch copies both sweep kernels take at the top of every substep, timed on their
// own. They are not a guess at where the time goes — they are the exact operations
// `StepConduction` and `StepFlow` perform, on a buffer of the same size, so the numbers
// subtract cleanly from the kernel totals above.
//
// `StepFlow` copies the whole cell array (12 B/cell) and clears a byte per cell;
// `StepConduction` copies temperatures alone (4 B/cell).
void ProbeCopies(size_t cells, int reps) {
  std::vector<PhaseEntry> src(cells), dst;
  std::vector<float> temps(cells), tdst;
  std::vector<uint8_t> flags;
  Samples phase_copy("StepFlow start.assign"), temp_copy("StepConduction start gather"),
      clear("StepFlow swapped.assign");
  for (int i = 0; i < reps; ++i) {
    phase_copy.Add(TimeIt([&] { dst.assign(src.begin(), src.end()); }));
    temp_copy.Add(TimeIt([&] {
      tdst.resize(cells);
      for (size_t k = 0; k < cells; ++k) tdst[k] = temps[k];
    }));
    clear.Add(TimeIt([&] { flags.assign(cells, 0); }));
  }
  printf("\n--- scratch-buffer probe (%zu cells, %d reps) ---\n", cells, reps);
  printf("  %-30s med %7.3f ms  max %7.3f ms  (%zu KB)\n", phase_copy.name,
         phase_copy.Quantile(0.5), phase_copy.Max(), cells * sizeof(PhaseEntry) / 1024);
  printf("  %-30s med %7.3f ms  max %7.3f ms  (%zu KB)\n", temp_copy.name,
         temp_copy.Quantile(0.5), temp_copy.Max(), cells * sizeof(float) / 1024);
  printf("  %-30s med %7.3f ms  max %7.3f ms  (%zu KB)\n", clear.name, clear.Quantile(0.5),
         clear.Max(), cells / 1024);
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  const char* corpus = nullptr;
  const char* scenario = "all";
  const char* regions = "clamped";
  int width = 256, height = 384, ticks = 50;
  float dt = 0.2f;
  const char* settle_arg = "0";
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--corpus") && i + 1 < argc) corpus = argv[++i];
    else if (!strcmp(argv[i], "--scenario") && i + 1 < argc) scenario = argv[++i];
    else if (!strcmp(argv[i], "--regions") && i + 1 < argc) regions = argv[++i];
    else if (!strcmp(argv[i], "--verify")) g_verify = true;
    else if (!strcmp(argv[i], "--sunlight")) g_sunlight = true;
    else if (!strcmp(argv[i], "--width") && i + 1 < argc) width = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--height") && i + 1 < argc) height = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--dt") && i + 1 < argc) dt = static_cast<float>(atof(argv[++i]));
    else if (!strcmp(argv[i], "--settle") && i + 1 < argc) settle_arg = argv[++i];
  }
  if (!corpus) {
    printf("usage: bench.exe --corpus <corpus.bin> [--scenario asteroid|granite|all]\n"
           "       [--width W --height H] [--ticks N] [--dt SECONDS]\n"
           "       [--regions full|clamped|quarter|cluster:N|rows:N] [--verify]\n"
           "       [--settle N]   run N untimed ticks first, so the measured ones describe\n"
           "                      a settled world and not the transient it was seeded as\n");
    return 2;
  }
  // Digits only. `atoi` answers 0 for "lots", and a settle that silently did not happen
  // reports the transient under a header that claims a settled world.
  {
    const char* p = settle_arg;
    for (; *p; ++p) {
      if (*p < '0' || *p > '9') break;
    }
    if (p == settle_arg || *p != '\0' || p - settle_arg > 9) {
      printf("--settle takes a non-negative tick count -- not \"%s\"\n", settle_arg);
      return 2;
    }
    g_settle = atoi(settle_arg);
  }

  Tables tables;
  if (!oni_bench::LoadTables(corpus, &tables)) return 1;
  printf("element table: %d elements\n", tables.count);

  if (!strcmp(scenario, "all") || !strcmp(scenario, "granite")) {
    RunScenario("granite (floor: nothing to simulate)",
                oni_bench::UniformGranite(tables, width, height), tables, regions, ticks,
                dt);
  }
  if (!strcmp(scenario, "all") || !strcmp(scenario, "asteroid")) {
    RunScenario("asteroid (representative)", oni_bench::Asteroid(tables, width, height),
                tables, regions, ticks, dt);
  }
  ProbeCopies(static_cast<size_t>(width + 2) * (height + 2), ticks);
  if (g_verify) {
    printf("\nverify: %d mismatch%s against a full recompute\n", g_verify_fails,
           g_verify_fails == 1 ? "" : "es");
    if (g_verify_fails != 0) return 1;
  }
  // Reported on every run, `--verify` or not: Klei's validator asks an absolute question, so
  // its answer does not depend on which switch the run was asked for.
  if (g_simcheck_fails != 0) {
    printf("\nsimcheck: %d scenario%s left a vacuum cell holding a temperature or mass\n",
           g_simcheck_fails, g_simcheck_fails == 1 ? "" : "s");
    return 1;
  }
  return 0;
}
