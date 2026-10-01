// Run Klei's SimDLL and the replacement side by side on the same world and diff them.
//
// This is the regression harness for everything that follows. Both DLLs are loaded into
// one process, seeded identically, and stepped in lockstep; after every tick the
// game-visible arrays are compared field by field, and each scenario must stay inside its
// temperature envelope. When it does not, the harness says which field diverged, and where.
//
// It also cross-loads save blobs in both directions. The replacement writes Klei's
// format, so Klei's sim should accept a blob the replacement produced and vice versa;
// that is a much stronger statement than "our own round trip works".
//
// Usage:
//   diffsim.exe --corpus <corpus.bin> [--klei <dll>] [--mine <dll>]
//               [--scenario <name>|all] [--ticks N]

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <windows.h>

#include "../../abi/sim_abi.h"
#include "../../sim/saveblob.h"
#include "simcheck.h"

using namespace oni_sim;

namespace {

// One loaded sim. Two of these coexist, so nothing here may be global.
struct Backend {
  const char* name = "";
  HMODULE module = nullptr;
  void (*initialize)(int (*)(int, void*)) = nullptr;
  void (*shutdown)() = nullptr;
  void* (*handle_message)(int, int, const uint8_t*) = nullptr;
  uint8_t* (*begin_save)(int*, int, int) = nullptr;
  void (*end_save)() = nullptr;

  // The conduit temperature manager. Bound separately from the five above because nothing
  // else in this file needs them and a DLL that lacks them is still usable for every other
  // scenario — the suite predates them by the whole life of the project.
  void (*ct_initialize)() = nullptr;
  void (*ct_shutdown)() = nullptr;
  int (*ct_add)(float, float, int, int, float, float, int32_t) = nullptr;
  int (*ct_set)(int, float, float, int) = nullptr;
  void (*ct_remove)(int) = nullptr;
  void (*ct_clear)() = nullptr;
  void* (*ct_update)(float, void*) = nullptr;

  bool BindConduits() {
    ct_initialize = reinterpret_cast<void (*)()>(
        GetProcAddress(module, "ConduitTemperatureManager_Initialize"));
    ct_shutdown = reinterpret_cast<void (*)()>(
        GetProcAddress(module, "ConduitTemperatureManager_Shutdown"));
    ct_add = reinterpret_cast<int (*)(float, float, int, int, float, float, int32_t)>(
        GetProcAddress(module, "ConduitTemperatureManager_Add"));
    ct_set = reinterpret_cast<int (*)(int, float, float, int)>(
        GetProcAddress(module, "ConduitTemperatureManager_Set"));
    ct_remove = reinterpret_cast<void (*)(int)>(
        GetProcAddress(module, "ConduitTemperatureManager_Remove"));
    ct_clear = reinterpret_cast<void (*)()>(
        GetProcAddress(module, "ConduitTemperatureManager_Clear"));
    ct_update = reinterpret_cast<void* (*)(float, void*)>(
        GetProcAddress(module, "ConduitTemperatureManager_Update"));
    return ct_initialize && ct_shutdown && ct_add && ct_set && ct_remove && ct_clear &&
           ct_update;
  }

  bool Bind(const char* dll, const char* label) {
    name = label;
    module = LoadLibraryA(dll);
    if (!module) {
      // The default path points at SimDLL_orig.dll, which only exists while the shim is
      // installed. With the shim removed, Klei's DLL is back under its own name — fall
      // back to it rather than failing, so the harness works in both states.
      std::string alt(dll);
      const size_t at = alt.find("SimDLL_orig.dll");
      if (at != std::string::npos) {
        alt.replace(at, strlen("SimDLL_orig.dll"), "SimDLL.dll");
        module = LoadLibraryA(alt.c_str());
      }
    }
    if (!module) {
      printf("LoadLibrary failed for %s (err %lu)\n", dll, GetLastError());
      return false;
    }
    initialize = reinterpret_cast<void (*)(int (*)(int, void*))>(
        GetProcAddress(module, "SIM_Initialize"));
    shutdown = reinterpret_cast<void (*)()>(GetProcAddress(module, "SIM_Shutdown"));
    handle_message = reinterpret_cast<void* (*)(int, int, const uint8_t*)>(
        GetProcAddress(module, "SIM_HandleMessage"));
    begin_save = reinterpret_cast<uint8_t* (*)(int*, int, int)>(
        GetProcAddress(module, "SIM_BeginSave"));
    end_save = reinterpret_cast<void (*)()>(GetProcAddress(module, "SIM_EndSave"));
    if (!initialize || !shutdown || !handle_message || !begin_save || !end_save) {
      printf("%s is missing one of the required exports\n", dll);
      return false;
    }
    return true;
  }

  // Where the random stream stands right now. Only our DLL exports it, so for the game's DLL
  // this is zero, which means "cannot read it", not "state is zero".
  uint32_t RandomState() const {
    if (!module) return 0;
    auto mine = reinterpret_cast<uint32_t (*)()>(
        GetProcAddress(module, "SIM_DebugRandomState"));
    return mine ? mine() : 0;
  }

  // The conservation ledger, ours only — Klei's DLL has no such export and keeps no such
  // data, so this side of the check has no other side. Returns the number of fields the
  // DLL knows about, or 0 if it is not ours.
  int Ledger(double* out, int count) const {
    if (!module) return 0;
    auto fn = reinterpret_cast<int (*)(double*, int)>(
        GetProcAddress(module, "SIM_DebugLedger"));
    return fn ? fn(out, count) : 0;
  }

  void* Send(SimMessageHash hash, const std::vector<uint8_t>& b) {
    return handle_message(static_cast<int32_t>(hash), static_cast<int>(b.size()),
                          b.empty() ? nullptr : b.data());
  }
  void* SendEmpty(SimMessageHash hash) {
    return handle_message(static_cast<int32_t>(hash), 0, nullptr);
  }
  std::vector<uint8_t> Save(int x, int y) {
    int size = 0;
    const uint8_t* p = begin_save(&size, x, y);
    std::vector<uint8_t> out;
    if (p && size > 0) out.assign(p, p + size);
    end_save();
    return out;
  }
};

int Quiet(int, void*) { return 0; }

// Set once the scenario world is known, so divergence reports can print x,y.
int g_grid_width = 0;

// The property textures are a known-incomplete area (liquid alpha and the sunlight input
// are unrecovered), and they diverge on tick 1 in any world with liquid in it. That hides
// the first *state* divergence, which is what a physics kernel is debugged from, so they
// can be muted while working on physics.
bool g_skip_textures = false;

// ------------------------------------------------------------------------- the ledger
//
// `--ledger`. Every other check in this harness is a
// comparison against Klei; this one is not, which is why it is the piece of verification
// that survives volume fractions. Klei has no mixed cell to be compared against, but it
// still has to conserve mass.
//
// Off by default, and deliberately so: reading it walks the grid once per tick, and the
// suite's output is a reproducibility hash that nothing should be added to casually.
bool g_ledger = false;

// The field order `SIM_DebugLedger` publishes. Append only, never reorder — the DLL's copy
// of this list is the other half of the contract.
constexpr int kLedgerCount = 14;
const char* const kLedgerFields[kLedgerCount] = {
    "grid",     "emitted",    "modified", "consumed", "dug",       "ore",
    "unstable", "sublimated", "wisp",     "thinliq",  "cleared",   "compconsumed",
    "compemitted", "emitore"};

// Everything the grid gained, minus everything it lost. Field 0 is the grid itself and is
// not part of the sum.
inline double LedgerNet(const double* v) {
  // `emitore` (v[13]) is deliberately **not** in this sum. A solid element emitter puts
  // nothing into the grid and takes nothing out of it: it manufactures a lump of ore and
  // hands it to the game, out of mass the building is holding outside the sim. It is
  // recorded because "the sim created 1495 kg this run" is worth being able to see, but it
  // is not a grid flow and putting it on either side of the balance would break it.
  return v[1] + v[2] + v[12] -
         (v[3] + v[4] + v[5] + v[6] + v[7] + v[8] + v[9] + v[10] + v[11]);
}

// The quantity that must not move. Drift is the difference between two of these, so the
// scenario's starting mass cancels and only the unexplained part is left.
inline double LedgerAnchor(const double* v) { return v[0] - LedgerNet(v); }

// The seed `SimData_InitializeFromCells` carries. It is not saved in the blob and had no
// known effect until the tracer scenario found Klei shuffling gas cells; if the shuffle is
// seeded, changing this changes the pattern.
uint32_t g_world_seed = 12345u;

// ------------------------------------------------------------------- world seeding

struct World {
  int width = 0, height = 0;
  std::vector<uint16_t> element;
  std::vector<float> mass, temperature;
  // 255 is "no insulation", which is the literal every older scenario
  // got from the payload writer below. Defaulting to it here keeps all of them sending
  // exactly the bytes they used to.
  std::vector<uint8_t> insulation;
  // The properties bitfield. 0 is what every older scenario sent, and
  // it is what the game sends for an ordinary cell; the bits ride on building tiles. A
  // scenario that leaves this alone sends exactly the bytes it used to.
  std::vector<uint8_t> properties;
  // The backwall: a second material behind the cell, carried by `Sim.SimBackwall`. Older
  // scenarios sent `SimBackwall{}` for every cell of every world,
  // so the whole struct was one-valued and the three arrays the sim publishes back for it
  // were never compared either. Zeros here are exactly the bytes those scenarios sent.
  std::vector<uint16_t> backwall_element;
  std::vector<float> backwall_mass, backwall_temperature;
  // Germs. `Sim.DiseaseCell` carries the index, the count and the two fields the sim owns,
  // and the infestation age is one of them: a cell has to have held its germs for
  // `minDiffusionInfestationTickCount` substeps before any of them can leave, so a scenario
  // that seeds zero here is testing the gate rather than the diffusion.
  std::vector<uint8_t> disease_idx;
  std::vector<int32_t> disease_count;
  std::vector<uint8_t> disease_infest;
  // The world header's radiation byte. **Zero in every older scenario**,
  // which left the whole radiation emitter component and the `radiationKillRate` term in
  // disease growth behind a flag nothing had ever set. A scenario that leaves this alone
  // sends exactly the byte it used to.
  bool radiation = false;
  // Send `headless = 0` for this world whatever `--gameside` says. For the scenarios whose
  // whole point is a path headless switches off -- a liquid handed to the game as a falling
  // particle -- so the canonical suite scores it instead of leaving it to a flag nobody passes.
  bool gameside = false;
  // `NewGameFrame::currentCosmicRadiationIntensity`, the sky's contribution. Sent as zero by
  // every older scenario, so the occlusion column walk that consumes it
  // had nothing to consume.
  float cosmic = 0.0f;

  int Cell(int x, int y) const { return y * width + x; }
  size_t Count() const { return element.size(); }
  void Init(int w, int h, uint16_t e, float m, float t) {
    width = w;
    height = h;
    const size_t n = static_cast<size_t>(w) * h;
    element.assign(n, e);
    mass.assign(n, m);
    temperature.assign(n, t);
    insulation.assign(n, 255);
    properties.assign(n, 0);
    backwall_element.assign(n, 0);
    backwall_mass.assign(n, 0.0f);
    backwall_temperature.assign(n, 0.0f);
    disease_idx.assign(n, 0xFF);
    disease_count.assign(n, 0);
    disease_infest.assign(n, 0);
  }
  void SetDisease(int x, int y, uint8_t idx, int32_t count, uint8_t infest = 0xFE) {
    const int c = Cell(x, y);
    disease_idx[c] = idx;
    disease_count[c] = count;
    disease_infest[c] = infest;
  }
  void Set(int x, int y, uint16_t e, float m, float t) {
    const int c = Cell(x, y);
    element[c] = e;
    mass[c] = m;
    temperature[c] = t;
  }
  void SetInsulation(int x, int y, uint8_t v) { insulation[Cell(x, y)] = v; }
  void SetProperties(int x, int y, uint8_t v) { properties[Cell(x, y)] = v; }
  void SetBackwall(int x, int y, uint16_t e, float m, float t) {
    const int c = Cell(x, y);
    backwall_element[c] = e;
    backwall_mass[c] = m;
    backwall_temperature[c] = t;
  }
};

// `headless` is sent as 1 by every scenario here, which is what an offline harness ought
// to say. It is also a candidate for why the sunlight texture never fills, so it is
// switchable.
//
// `--gameside` flips the default for the whole run. It matters for exactly one kernel:
// an unstable solid is simulated falling through the grid when this is 1 and handed to
// the game as an `unstableCellInfo` when it is 0, and the game itself sends 0 in normal
// play. Running `sand --gameside` is the only way to score that half.
bool g_gameside = false;
std::vector<uint8_t> WorldPayload(const World& w, bool headless_mode = !g_gameside) {
  std::vector<uint8_t> b;
  auto put = [&](const void* p, size_t n) {
    const uint8_t* s = static_cast<const uint8_t*>(p);
    b.insert(b.end(), s, s + n);
  };
  const int32_t width = w.width, height = w.height;
  const uint32_t seed = g_world_seed;
  const uint8_t radiation = w.radiation ? 1 : 0;
  const uint8_t headless = headless_mode && !w.gameside ? 1 : 0;
  put(&width, 4);
  put(&height, 4);
  put(&seed, 4);
  put(&radiation, 1);
  put(&headless, 1);
  for (size_t i = 0; i < w.Count(); ++i) {
    Cell c{};
    c.elementIdx = w.element[i];
    c.mass = w.mass[i];
    c.temperature = w.temperature[i];
    c.insulation = w.insulation.empty() ? 255 : w.insulation[i];
    c.properties = w.properties.empty() ? 0 : w.properties[i];
    put(&c, sizeof(Cell));
  }
  for (size_t i = 0; i < w.Count(); ++i) {
    DiseaseCell d{};
    d.diseaseIdx = w.disease_idx.empty() ? 0xFF : w.disease_idx[i];
    d.elementCount = w.disease_count.empty() ? 0 : w.disease_count[i];
    d.reservedInfestationTickCount = w.disease_infest.empty() ? 0 : w.disease_infest[i];
    put(&d, sizeof(DiseaseCell));
  }
  for (size_t i = 0; i < w.Count(); ++i) {
    SimBackwall s{};
    s.elementIdx = w.backwall_element.empty() ? 0 : w.backwall_element[i];
    s.mass = w.backwall_mass.empty() ? 0.0f : w.backwall_mass[i];
    s.temperature = w.backwall_temperature.empty() ? 0.0f : w.backwall_temperature[i];
    put(&s, sizeof(SimBackwall));
  }
  return b;
}

// ------------------------------------------------------------------- element table

struct Tables {
  std::vector<uint8_t> elements, diseases;
  int32_t count = 0;

  const Element* At(int32_t i) const {
    return reinterpret_cast<const Element*>(elements.data() + 4 +
                                            static_cast<size_t>(i) * sizeof(Element));
  }
  int32_t IndexOf(int32_t hash) const {
    for (int32_t i = 0; i < count; ++i) {
      if (At(i)->id == hash) return i;
    }
    return -1;
  }
};

bool LoadTables(const char* path, Tables* out) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    printf("cannot open corpus %s\n", path);
    return false;
  }
  for (;;) {
    int32_t h[4];
    if (fread(h, sizeof(h), 1, f) != 1) break;
    const size_t bytes = static_cast<size_t>(h[2]) * (h[3] > 0 ? h[3] : 1);
    std::vector<uint8_t> payload(bytes);
    if (bytes && fread(payload.data(), 1, bytes, f) != bytes) break;
    if (h[1] == static_cast<int32_t>(SimMessageHash::Elements_CreateTable) &&
        out->elements.empty()) {
      out->elements = std::move(payload);
    } else if (h[1] == static_cast<int32_t>(SimMessageHash::Disease_CreateTable) &&
               out->diseases.empty()) {
      out->diseases = std::move(payload);
    }
    if (!out->elements.empty() && !out->diseases.empty()) break;
  }
  fclose(f);
  if (out->elements.size() < 4 || out->diseases.empty()) return false;
  memcpy(&out->count, out->elements.data(), 4);
  return out->count > 0;
}

enum : int32_t {
  kVacuum = 758759285,
  kOxygen = -1528777920,
  kWater = 1836671383,
  kGranite = -105943486,
  kCopper = -1725038055,
  kSandStone = 493438017,
  kIce = 873952427,
  kSteam = -899515856,
  // The two solids with the lowest `lightAbsorptionFactor` in the table, both 0.1 with a
  // `maxMass` of 1840. Glass is what Klei's Glass Tile is built from, and the tile marks its cell
  // `Transparent`; `sunglass` asks whether Klei's sunlight reads either fact.
  kGlass = 623986332,
  kDiamond = -2079931820,
  // The one element in the table with no transition it can actually take: its high
  // transition points back at itself. Nothing else survives 10000 K.
  kNeutronium = 1838482828,
  kHydrogen = -1046145888,
  // Element index 151: the widest liquid range in the table, 2 K to 710 K. The corpus
  // carries no element names, so it is identified by hash and named for what it is used
  // for here — a liquid that stays liquid whatever the rest of the scenario does to it.
  kWideLiquid = -123825053,
  // Two solid -> solid transitions, which is what a transition probe needs if it is not to
  // drag the liquid textures in with it. Peat becomes element 90 at 500 K and Clay becomes
  // Ceramic at 1200 K; every other high transition in the table lands in a liquid or a gas.
  kPeat = -1927771704,
  kClay = 867327137,
  // The only Unstable element any scenario here uses. Regolith (1362238252) is the other
  // obvious one and behaves identically — the flag is on the element, not the kernel.
  kSand = 381796644,
  // The five elements in the table with a `sublimateIndex`, which is the whole reachable
  // surface of `DoSublimation` and the liquid off-gas path. Nothing else can trigger them.
  kOxyRock = 1262005685,      // solid  -> Oxygen,            probability 1.00
  kToxicSand = 869554203,     // solid  -> ContaminatedOxygen, probability 0.05
  kToxicMud = 900133477,      // solid  -> ContaminatedOxygen, probability 0.05, *and* Unstable
  kDirtyWater = 1832607973,   // liquid -> ContaminatedOxygen, off-gas rate 0.001
};

// Disease indices, in the order the shipped table lists them. The table arrives keyed by
// hash but is indexed by position everywhere else, and the corpus is pinned to build
// 744825, so these are as fixed as the element hashes above.
enum : uint8_t {
  kFoodPoisoning = 0,
  kSlimelung = 1,
  kFloralScent = 2,
  kZombieSpores = 3,
  kRadContaminants = 4,
};

// `Sim.Cell.Properties`, mirrored from `sim/world.h` because this file does not include it.
// A scenario that leaves it alone sends a zero byte, which leaves the property branches in
// `physics.h` unreached; the property scenarios set it.
enum : uint8_t {
  kGasImpermeable = 1,
  kLiquidImpermeable = 2,
  kSolidImpermeable = 4,
  kUnbreakable = 8,
  kTransparent = 0x10,
  kNotifyOnMelt = 0x40,
};

// ------------------------------------------------------------------- comparison

struct FieldDiff {
  const char* name;
  size_t wrong = 0;
  size_t first = 0;
  double worst = 0.0;
};

template <typename T>
FieldDiff CompareExact(const char* name, const T* a, const T* b, size_t n) {
  FieldDiff d{name};
  for (size_t i = 0; i < n; ++i) {
    if (a[i] == b[i]) continue;
    if (!d.wrong) d.first = i;
    ++d.wrong;
  }
  return d;
}

FieldDiff CompareFloat(const char* name, const float* a, const float* b, size_t n,
                       float tolerance) {
  FieldDiff d{name};
  for (size_t i = 0; i < n; ++i) {
    const double diff = std::fabs(static_cast<double>(a[i]) - b[i]);
    if (diff <= tolerance) continue;
    if (!d.wrong) d.first = i;
    ++d.wrong;
    if (diff > d.worst) d.worst = diff;
  }
  return d;
}

bool ReportDiffs(const std::vector<FieldDiff>& diffs, size_t cells, int tick) {
  bool clean = true;
  for (const FieldDiff& d : diffs) {
    if (!d.wrong) continue;
    clean = false;
    printf("  tick %-4d %-18s %zu/%zu cells differ, first at %zu", tick, d.name, d.wrong,
           cells, d.first);
    if (d.worst > 0) printf(", worst |delta| %.6g", d.worst);
    printf("\n");
  }
  return clean;
}

// The backwall transition announcement is level-triggered: every frame republishes every
// backwall that is out of range. That cannot be compared tick by tick, because Klei's sim
// is on a worker thread and a `PrepareGameData` finds nought, one or two frames queued —
// over 50 ticks of the `backwall` scenario Klei runs 49 frames and we run 50, so the totals
// are 1960 against 2000 for two sims that agree exactly about every frame.
//
// So the comparable quantity is *one frame's* list. A tick's update holds its frames'
// announcements concatenated, so the tick is divided by the frame count it reports and the
// chunk is checked against the rest of the tick and against every other frame of the run.
struct FrameList {
  std::vector<int32_t> per_frame;   // what one frame announces
  bool set = false;
  bool varied = false;              // true if two frames of the run disagreed
  size_t total = 0;                 // every announcement, for the report
};
FrameList g_bw_klei, g_bw_mine;
// The chunk melt list is the same shape and for the same reason: `ElementChunk::Update`
// re-announces every out-of-range chunk on every substep, so a tick's list is one frame's
// list repeated, and the totals differ whenever Klei's frame queue does.
FrameList g_ec_klei, g_ec_mine;
// The size `elementChunkInfos` had on the previous tick. `ElementChunk::Update` *accumulates*
// into `deltaKJ` and only ever *assigns* `temperature`, so on the first substep after the
// list grows the accumulation reads whatever the new entries were born holding — and Klei's
// resize does not value-initialise them. Two runs of the unchanged suite came back clean and
// a third reported `deltaKJ klei 18495874743460826419745735573504.00000` for a chunk whose
// temperature was exact, on the one tick the array went from 0 entries to 4.
//
// So a chunk's first reported energy delta is uninitialised memory in the shipped game. It
// cannot be reproduced and should not be: `temperature` is still compared on that tick, and
// `deltaKJ` is compared on every tick after it.
int g_prev_chunk_infos = -1;

// `n` announcements spread over `frames` frames. Anything that does not divide evenly, or
// whose chunks are not all the same, is itself the finding.
void RecordFrameList(FrameList* out, const int32_t* cells, int n, int frames) {
  out->total += static_cast<size_t>(n);
  if (frames <= 0) {
    // A tick Klei spun on: no frame ran, so it announced nothing and there is nothing to
    // learn from it. A non-empty list here would be a finding, and is reported as varied.
    if (n) out->varied = true;
    return;
  }
  // A frame that announced nothing is not a claim that the steady list is empty. Our first
  // tick runs a frame that drains no messages and touches no component, so it announces
  // nothing while Klei, which spun instead, is not recorded at all — and latching `[]` from
  // it made every chunk scenario read as "mine 0 per frame" against a list the two sides
  // agreed about entry for entry.
  //
  // Silence *after* the list has been latched is different, and is the finding it looks
  // like: a side that announced two handles a frame and then stops is announcing at the
  // wrong rate, which is the one way a per-frame comparison could otherwise be passed by a
  // sim that fires half as often.
  if (n == 0) {
    if (out->set && !out->per_frame.empty()) out->varied = true;
    return;
  }
  if (n % frames) {
    out->varied = true;
    return;
  }
  const int per = n / frames;
  std::vector<int32_t> chunk(cells, cells + per);
  for (int f = 1; f < frames; ++f) {
    if (!std::equal(chunk.begin(), chunk.end(), cells + f * per)) out->varied = true;
  }
  if (!out->set) {
    out->per_frame = chunk;
    out->set = true;
  } else if (out->per_frame != chunk) {
    out->varied = true;
  }
}

// GameDataUpdate is Pack = 4 and the pointers are only valid until the next call, so
// everything compared has to be read out of both structs in the same breath.
bool CompareUpdates(const GameDataUpdate* a, const GameDataUpdate* b, size_t cells,
                    int tick) {
  std::vector<FieldDiff> diffs;
  diffs.push_back(CompareExact("elementIdx", a->elementIdx, b->elementIdx, cells));
  diffs.push_back(CompareFloat("mass", a->mass, b->mass, cells, 1e-3f));
  diffs.push_back(CompareFloat("temperature", a->temperature, b->temperature, cells,
                               1e-2f));
  diffs.push_back(CompareExact("diseaseIdx", a->diseaseIdx, b->diseaseIdx, cells));
  diffs.push_back(CompareExact("diseaseCount", a->diseaseCount, b->diseaseCount, cells));
  diffs.push_back(CompareFloat("radiation", a->radiation, b->radiation, cells, 1e-3f));
  // The backwall arrives from the game and, on our side, leaves again unchanged. Comparing it,
  // with scenarios that send non-zero backwalls, is what makes "our backwall matches Klei's"
  // a tested claim.
  diffs.push_back(
      CompareExact("backwallElement", a->backwallElement, b->backwallElement, cells));
  diffs.push_back(CompareFloat("backwallMass", a->backwallMass, b->backwallMass, cells,
                               1e-3f));
  diffs.push_back(CompareFloat("backwallTemp", a->backwallTemperature,
                               b->backwallTemperature, cells, 1e-2f));
  // The property textures are raw buffers, not handles, so they can be compared cell by
  // cell like everything else. A null on either side is a hard failure: the game feeds
  // these straight to Texture2D.LoadRawTextureData.
  if (g_skip_textures) {
    bool ok = ReportDiffs(diffs, cells, tick);
    if (!ok && g_grid_width > 0) {
      int shown = 0;
      for (size_t i = 0; i < cells && shown < 8; ++i) {
        const bool temp_bad =
            std::fabs(static_cast<double>(a->temperature[i]) - b->temperature[i]) > 1e-2f;
        const bool mass_bad =
            std::fabs(static_cast<double>(a->mass[i]) - b->mass[i]) > 1e-3f;
        if (!temp_bad && !mass_bad) continue;
        ++shown;
        printf("      cell %-5zu (%2d,%2d) elem k%3u/m%3u  mass k%10.4f m%10.4f"
               "  temp k%11.5f m%11.5f\n",
               i, static_cast<int>(i) % g_grid_width, static_cast<int>(i) / g_grid_width,
               a->elementIdx[i], b->elementIdx[i], a->mass[i], b->mass[i],
               a->temperature[i], b->temperature[i]);
      }
    }
    return ok;
  }
  if (!a->propertyTextureFlow || !b->propertyTextureFlow ||
      !a->propertyTextureLiquid || !b->propertyTextureLiquid ||
      !a->propertyTextureLiquidData || !b->propertyTextureLiquidData ||
      !a->propertyTextureMaterialData || !b->propertyTextureMaterialData ||
      !a->propertyTextureExposedToSunlight || !b->propertyTextureExposedToSunlight) {
    printf("  tick %-4d property texture pointer is NULL (klei %s, mine %s)\n", tick,
           a->propertyTextureFlow ? "ok" : "null",
           b->propertyTextureFlow ? "ok" : "null");
    return false;
  }
  diffs.push_back(CompareFloat("tex.flow",
                               static_cast<const float*>(a->propertyTextureFlow),
                               static_cast<const float*>(b->propertyTextureFlow),
                               cells * 2, 1e-4f));
  diffs.push_back(CompareExact("tex.liquid",
                               static_cast<const uint8_t*>(a->propertyTextureLiquid),
                               static_cast<const uint8_t*>(b->propertyTextureLiquid),
                               cells * 4));
  diffs.push_back(CompareExact("tex.liquidData",
                               static_cast<const uint8_t*>(a->propertyTextureLiquidData),
                               static_cast<const uint8_t*>(b->propertyTextureLiquidData),
                               cells * 4));
  diffs.push_back(CompareExact("tex.materialData",
                               static_cast<const uint8_t*>(a->propertyTextureMaterialData),
                               static_cast<const uint8_t*>(b->propertyTextureMaterialData),
                               cells * 4));
  diffs.push_back(CompareExact(
      "tex.sunlight", static_cast<const uint8_t*>(a->propertyTextureExposedToSunlight),
      static_cast<const uint8_t*>(b->propertyTextureExposedToSunlight), cells));
  bool clean = ReportDiffs(diffs, cells, tick);
  if (!clean && g_grid_width > 0) {
    const auto* la = static_cast<const uint8_t*>(a->propertyTextureLiquid);
    const auto* lb = static_cast<const uint8_t*>(b->propertyTextureLiquid);
    int shown = 0;
    for (size_t i = 0; i < cells && shown < 6; ++i) {
      if (!memcmp(la + i * 4, lb + i * 4, 4)) continue;
      ++shown;
      printf("      tex.liquid cell %-5zu (%2d,%2d) elem %3u mass %8.2f  klei"
             " %3u,%3u,%3u,%3u  mine %3u,%3u,%3u,%3u\n",
             i, static_cast<int>(i) % g_grid_width, static_cast<int>(i) / g_grid_width,
             a->elementIdx[i], a->mass[i], la[i * 4], la[i * 4 + 1], la[i * 4 + 2],
             la[i * 4 + 3], lb[i * 4], lb[i * 4 + 1], lb[i * 4 + 2], lb[i * 4 + 3]);
    }
  }
  if (!clean && g_grid_width > 0) {
    const auto* la = static_cast<const uint8_t*>(a->propertyTextureLiquidData);
    const auto* lb = static_cast<const uint8_t*>(b->propertyTextureLiquidData);
    const auto* sa = static_cast<const uint8_t*>(a->propertyTextureExposedToSunlight);
    int shown = 0;
    for (size_t i = 0; i < cells && shown < 6; ++i) {
      if (!memcmp(la + i * 4, lb + i * 4, 4)) continue;
      ++shown;
      printf("      tex.liquidData cell %-5zu (%2d,%2d) elem %3u mass %8.2f temp %8.3f"
             " sun %3u  klei %3u,%3u,%3u,%3u  mine %3u,%3u,%3u,%3u\n",
             i, static_cast<int>(i) % g_grid_width, static_cast<int>(i) / g_grid_width,
             a->elementIdx[i], a->mass[i], a->temperature[i], sa[i], la[i * 4],
             la[i * 4 + 1], la[i * 4 + 2], la[i * 4 + 3], lb[i * 4], lb[i * 4 + 1],
             lb[i * 4 + 2], lb[i * 4 + 3]);
    }
  }
  // A cell index and a worst-case delta are not enough to debug a kernel. Print the
  // first few disagreeing cells with both sides' values and what is in them.
  if (!clean && g_grid_width > 0) {
    int shown = 0;
    for (size_t i = 0; i < cells && shown < 8; ++i) {
      const bool temp_bad =
          std::fabs(static_cast<double>(a->temperature[i]) - b->temperature[i]) > 1e-2f;
      const bool mass_bad =
          std::fabs(static_cast<double>(a->mass[i]) - b->mass[i]) > 1e-3f;
      if (!temp_bad && !mass_bad) continue;
      ++shown;
      printf("      cell %-5zu (%2d,%2d) elem k%3u/m%3u  mass k%10.4f m%10.4f"
             "  temp k%11.5f m%11.5f\n",
             i, static_cast<int>(i) % g_grid_width, static_cast<int>(i) / g_grid_width,
             a->elementIdx[i], b->elementIdx[i], a->mass[i], b->mass[i],
             a->temperature[i], b->temperature[i]);
    }
  }

  if (a->numSubstanceChangeInfo != b->numSubstanceChangeInfo) {
    printf("  tick %-4d substanceChangeInfo  klei %d, mine %d\n", tick,
           a->numSubstanceChangeInfo, b->numSubstanceChangeInfo);
    // A count alone does not say what Klei thinks changed. Print the first few with the
    // element indices, because a vacuum cell being zeroed and a cell genuinely changing
    // material are indistinguishable from the count.
    for (int i = 0; i < a->numSubstanceChangeInfo && i < 6; ++i) {
      const SubstanceChangeInfo& s = a->substanceChangeInfo[i];
      printf("      klei change[%d] cell %-5d (%2d,%2d)  %u -> %u\n", i, s.cellIdx,
             g_grid_width ? s.cellIdx % g_grid_width : 0,
             g_grid_width ? s.cellIdx / g_grid_width : 0, s.oldElemIdx, s.newElemIdx);
    }
    clean = false;
  }
  // Klei owns two backwall event arrays we have never published. If either fires, the
  // backwall is a live subsystem on its side and a passive store on ours.
  if (a->numBackwallElementChangedInfos != b->numBackwallElementChangedInfos) {
    printf("  tick %-4d backwallElementChanged  klei %d, mine %d\n", tick,
           a->numBackwallElementChangedInfos, b->numBackwallElementChangedInfos);
    clean = false;
  }
  // Reduced to one frame's worth here and scored at the end of the run. `gameCell` is the
  // only member, so the info array is an int32 array.
  RecordFrameList(&g_bw_klei,
                  reinterpret_cast<const int32_t*>(a->backwallShouldTransitionInfos),
                  a->numBackwallShouldTransitionInfos, a->numFramesProcessed);
  RecordFrameList(&g_bw_mine,
                  reinterpret_cast<const int32_t*>(b->backwallShouldTransitionInfos),
                  b->numBackwallShouldTransitionInfos, b->numFramesProcessed);
  extern bool g_show_messages;  // defined with the other dump flags, below
  if (a->numSolidInfo != b->numSolidInfo) {
    printf("  tick %-4d solidInfo            klei %d, mine %d\n", tick, a->numSolidInfo,
           b->numSolidInfo);
    // The entries, not just the count. A `solidInfo` carries which way the cell went, and
    // without that a divergence here reads as "one list is longer" when the question is
    // always which cell changed its mind and in which direction. Printed under `--messages`
    // the way the substance list is, and capped the same way, because a first frame can
    // legitimately announce thousands.
    if (g_show_messages) {
      const int lim = 12;
      for (int i = 0; i < a->numSolidInfo && i < lim; ++i) {
        printf("      klei [%2d] cell %5d -> %s\n", i, a->solidInfo[i].cellIdx,
               a->solidInfo[i].isSolid ? "solid" : "not solid");
      }
      for (int i = 0; i < b->numSolidInfo && i < lim; ++i) {
        printf("      mine [%2d] cell %5d -> %s\n", i, b->solidInfo[i].cellIdx,
               b->solidInfo[i].isSolid ? "solid" : "not solid");
      }
    }
    clean = false;
  }

  // Always zero on both sides while the harness sends `headless = 1` — the unstable path
  // simulates the fall in the grid instead of handing it over. Compared anyway, because
  // "always zero" is exactly the kind of claim that stops being true silently.
  if (a->numUnstableCellInfo != b->numUnstableCellInfo) {
    printf("  tick %-4d unstableCellInfo     klei %d, mine %d\n", tick,
           a->numUnstableCellInfo, b->numUnstableCellInfo);
    clean = false;
  }

  // Falling liquid, the other thing `headless` switches off: zero on both sides under the
  // canonical suite, and the whole of what `--gameside` on a liquid scenario scores. Every
  // record is compared byte for byte and in order, because the list is handed to the game
  // unsorted and the order is what the game spawns in.
  {
    int first = -1;
    const int n = a->numSpawnFallingLiquidInfo < b->numSpawnFallingLiquidInfo
                      ? a->numSpawnFallingLiquidInfo : b->numSpawnFallingLiquidInfo;
    for (int i = 0; i < n && first < 0; ++i) {
      if (memcmp(&a->spawnFallingLiquidInfo[i], &b->spawnFallingLiquidInfo[i],
                 sizeof(SpawnFallingLiquidInfo)) != 0) {
        first = i;
      }
    }
    if (a->numSpawnFallingLiquidInfo != b->numSpawnFallingLiquidInfo || first >= 0) {
      printf("  tick %-4d spawnFallingLiquid   klei %d, mine %d, first differing record %d\n",
             tick, a->numSpawnFallingLiquidInfo, b->numSpawnFallingLiquidInfo, first);
      clean = false;
    }
  }

  // World damage: the walls over-full liquid broke (`DoPressureBreak`). Not gated on
  // headless, so every scenario scores it. Compared record by record and in order, like
  // falling liquid, because the game applies them in the order they arrive.
  {
    int first = -1;
    const int n = a->numWorldDamageInfo < b->numWorldDamageInfo ? a->numWorldDamageInfo
                                                                 : b->numWorldDamageInfo;
    for (int i = 0; i < n && first < 0; ++i) {
      if (memcmp(&a->worldDamageInfo[i], &b->worldDamageInfo[i], sizeof(WorldDamageInfo)) !=
          0) {
        first = i;
      }
    }
    if (a->numWorldDamageInfo != b->numWorldDamageInfo || first >= 0) {
      printf("  tick %-4d worldDamage          klei %d, mine %d, first differing record %d\n",
             tick, a->numWorldDamageInfo, b->numWorldDamageInfo, first);
      clean = false;
    }
  }

  // Melted tiles: the cells whose `kNotifyOnMelt` tile heated past its melting point
  // (`DoStateTransition`'s high branch). Not gated on headless. Compared in order, because the
  // game destroys the tiles in the order they arrive.
  {
    int first = -1;
    const int n = a->numCellMeltedInfos < b->numCellMeltedInfos ? a->numCellMeltedInfos
                                                                 : b->numCellMeltedInfos;
    for (int i = 0; i < n && first < 0; ++i) {
      if (a->cellMeltedInfos[i].gameCell != b->cellMeltedInfos[i].gameCell) first = i;
    }
    if (a->numCellMeltedInfos != b->numCellMeltedInfos || first >= 0) {
      printf("  tick %-4d cellMelted           klei %d, mine %d, first differing record %d\n",
             tick, a->numCellMeltedInfos, b->numCellMeltedInfos, first);
      clean = false;
    }
  }

  // Klei tells us how many frames it actually ran, and it is **not always one**. Its sim is
  // on a worker thread, and a `PrepareGameData` that finds two frames queued runs both — one
  // call in roughly fifteen, measured here. That is a second substep of everything, and in a
  // world where only one quantity is moving it looks exactly like a kernel that transfers at
  // twice the right rate. This line is why the building-to-building probe is not a bug.
  //
  // Reported, never counted as a divergence: it is Klei's scheduler, not our arithmetic, and
  // marking it unclean would make every scenario's "first divergence" read tick 1.
  // Only the doubled case is worth printing. Klei reports 0 for `Start` and for the first
  // `PrepareGameData`, which is its warm-up and is normal — and even that is not stable
  // between runs, which is its own small piece of evidence about the frame queue.
  if (a->numFramesProcessed > 1) {
    printf("  tick %-4d klei ran %d frames in one call (its own scheduling; every kernel"
           " below ran that many times)\n", tick, a->numFramesProcessed);
  }

  // Buildings. The temperature list is indexed by handle, so a mismatched handle at the
  // same index is an allocator disagreement rather than a physics one and is worth saying
  // out loud — the two failures look identical in a temperature-only diff.
  if (a->numBuildingTemperatures != b->numBuildingTemperatures) {
    printf("  tick %-4d buildingTemperatures klei %d, mine %d\n", tick,
           a->numBuildingTemperatures, b->numBuildingTemperatures);
    clean = false;
  } else {
    for (int i = 0; i < a->numBuildingTemperatures; ++i) {
      const BuildingTemperatureInfo& x = a->buildingTemperatures[i];
      const BuildingTemperatureInfo& y = b->buildingTemperatures[i];
      if (x.handle != y.handle) {
        printf("  tick %-4d building[%d] handle klei %d, mine %d\n", tick, i, x.handle,
               y.handle);
        clean = false;
      } else if (std::fabs(static_cast<double>(x.temperature) - y.temperature) > 1e-3) {
        printf("  tick %-4d building[%d] handle %d  temp klei %11.5f  mine %11.5f\n", tick,
               i, x.handle, x.temperature, y.temperature);
        clean = false;
      }
    }
  }
  // Element chunks. `elementChunkInfos` is indexed by handle slot the way the building
  // temperature list is, so a length mismatch is the allocators disagreeing about how many
  // slots exist rather than about physics — worth separating from a value mismatch.
  //
  // `deltaKJ` is compared as well as the temperature, and it has to be: it is the only
  // observable that says the *energy* moved rather than just the temperature landing in the
  // right place, and it is a running total rather than a per-frame figure, so an error in it
  // never washes out.
  if (a->numElementChunkInfos != b->numElementChunkInfos) {
    printf("  tick %-4d elementChunkInfos    klei %d, mine %d\n", tick,
           a->numElementChunkInfos, b->numElementChunkInfos);
    clean = false;
  } else {
    for (int i = 0; i < a->numElementChunkInfos; ++i) {
      const ElementChunkInfo& x = a->elementChunkInfos[i];
      const ElementChunkInfo& y = b->elementChunkInfos[i];
      if (std::fabs(static_cast<double>(x.temperature) - y.temperature) > 1e-3) {
        printf("  tick %-4d chunk[%d] temp   klei %11.5f  mine %11.5f\n", tick, i,
               x.temperature, y.temperature);
        clean = false;
      }
      if (a->numElementChunkInfos == g_prev_chunk_infos &&
          std::fabs(static_cast<double>(x.deltaKJ) - y.deltaKJ) > 1e-3) {
        printf("  tick %-4d chunk[%d] deltaKJ klei %11.5f  mine %11.5f\n", tick, i,
               x.deltaKJ, y.deltaKJ);
        clean = false;
      }
    }
  }
  g_prev_chunk_infos = a->numElementChunkInfos;
  // The handles an Add reports back. Every scenario before the chunk probes sent
  // `callbackIdx = -1` for everything, so this array was empty on both sides always and
  // "the two allocators agree" was a claim nothing had tested.
  if (a->numComponentStateChangedMessages != b->numComponentStateChangedMessages) {
    printf("  tick %-4d componentStateChanged klei %d, mine %d\n", tick,
           a->numComponentStateChangedMessages, b->numComponentStateChangedMessages);
    clean = false;
  } else {
    for (int i = 0; i < a->numComponentStateChangedMessages; ++i) {
      const ComponentStateChangedMessage& x = a->componentStateChangedMessages[i];
      const ComponentStateChangedMessage& y = b->componentStateChangedMessages[i];
      if (x.callbackIdx == y.callbackIdx && x.simHandle == y.simHandle) continue;
      printf("  tick %-4d stateChanged[%d] klei cb %d handle %d, mine cb %d handle %d\n",
             tick, i, x.callbackIdx, x.simHandle, y.callbackIdx, y.simHandle);
      clean = false;
    }
  }

  // The two element components' output, and the two message callbacks that share their
  // machinery. All four are **per frame**, and `emittedMassEntries` accumulates across the
  // substeps of a frame the way `deltaKJ` does, so a tick on which Klei's scheduler ran two
  // frames in one call carries twice as much as ours and cannot be compared. Skipped rather
  // than fudged: the count is printed above whenever it happens.
  if (a->numFramesProcessed == b->numFramesProcessed) {
    if (a->numRemovedMassEntries != b->numRemovedMassEntries) {
      printf("  tick %-4d removedMassEntries   klei %d, mine %d\n", tick,
             a->numRemovedMassEntries, b->numRemovedMassEntries);
      clean = false;
    } else {
      for (int i = 0; i < a->numRemovedMassEntries; ++i) {
        const ConsumedMassInfo& x = a->removedMassEntries[i];
        const ConsumedMassInfo& y = b->removedMassEntries[i];
        if (x.simHandle == y.simHandle && x.removedElemIdx == y.removedElemIdx &&
            x.diseaseIdx == y.diseaseIdx && x.diseaseCount == y.diseaseCount &&
            std::fabs(static_cast<double>(x.mass) - y.mass) <= 1e-5 &&
            std::fabs(static_cast<double>(x.temperature) - y.temperature) <= 1e-3) {
          continue;
        }
        printf("  tick %-4d removed[%d] klei h%d e%u %.6f kg %.4f K d%u/%d  mine h%d e%u"
               " %.6f kg %.4f K d%u/%d\n",
               tick, i, x.simHandle, x.removedElemIdx, x.mass, x.temperature, x.diseaseIdx,
               x.diseaseCount, y.simHandle, y.removedElemIdx, y.mass, y.temperature,
               y.diseaseIdx, y.diseaseCount);
        clean = false;
      }
    }
    if (a->numEmittedMassEntries != b->numEmittedMassEntries) {
      printf("  tick %-4d emittedMassEntries   klei %d, mine %d\n", tick,
             a->numEmittedMassEntries, b->numEmittedMassEntries);
      clean = false;
    } else {
      for (int i = 0; i < a->numEmittedMassEntries; ++i) {
        const EmittedMassInfo& x = a->emittedMassEntries[i];
        const EmittedMassInfo& y = b->emittedMassEntries[i];
        if (x.elemIdx == y.elemIdx &&
            std::fabs(static_cast<double>(x.mass) - y.mass) <= 1e-5 &&
            std::fabs(static_cast<double>(x.temperature) - y.temperature) <= 1e-3) {
          continue;
        }
        printf("  tick %-4d emitted[%d] klei e%u %.6f kg %.4f K  mine e%u %.6f kg %.4f K\n",
               tick, i, x.elemIdx, x.mass, x.temperature, y.elemIdx, y.mass, y.temperature);
        clean = false;
      }
    }
    // `ProcessConsumeDisease`'s callbacks.
    if (a->numDiseaseConsumptionCallbacks != b->numDiseaseConsumptionCallbacks) {
      printf("  tick %-4d diseaseConsumption   klei %d, mine %d\n", tick,
             a->numDiseaseConsumptionCallbacks, b->numDiseaseConsumptionCallbacks);
      clean = false;
    } else {
      for (int i = 0; i < a->numDiseaseConsumptionCallbacks; ++i) {
        const DiseaseConsumptionCallback& x = a->diseaseConsumptionCallbacks[i];
        const DiseaseConsumptionCallback& y = b->diseaseConsumptionCallbacks[i];
        if (x.callbackIdx == y.callbackIdx && x.diseaseIdx == y.diseaseIdx &&
            x.diseaseCount == y.diseaseCount) {
          continue;
        }
        printf("  tick %-4d diseaseConsumed[%d] klei cb%d d%u/%d  mine cb%d d%u/%d\n", tick, i,
               x.callbackIdx, x.diseaseIdx, x.diseaseCount, y.callbackIdx, y.diseaseIdx,
               y.diseaseCount);
        clean = false;
      }
    }
    // The disease emitter's report list, indexed by handle slot. A list nothing
    // sends is a list nothing tests, so the emitter scenarios send it.
    if (a->numDiseaseEmittedInfos != b->numDiseaseEmittedInfos) {
      printf("  tick %-4d diseaseEmittedInfos  klei %d, mine %d\n", tick,
             a->numDiseaseEmittedInfos, b->numDiseaseEmittedInfos);
      clean = false;
    } else {
      for (int i = 0; i < a->numDiseaseEmittedInfos; ++i) {
        const DiseaseEmittedInfo& x = a->diseaseEmittedInfos[i];
        const DiseaseEmittedInfo& y = b->diseaseEmittedInfos[i];
        if (x.diseaseIdx == y.diseaseIdx && x.count == y.count) continue;
        printf("  tick %-4d diseaseEmitted[%d] klei d%u/%d  mine d%u/%d\n", tick, i,
               x.diseaseIdx, x.count, y.diseaseIdx, y.count);
        clean = false;
      }
    }
    if (a->numDiseaseConsumedInfos != b->numDiseaseConsumedInfos) {
      printf("  tick %-4d diseaseConsumedInfos klei %d, mine %d\n", tick,
             a->numDiseaseConsumedInfos, b->numDiseaseConsumedInfos);
      clean = false;
    }
    if (a->numMassConsumedCallbacks != b->numMassConsumedCallbacks) {
      printf("  tick %-4d massConsumedCallbacks klei %d, mine %d\n", tick,
             a->numMassConsumedCallbacks, b->numMassConsumedCallbacks);
      clean = false;
    } else {
      for (int i = 0; i < a->numMassConsumedCallbacks; ++i) {
        const MassConsumedCallback& x = a->massConsumedCallbacks[i];
        const MassConsumedCallback& y = b->massConsumedCallbacks[i];
        if (x.callbackIdx == y.callbackIdx && x.elemIdx == y.elemIdx &&
            x.diseaseIdx == y.diseaseIdx && x.diseaseCount == y.diseaseCount &&
            std::fabs(static_cast<double>(x.mass) - y.mass) <= 1e-5 &&
            std::fabs(static_cast<double>(x.temperature) - y.temperature) <= 1e-3) {
          continue;
        }
        printf("  tick %-4d consumedCb[%d] klei cb%d e%u %.6f kg %.4f K d%u/%d  mine cb%d"
               " e%u %.6f kg %.4f K d%u/%d\n",
               tick, i, x.callbackIdx, x.elemIdx, x.mass, x.temperature, x.diseaseIdx,
               x.diseaseCount, y.callbackIdx, y.elemIdx, y.mass, y.temperature,
               y.diseaseIdx, y.diseaseCount);
        clean = false;
      }
    }
    if (a->numMassEmittedCallbacks != b->numMassEmittedCallbacks) {
      printf("  tick %-4d massEmittedCallbacks klei %d, mine %d\n", tick,
             a->numMassEmittedCallbacks, b->numMassEmittedCallbacks);
      clean = false;
    } else {
      for (int i = 0; i < a->numMassEmittedCallbacks; ++i) {
        const MassEmittedCallback& x = a->massEmittedCallbacks[i];
        const MassEmittedCallback& y = b->massEmittedCallbacks[i];
        if (x.callbackIdx == y.callbackIdx && x.elemIdx == y.elemIdx &&
            x.suceeded == y.suceeded && x.diseaseIdx == y.diseaseIdx &&
            x.diseaseCount == y.diseaseCount &&
            std::fabs(static_cast<double>(x.mass) - y.mass) <= 1e-5 &&
            std::fabs(static_cast<double>(x.temperature) - y.temperature) <= 1e-3) {
          continue;
        }
        printf("  tick %-4d emittedCb[%d] klei cb%d e%u ok%u %.6f kg %.4f K d%u/%d  mine"
               " cb%d e%u ok%u %.6f kg %.4f K d%u/%d\n",
               tick, i, x.callbackIdx, x.elemIdx, x.suceeded, x.mass, x.temperature,
               x.diseaseIdx, x.diseaseCount, y.callbackIdx, y.elemIdx, y.suceeded, y.mass,
               y.temperature, y.diseaseIdx, y.diseaseCount);
        clean = false;
      }
    }
    // Bare callback ids: `ModifyCell`'s `callbackIdx`, and an element emitter's two blocked
    // callbacks. A published array with no comparison behind
    // it is a subsystem nothing is testing.
    if (a->numCallbackInfo != b->numCallbackInfo) {
      printf("  tick %-4d callbackInfo         klei %d, mine %d\n", tick, a->numCallbackInfo,
             b->numCallbackInfo);
      clean = false;
    } else {
      for (int i = 0; i < a->numCallbackInfo; ++i) {
        if (a->callbackInfo[i].callbackIdx == b->callbackInfo[i].callbackIdx) continue;
        printf("  tick %-4d callback[%d] klei %d, mine %d\n", tick, i,
               a->callbackInfo[i].callbackIdx, b->callbackInfo[i].callbackIdx);
        clean = false;
      }
    }
    // `radiationConsumedCallbacks`. Filled only by `ProcessCellRadiationChanges` — the
    // emitters never touch it — and it is the second array this harness has found with no
    // comparison behind it.
    if (a->numRadiationConsumedCallbacks != b->numRadiationConsumedCallbacks) {
      printf("  tick %-4d radConsumedCb        klei %d, mine %d\n", tick,
             a->numRadiationConsumedCallbacks, b->numRadiationConsumedCallbacks);
      clean = false;
    } else {
      for (int i = 0; i < a->numRadiationConsumedCallbacks; ++i) {
        const ConsumedRadiationCallback& x = a->radiationConsumedCallbacks[i];
        const ConsumedRadiationCallback& y = b->radiationConsumedCallbacks[i];
        if (x.callbackIdx == y.callbackIdx && x.gameCell == y.gameCell &&
            std::fabs(static_cast<double>(x.radiation) - y.radiation) <= 1e-4) {
          continue;
        }
        printf("  tick %-4d radCb[%d] klei cb%d cell %d %.6f  mine cb%d cell %d %.6f\n",
               tick, i, x.callbackIdx, x.gameCell, x.radiation, y.callbackIdx, y.gameCell,
               y.radiation);
        clean = false;
      }
    }
    // A solid element emitter spawns ore instead of filling a cell, which is the only way
    // this list can be non-empty without a dig or a transition.
    if (a->numSpawnOreInfo != b->numSpawnOreInfo) {
      printf("  tick %-4d spawnOreInfo         klei %d, mine %d\n", tick,
             a->numSpawnOreInfo, b->numSpawnOreInfo);
      clean = false;
    } else {
      for (int i = 0; i < a->numSpawnOreInfo; ++i) {
        const SpawnOreInfo& x = a->spawnOreInfo[i];
        const SpawnOreInfo& y = b->spawnOreInfo[i];
        if (x.cellIdx == y.cellIdx && x.elemIdx == y.elemIdx &&
            std::fabs(static_cast<double>(x.mass) - y.mass) <= 1e-5 &&
            std::fabs(static_cast<double>(x.temperature) - y.temperature) <= 1e-3 &&
            x.diseaseIdx == y.diseaseIdx && x.diseaseCount == y.diseaseCount) {
          continue;
        }
        printf("  tick %-4d ore[%d] klei cell %d e%u %.6f kg %.4f K d%u:%d  mine cell %d e%u"
               " %.6f kg %.4f K d%u:%d\n",
               tick, i, x.cellIdx, x.elemIdx, x.mass, x.temperature, x.diseaseIdx,
               x.diseaseCount, y.cellIdx, y.elemIdx, y.mass, y.temperature, y.diseaseIdx,
               y.diseaseCount);
        clean = false;
      }
    }
  }

  const struct {
    const char* name;
    int ka, kb;
  } melted[] = {
      {"buildingOverheat", a->numBuildingOverheatInfos, b->numBuildingOverheatInfos},
      {"buildingNoLongerOverheat", a->numBuildingNoLongerOverheatedInfos,
       b->numBuildingNoLongerOverheatedInfos},
      {"buildingMelted", a->numBuildingMeltedInfos, b->numBuildingMeltedInfos},
  };
  for (const auto& m : melted) {
    if (m.ka == m.kb) continue;
    printf("  tick %-4d %-24s klei %d, mine %d\n", tick, m.name, m.ka, m.kb);
    clean = false;
  }
  return clean;
}

// Per-tick chunk state from both sims, printed past the first divergence. `--chunks`.
bool g_show_chunks = false;

void DumpChunks(const GameDataUpdate* a, const GameDataUpdate* b, int tick) {
  printf("  tick %-4d chunks klei %d mine %d  melted klei %d mine %d\n", tick,
         a->numElementChunkInfos, b->numElementChunkInfos, a->numElementChunkMeltedInfos,
         b->numElementChunkMeltedInfos);
  const int n = a->numElementChunkInfos < b->numElementChunkInfos ? a->numElementChunkInfos
                                                                  : b->numElementChunkInfos;
  for (int i = 0; i < n; ++i) {
    printf("      [%d] temp k%11.5f m%11.5f   deltaKJ k%11.5f m%11.5f\n", i,
           a->elementChunkInfos[i].temperature, b->elementChunkInfos[i].temperature,
           a->elementChunkInfos[i].deltaKJ, b->elementChunkInfos[i].deltaKJ);
  }
}

bool g_no_wait = false;
long g_first_wait_us = 20000;
long g_tick_wait_us = 200;
// Zero by default: `Boot` now joins the previous scenario's `SimThread` with `SIM_Shutdown`
// rather than waiting a guessed interval for it. `--bootwait N` puts the old wait back for
// bisecting.
long g_boot_wait_us = 0;

// `Sleep(1)` is not one millisecond: the default Windows timer tick rounds it up to ~15 ms, and
// at two waits per tick that alone took the suite from half a second to over two minutes. The
// wait here is measured in microseconds and spent spinning, because what we are waiting for is
// another core finishing a frame that takes tens of microseconds, not a scheduler quantum.
void WaitUs(long us) {
  if (us <= 0) return;
  LARGE_INTEGER freq, start, now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  const long long target = start.QuadPart + (freq.QuadPart * us) / 1000000;
  for (int spins = 0;; ++spins) {
    QueryPerformanceCounter(&now);
    if (now.QuadPart >= target) return;
    // Yield the core every so often so this still terminates if `SimThread` were ever scheduled
    // on top of us, without paying a context switch on the common path.
    if ((spins & 0xFF) == 0xFF) SwitchToThread();
  }
}

// Which backends have already had a tick since their last `Boot`. A scenario drives both
// backends alternately *within* each tick, so "the backend the last tick drove" is not the same
// question and answering it that way charges every tick the warm-up wait.
std::vector<const Backend*> g_warm;
bool Warm(const Backend* s) {
  for (const Backend* p : g_warm) if (p == s) return true;
  g_warm.push_back(s);
  return false;
}

const GameDataUpdate* Boot(Backend* s, const Tables& t, const std::vector<uint8_t>& w) {
  // A fresh `Boot` means a fresh `SimThread`, so this backend owes the warm-up wait again.
  for (size_t i = 0; i < g_warm.size(); ++i) {
    if (g_warm[i] == s) { g_warm.erase(g_warm.begin() + i); break; }
  }
  // The largest single source of the suite's run-to-run drift is here, not in `Tick`. The
  // previous scenario's `SimThread` is still finishing when the next one boots, and what it
  // finishes into is the world the new scenario is about to publish — so a scenario's tick 1
  // could come back carrying the tail of the scenario before it. That is why the noise sat at
  // tick 1 and why waiting longer at tick 1 did nothing: by then the damage was already done.
  //
  // THIS USED TO BE A 5 ms WAIT, AND A WAIT IS A BET. It took the `all` run from a different
  // output every time to *usually* byte-identical -- measured at
  // roughly one run in twenty still diverging, which is not a golden anybody can gate on. The
  // instrumented capture showed exactly what survived: a scenario's boot warm-up lap landing
  // at tick 0 instead of tick 1, and a doubled lap later in that scenario as the pipeline
  // resynchronised.
  //
  // So stop betting and close the thread instead. `SIM_Shutdown` calls
  // `CleanUp`, which null-checks the sim global and joins the thread through
  // `Thread::Joinable`, then runs `Sim::~Sim` and nulls the global. Both halves are
  // null-guarded, so this is safe on a backend that has never been initialised and safe after
  // a scenario that shut down for itself. The old `SimThread` is then *gone* rather than
  // probably-gone, and the next `Start` begins from a known state.
  //
  // Klei's own handshake makes this sufficient rather than merely better: `FrameSync::GameSync`
  // blocks on `mSimReadyToSwap` until the sim has completed a lap, so with no
  // stale thread able to set that flag, every publication after this point is the new
  // scenario's. `--bootwait` survives for bisecting; it is no longer load-bearing.
  s->shutdown();
  if (!g_no_wait && g_boot_wait_us > 0) WaitUs(g_boot_wait_us);
  s->initialize(&Quiet);
  s->Send(SimMessageHash::Elements_CreateTable, t.elements);
  s->Send(SimMessageHash::Disease_CreateTable, t.diseases);
  s->Send(SimMessageHash::SimData_InitializeFromCells, w);
  return static_cast<const GameDataUpdate*>(s->SendEmpty(SimMessageHash::Start));
}

// One active region. The game sends one of these per active area and the payload of a
// single `NewGameFrame` message is the whole array, so a two-asteroid world arrives as one
// message carrying two structs. Half-open in x; y follows the harness's long-standing
// `height - 1` clamp, which is what the game itself sends (see "NewGameFrame maxX / maxY").
struct Rect {
  int32_t x0, y0, x1, y1;
};

// A scenario that wants more than the default single full-extent region supplies one of
// these. Taking the world means a region can be written relative to the room it covers
// rather than hard-coded against a size the world builder chose.
using RegionFn = std::vector<Rect> (*)(const World&);

long g_spins = 0, g_frames = 0, g_ticks = 0;

const GameDataUpdate* Tick(Backend* s, const World& w, std::vector<uint8_t>* visible,
                           float dt = 0.2f, const std::vector<Rect>* regions = nullptr) {
  std::vector<NewGameFrame> frames;
  if (regions && !regions->empty()) {
    for (const Rect& r : *regions) {
      NewGameFrame f{};
      f.elapsedSeconds = dt;
      f.minX = r.x0;
      f.minY = r.y0;
      f.maxX = r.x1;
      f.maxY = r.y1;
      f.currentCosmicRadiationIntensity = w.cosmic;
      frames.push_back(f);
    }
  } else {
    NewGameFrame f{};
    f.elapsedSeconds = dt;
    f.maxX = w.width;
    f.maxY = w.height - 1;
    f.currentCosmicRadiationIntensity = w.cosmic;
    frames.push_back(f);
  }
  // `elapsedSeconds` is read off the *first* struct by both sims; the rest carry it anyway
  // so that a payload dumped from here reads the way the game's would.
  s->handle_message(static_cast<int32_t>(SimMessageHash::SimFrameManager_NewGameFrame),
                    static_cast<int>(frames.size() * sizeof(NewGameFrame)),
                    reinterpret_cast<const uint8_t*>(frames.data()));
  auto prepare = [&] {
    return static_cast<const GameDataUpdate*>(
        s->handle_message(static_cast<int32_t>(SimMessageHash::PrepareGameData),
                          static_cast<int>(visible->size()), visible->data()));
  };
  // Klei runs the frame on its own `SimThread`, and `NewGameFrame` only queues it.
  // `PrepareGameData` publishes whatever that thread has finished, so a call that wins the race
  // against the queue comes back with `numFramesProcessed == 0` and hands us the *previous*
  // frame's grid — and the grids it hands back are Klei's live buffers, so the thread can go on
  // writing them while we compare. Nothing about either sim is random; this is, and it was the
  // whole of what made half the suite differ between two runs of one unchanged DLL.
  //
  // Re-asking is *not* the fix: a second `PrepareGameData` advances the queue rather than merely
  // publishing, so spinning on one makes Klei run a frame our sim never ran (measured: 2548
  // frames becomes 2600, and 24 scenarios fail that otherwise pass). The only safe instrument is
  // to wait before the single call, which changes nothing about how many frames either side runs.
  //
  // Three waits are needed and all three are load-bearing — dropping any one of them puts the
  // suite back to a different output every run (see `Boot` for the third and largest):
  //   * the first tick after a `Boot`, where `SimThread` is still starting up;
  //   * every other tick, where it only has to land a frame it has already begun;
  //   * `Boot` itself.
  // Our own sim runs the frame inside `NewGameFrame` and always reports 1, so none of this
  // applies to it — but the wait is unconditional so that both sides are driven identically.
  if (!g_no_wait) WaitUs(Warm(s) ? g_tick_wait_us : g_first_wait_us);
  const GameDataUpdate* u = prepare();
  if (u && u->numFramesProcessed == 0) ++g_spins;
  // Only our DLL exports `SIM_DebugRandomState`, so that is how a backend says which side
  // it is without threading a flag through every scenario.
  if (!GetProcAddress(s->module, "SIM_DebugRandomState")) {
    if (u) g_frames += u->numFramesProcessed;
    ++g_ticks;
  }
  return u;
}

// Print the same rectangle out of both sims, row by row, highest y first so the picture
// reads the way the world looks.
//
// The divergence report says which cells differ; it does not say what the neighbourhood
// looks like, and for a flow defect the neighbourhood is the whole story — a cell that is
// 1.7 K wrong because the *other* cell got the mass is a different bug from one that is
// 1.7 K wrong on its own. Rows that are identical in both sims are dropped, so a dump of a
// large room stays readable.
struct DumpRect {
  bool on = false;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  // t emperature, m ass, e lement, f low.r, g = flow.g, r adiation, c = disease count,
  // and the backwall: z = element, x = mass, w = temperature
  //
  // The flow texture is the only per-cell field that records *movement* rather than state,
  // so it is the field that shows a transfer the two sims disagree about on the tick it
  // happens, before the mass difference is large enough to see. `sublvac` diverges there
  // two ticks before it diverges anywhere else.
  char field = 't';
};

bool g_dump_exact = false;


void Dump(const DumpRect& d, const GameDataUpdate* a, const GameDataUpdate* b, int width,
          int tick) {
  if (!d.on || !a || !b) return;
  const char lower = static_cast<char>(d.field | 0x20);
  printf("  --- tick %d, %s ---\n", tick,
         lower == 'm' ? "mass"
                      : (lower == 'e' ? "element"
                                      : (lower == 'f' ? "flow.r"
                                                      : (lower == 'g' ? "flow.g"
                                        : (lower == 'z' ? "backwall.element"
                                        : (lower == 'x' ? "backwall.mass"
                                        : (lower == 'w' ? "backwall.temperature"
                                        : (lower == 'r' ? "radiation"
                                        : (lower == 'c' ? "diseaseCount"
                                        : (lower == 's' ? "tex.sunlight"
                                        : (lower == 'i' ? "diseaseIdx"
                                                        : "temperature")))))))))));
  for (int y = d.y1; y >= d.y0; --y) {
    auto value = [&](const GameDataUpdate* g, int x) -> double {
      const size_t c = static_cast<size_t>(y) * width + x;
      if (d.field == 'm' || d.field == 'M') return g->mass[c];
      if (d.field == 'e' || d.field == 'E') return g->elementIdx[c];
      if (lower == 'r') return g->radiation[c];
      if (lower == 'c') return g->diseaseCount[c];
      // `i` is the disease *index*, and it is the field that separates a cell holding germs
      // from a cell holding a count with nothing attached to it. `Disease::PostProcess`
      // skips every cell whose index is 0xFF, so an orphaned count — index cleared, count
      // left standing — never decays and never grows, whatever the table says.
      if (lower == 'i') return g->diseaseIdx[c];
      // `s` is the sunlight texture, one byte per cell. It is a **per-world** texture: it
      // stays zero until `DefineWorldOffsets` tells the sim where the worlds are.
      if (lower == 's') return static_cast<const uint8_t*>(g->propertyTextureExposedToSunlight)[c];
      if (lower == 'z') return g->backwallElement[c];
      if (lower == 'x') return g->backwallMass[c];
      if (lower == 'w') return g->backwallTemperature[c];
      if (lower == 'f' || lower == 'g') {
        const auto* f = static_cast<const float*>(g->propertyTextureFlow);
        if (!f) return 0.0;
        return f[c * 2 + (lower == 'g' ? 1 : 0)];
      }
      return g->temperature[c];
    };
    bool same = true;
    for (int x = d.x0; x <= d.x1; ++x) {
      if (value(a, x) != value(b, x)) same = false;
    }
    // An upper-case field letter keeps the matching rows: a row that agrees is still
    // evidence, and suppressing it hides the case where both sims are doing something
    // unexpected in the same place.
    if (same && d.field >= 'a') continue;
    printf("   y%3d klei", y);
    for (int x = d.x0; x <= d.x1; ++x) printf(" %12.6g", value(a, x));
    printf("\n        mine");
    for (int x = d.x0; x <= d.x1; ++x) {
      const double m = value(b, x), k = value(a, x);
      printf(" %12.6g", m);
      (void)k;
    }
    printf("\n");
    // Four decimals hide a one-ulp difference, and a one-ulp difference in *temperature* is
    // not cosmetic: `GasShuffle` keeps the coolest candidate, so a last-bit disagreement
    // picks a different cell to swap and the two sims part company for good. With
    // `--dumpexact` every differing cell in the row is also printed at full precision.
    if (!g_dump_exact) continue;
    for (int x = d.x0; x <= d.x1; ++x) {
      const double k = value(a, x), m = value(b, x);
      if (k == m) continue;
      printf("          (%2d,%2d) klei %.9g  mine %.9g\n", x, y, k, m);
    }
  }
}

// ------------------------------------------------------------------- scenarios

// Solid granite everywhere. Nothing can move, so Klei's physics has nothing to do and a
// physics-free replacement must agree with it forever. If this diverges, the divergence
// is in the ABI or the projection, which is exactly what this build is meant to get
// right.
World Equilibrium(const Tables& t) {
  World w;
  w.Init(24, 16, static_cast<uint16_t>(t.IndexOf(kGranite)), 2000.0f, 293.15f);
  return w;
}

// ------------------------------------------------------------------- buildings
//
// A building is not in the world payload. It arrives as a message and lives outside the
// grid, so these scenarios need a hook that runs after `Boot` and before the first tick;
// the same messages go to both sims, in the same order, and the handles each allocates are
// compared as part of the result.
//
// Handles are not read back from `componentStateChangedMessages` here, deliberately. Both
// sims hand out slot 0 for the first registration and slot 1 for the second, and *that they
// agree* is one of the things worth testing — a scenario that looked its handles up would
// pass even if the two allocators disagreed.
using BuildingSetup = void (*)(Backend*, const Tables&, const World&);

// The same thing on a schedule. `setup` can only place things before tick 1, which is enough
// for a building that never changes, but the chunk messages that matter most — move, remove,
// energy, a retired adjuster — are the ones the game sends to a component that is already
// running. This hook fires immediately before each tick, on both sims, with the tick number.
using TickHook = void (*)(Backend*, const Tables&, const World&, int);

constexpr float kNeverOverheats = 3.4028235e38f;

void SendBuilding(Backend* s, const Tables& t, int32_t hash, float mass, float temperature,
                  float conductivity, float operating_kw, float overheat, int x, int y,
                  int w, int h) {
  AddBuildingHeatExchangeMessage m{};
  m.callbackIdx = -1;
  m.elemIdx = static_cast<uint16_t>(t.IndexOf(hash));
  m.mass = mass;
  m.temperature = temperature;
  m.thermalConductivity = conductivity;
  m.overheatTemperature = overheat;
  m.operatingKilowatts = operating_kw;
  m.minX = x;
  m.minY = y;
  m.maxX = x + w;
  m.maxY = y + h;
  s->handle_message(static_cast<int32_t>(SimMessageHash::AddBuildingHeatExchange),
                    sizeof(m), reinterpret_cast<const uint8_t*>(&m));
}

// `SetDebugProperties` is what sets the absolute rate of every building transfer, and the
// game sends it every frame. Without it both sims sit on the constructor's 0.001 — which is
// the same number, so this is here to pin the value rather than to change it.
void SendScales(Backend* s, float building, float building_to_building) {
  DebugProperties d{};
  d.buildingTemperatureScale = building;
  d.buildingToBuildingTemperatureScale = building_to_building;
  s->handle_message(static_cast<int32_t>(SimMessageHash::SetDebugProperties), sizeof(d),
                    reinterpret_cast<const uint8_t*>(&d));
}

// One hot copper machine in cold granite, not running. Everything in this scenario is the
// cell exchange and nothing else: the world is solid so no flow, and the building starts
// 100 K above the rock so heat moves out of it into all four cells it covers.
void BuildingHot(Backend* s, const Tables& t, const World&) {
  SendScales(s, 0.001f, 0.001f);
  SendBuilding(s, t, kCopper, 400.0f, 400.0f, 1.0f, 0.0f, kNeverOverheats, 7, 7, 2, 2);
}

// The same machine, cold and *running*. `operating_kilowatts` is added after the clamp that
// holds the building between the hottest and coldest thing it touched, so this is the probe
// that says whether a machine can drive itself above its surroundings — and it is also the
// one that fires `buildingOverheatInfos`, because the overheat threshold is set low enough
// to be crossed within the run.
void BuildingRunning(Backend* s, const Tables& t, const World&) {
  SendScales(s, 0.001f, 0.001f);
  SendBuilding(s, t, kCopper, 400.0f, 300.0f, 1.0f, 8.0f, 300.2f, 7, 7, 2, 2);
}

// `BuildingRunning`'s machine moved into `RegionsOverlap`'s overlap (columns 12..20).
// Everything else is identical — same element, same mass, same 8 kW, same overheat point — so a
// difference between `buildingrun` and `buildinglap` is attributable to region coverage and to
// nothing else.
void BuildingRunningInOverlap(Backend* s, const Tables& t, const World&) {
  SendScales(s, 0.001f, 0.001f);
  SendBuilding(s, t, kCopper, 400.0f, 300.0f, 1.0f, 8.0f, 300.2f, 14, 7, 2, 2);
}

// Two machines touching, in vacuum, so the cell exchange has nothing to work with (a cell
// with no mass is skipped) and the only thing running is building-to-building transfer.
// The zero heat capacity nothing guards. `BuildingToBuildingHeatExchange::Update` refuses the
// building on the FAR side of each pair when its heat capacity is not positive (`other_hc <=
// 0`) and then divides by the near side's without checking it at all — so a
// contact group whose OWNER registered with no mass runs `1 / 0`. The proposal it scales is
// `q = delta * source_hc`, and when the owner is the hotter body `source_hc` is that same
// zero, so the product is `inf * 0`: a NaN, on every pair in the group, every substep.
//
// Klei's clamp is a float minimum and maximum, which turns it into the group's bound; a
// comparison clamp would keep it. Both buildings are in
// vacuum so the cell sweep contributes nothing (a massless building fails
// `per_cell_heat_capacity > 0` anyway) and every number in the scenario is this one exchange.
void BuildingZeroHeatCapacity(Backend* s, const Tables& t, const World&) {
  SendScales(s, 0.001f, 0.001f);
  // Handle 0, the group owner: no mass, so `heat_capacity` is 0. Hotter than its partner, so
  // `delta < 0` picks ITS heat capacity as the source and drives `q` to zero as well.
  SendBuilding(s, t, kCopper, 0.0f, 400.0f, 1.0f, 0.0f, kNeverOverheats, 7, 7, 2, 2);
  // Handle 1, an ordinary machine with a real heat capacity, as the control.
  SendBuilding(s, t, kCopper, 400.0f, 300.0f, 1.0f, 0.0f, kNeverOverheats, 9, 7, 2, 2);

  RegisterBuildingToBuildingHeatExchangeMessage reg{};
  reg.callbackIdx = -1;
  reg.structureTemperatureHandler = 0;
  s->handle_message(
      static_cast<int32_t>(SimMessageHash::AddBuildingToBuildingHeatExchange), sizeof(reg),
      reinterpret_cast<const uint8_t*>(&reg));

  AddBuildingToBuildingHeatExchangeMessage add{};
  add.selfHandler = 0;
  add.buildingInContactHandle = 1;
  add.cellsInContact = 2;
  s->handle_message(
      static_cast<int32_t>(
          SimMessageHash::AddInContactBuildingToBuildingToBuildingHeatExchange),
      sizeof(add), reinterpret_cast<const uint8_t*>(&add));
}

void BuildingContact(Backend* s, const Tables& t, const World&) {
  SendScales(s, 0.001f, 0.001f);
  // Deliberately unequal masses. With equal ones "A gives to B" and "B gives to A" produce
  // the same number and the probe cannot tell how many times, or in which direction, the
  // exchange ran.
  SendBuilding(s, t, kCopper, 400.0f, 400.0f, 1.0f, 0.0f, kNeverOverheats, 7, 7, 2, 2);
  SendBuilding(s, t, kCopper, 400.0f, 300.0f, 1.0f, 0.0f, kNeverOverheats, 9, 7, 2, 2);

  RegisterBuildingToBuildingHeatExchangeMessage reg{};
  reg.callbackIdx = -1;
  reg.structureTemperatureHandler = 0;  // the first building registered above
  s->handle_message(
      static_cast<int32_t>(SimMessageHash::AddBuildingToBuildingHeatExchange), sizeof(reg),
      reinterpret_cast<const uint8_t*>(&reg));

  AddBuildingToBuildingHeatExchangeMessage add{};
  add.selfHandler = 0;  // the contact group just created
  add.buildingInContactHandle = 1;
  add.cellsInContact = 2;
  s->handle_message(
      static_cast<int32_t>(
          SimMessageHash::AddInContactBuildingToBuildingToBuildingHeatExchange),
      sizeof(add), reinterpret_cast<const uint8_t*>(&add));
}

// A machine that starts *above* its overheat temperature and cools through it. The only
// probe for `buildingNoLongerOverheatedInfos`, which fires on the one substep the crossing
// happens and never again — an off-by-one there would fire it every substep instead, and
// nothing else in the suite would notice.
void BuildingCooling(Backend* s, const Tables& t, const World&) {
  SendScales(s, 0.001f, 0.001f);
  SendBuilding(s, t, kCopper, 400.0f, 400.0f, 1.0f, 0.0f, 399.9f, 7, 7, 2, 2);
}

// A hot machine in vacuum with one solid tile under it. Nothing but that tile can take its
// heat, which is the case where the per-cell heat capacity matters: the building trades
// against a quarter of itself and the result is averaged back over four cells.
void BuildingVacuum(Backend* s, const Tables& t, const World&) {
  SendScales(s, 0.001f, 0.001f);
  SendBuilding(s, t, kCopper, 400.0f, 500.0f, 1.0f, 0.0f, kNeverOverheats, 7, 7, 2, 2);
}

// ------------------------------------------------------------------- element chunks
//
// A chunk is a lump of matter outside the grid — a dropped rock, the contents of a locker,
// a duplicant's suit — and like a building it arrives entirely as messages. Unlike a
// building it is a *point*: one cell, and optionally the tile under that cell.
//
// The chunk messages are the busiest thing the sim does that nothing here implemented until
// now (about 12,000 calls in 38 seconds of a real game), and none of them had ever
// been sent by this harness — so before these scenarios existed, every chunk field in
// `GameDataUpdate` was empty on both sides and agreed for the least interesting reason.
//
// Handles are written as literals rather than read back out of
// `componentStateChangedMessages`, for the same reason the building probes do it: that the
// two allocators hand out the *same* handle is one of the things being tested, and a
// scenario that looked its handles up would pass even if they disagreed.

void SendChunk(Backend* s, const Tables& t, int32_t hash, float mass, float temperature,
               float surface_area, float thickness, float ground_scale, int game_cell,
               int32_t callback_idx = -1) {
  AddElementChunkMessage m{};
  m.gameCell = game_cell;
  m.callbackIdx = callback_idx;
  m.mass = mass;
  m.temperature = temperature;
  // Sent separately and only ever used as the ratio `surfaceArea / thickness`. Kept as two
  // fields here so a scenario can vary the ratio without both halves being 1.
  m.surfaceArea = surface_area;
  m.thickness = thickness;
  m.groundTransferScale = ground_scale;
  m.elementIdx = static_cast<uint16_t>(t.IndexOf(hash));
  s->handle_message(static_cast<int32_t>(SimMessageHash::AddElementChunk), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

void SendChunkMove(Backend* s, int32_t handle, int game_cell) {
  MoveElementChunkMessage m{};
  m.handle = handle;
  m.gameCell = game_cell;
  s->handle_message(static_cast<int32_t>(SimMessageHash::MoveElementChunk), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

void SendChunkData(Backend* s, int32_t handle, float temperature, float heat_capacity) {
  SetElementChunkDataMessage m{};
  m.handle = handle;
  m.temperature = temperature;
  m.heatCapacity = heat_capacity;
  s->handle_message(static_cast<int32_t>(SimMessageHash::SetElementChunkData), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

void SendChunkEnergy(Backend* s, int32_t handle, float delta_kj) {
  ModifyElementChunkEnergyMessage m{};
  m.handle = handle;
  m.deltaKJ = delta_kj;
  s->handle_message(static_cast<int32_t>(SimMessageHash::ModifyElementChunkEnergy),
                    sizeof(m), reinterpret_cast<const uint8_t*>(&m));
}

// `ModifyCellEnergy`. No handle and no callback: the id is carried only so Klei's error
// message can name the sender, and nothing in the sim reads it otherwise.
void SendCellEnergy(Backend* s, int32_t game_cell, float kilojoules, float max_temperature,
                    int32_t id = 0) {
  ModifyCellEnergyMessage m{};
  m.cellIdx = game_cell;
  m.kilojoules = kilojoules;
  m.maxTemperature = max_temperature;
  m.id = id;
  s->handle_message(static_cast<int32_t>(SimMessageHash::ModifyCellEnergy), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

// `ModifyCell`, which no scenario in this suite had ever sent either — every world here
// arrives through the payload. `replaceType` 1 is Replace, which overwrites element, mass
// and temperature outright.
void SendModifyCell(Backend* s, const Tables& t, int32_t game_cell, int32_t hash, float mass,
                    float temperature, uint8_t replace_type = 1, uint8_t add_sub_type = 0,
                    uint8_t disease_idx = 0xFF, int32_t disease_count = 0,
                    int32_t callback = -1) {
  ModifyCellMessage m{};
  m.cellIdx = game_cell;
  m.callbackIdx = callback;
  m.temperature = temperature;
  m.mass = mass;
  m.diseaseCount = disease_count;
  m.elementIdx = static_cast<uint16_t>(t.IndexOf(hash));
  m.replaceType = replace_type;
  m.diseaseIdx = disease_idx;
  m.addSubType = add_sub_type;
  s->handle_message(static_cast<int32_t>(SimMessageHash::ModifyCell), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

// `SetInsulationValue` / `SetStrengthValue`. Both carry a raw float and neither is
// clamped on the way in, which is the whole point of sending values outside [0, 1].
void SendCellFloat(Backend* s, SimMessageHash hash, int32_t game_cell, float value) {
  SetCellFloatValueMessage m{};
  m.cellIdx = game_cell;
  m.value = value;
  s->handle_message(static_cast<int32_t>(hash), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

void SendChunkAdjuster(Backend* s, int32_t handle, float temperature, float heat_capacity,
                       float conductivity) {
  ModifyElementChunkAdjusterMessage m{};
  m.handle = handle;
  m.temperature = temperature;
  m.heatCapacity = heat_capacity;
  m.thermalConductivity = conductivity;
  s->handle_message(static_cast<int32_t>(SimMessageHash::ModifyChunkTemperatureAdjuster),
                    sizeof(m), reinterpret_cast<const uint8_t*>(&m));
}

void SendChunkRemove(Backend* s, int32_t handle, int32_t callback_idx) {
  RemoveElementChunkMessage m{};
  m.handle = handle;
  m.callbackIdx = callback_idx;
  s->handle_message(static_cast<int32_t>(SimMessageHash::RemoveElementChunk), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

// Three hot chunks in cold rock, differing in the two scale factors so that neither is
// one-valued: the transfer-area ratio and the ground scale each appear as 1, as something
// smaller and as something larger. Every cell here is solid, so all three exchange with
// their own cell *and* with the tile below.
void ChunkHot(Backend* s, const Tables& t, const World& w) {
  SendChunk(s, t, kCopper, 100.0f, 400.0f, 1.0f, 1.0f, 1.0f, w.Cell(7, 7));
  SendChunk(s, t, kCopper, 100.0f, 400.0f, 4.0f, 0.5f, 0.25f, w.Cell(11, 7));
  SendChunk(s, t, kCopper, 100.0f, 400.0f, 0.25f, 2.0f, 3.0f, w.Cell(15, 7));
}

// The vacuum box, which separates the two halves of the exchange. `(8,8)` floats in vacuum
// with vacuum underneath and must not move at all; `(7,8)` floats in vacuum but sits on the
// one granite tile, so the *only* transfer it sees is the ground one, at its own scale;
// `(7,7)` is inside that tile, so it gets the cell transfer and a ground transfer into the
// granite below the box.
void ChunkVacuum(Backend* s, const Tables& t, const World& w) {
  SendChunk(s, t, kCopper, 100.0f, 500.0f, 1.0f, 1.0f, 1.0f, w.Cell(8, 8));
  SendChunk(s, t, kCopper, 100.0f, 500.0f, 1.0f, 1.0f, 0.5f, w.Cell(7, 8));
  SendChunk(s, t, kCopper, 100.0f, 500.0f, 1.0f, 1.0f, 1.0f, w.Cell(7, 7));
}

// The adjuster: a fictitious body the chunk exchanges against *instead of* the world. Chunk
// 0 gets one and must ignore the rock it is standing in entirely; chunk 1 is the control in
// the same cell type with no adjuster. The tick hook switches chunk 0's adjuster back off
// part way through, which is the transition the `heat_capacity > 0` gate decides and the
// only way to tell "adjuster wins" from "adjuster is all there is".
void ChunkAdjuster(Backend* s, const Tables& t, const World& w) {
  SendChunk(s, t, kCopper, 100.0f, 300.0f, 1.0f, 1.0f, 1.0f, w.Cell(7, 7));
  SendChunk(s, t, kCopper, 100.0f, 300.0f, 1.0f, 1.0f, 1.0f, w.Cell(11, 7));
  SendChunkAdjuster(s, 0, 800.0f, 50.0f, 2.0f);
}

void ChunkAdjusterTick(Backend* s, const Tables&, const World&, int tick) {
  // Off again. Zero heat capacity is how the game retires an adjuster, and from here chunk 0
  // has to start trading with the rock instead.
  if (tick == 12) SendChunkAdjuster(s, 0, 0.0f, 0.0f, 0.0f);
}

// The melt announcement, both directions. Water transitions at 273.15 and 372.65, and the
// test is three kelvin outside that, so 500 K is over and 100 K is under. The masses are
// large enough that the rock cannot pull either chunk back across its threshold inside the
// run — this scenario is about the announcement, not about the exchange, and a chunk that
// drifted back in range mid-run would test the edge instead of the state.
//
// Chunk 2 is the control: in range, and must never appear in the list.
void ChunkMelt(Backend* s, const Tables& t, const World& w) {
  SendChunk(s, t, kWater, 100000.0f, 500.0f, 1.0f, 1.0f, 1.0f, w.Cell(7, 7));
  SendChunk(s, t, kWater, 100000.0f, 100.0f, 1.0f, 1.0f, 1.0f, w.Cell(11, 7));
  SendChunk(s, t, kWater, 100000.0f, 300.0f, 1.0f, 1.0f, 1.0f, w.Cell(15, 7));
}

// The element consumer and the element emitter. Neither component had ever been registered
// by anything in this suite — `experiments.cpp` drives them against Klei alone, with no
// comparison — so every one of these five senders is new coverage.
void SendAddConsumer(Backend* s, const Tables& t, int game_cell, int32_t hash, uint8_t radius,
                     uint8_t configuration, int32_t callback = -1) {
  AddElementConsumerMessage m{};
  m.cellIdx = game_cell;
  m.callbackIdx = callback;
  m.radius = radius;
  m.configuration = configuration;
  m.elementIdx = static_cast<uint16_t>(t.IndexOf(hash));
  s->handle_message(static_cast<int32_t>(SimMessageHash::AddElementConsumer), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

void SendConsumerData(Backend* s, int32_t handle, int game_cell, float rate) {
  SetElementConsumerDataMessage m{};
  m.handle = handle;
  m.cell = game_cell;
  m.consumptionRate = rate;
  s->handle_message(static_cast<int32_t>(SimMessageHash::SetElementConsumerData), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

void SendRemoveConsumer(Backend* s, int32_t handle, int32_t callback) {
  RemoveElementConsumerMessage m{};
  m.handle = handle;
  m.callbackIdx = callback;
  s->handle_message(static_cast<int32_t>(SimMessageHash::RemoveElementConsumer), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

void SendAddEmitter(Backend* s, float max_pressure, int32_t callback, int32_t on_blocked,
                    int32_t on_unblocked) {
  AddElementEmitterMessage m{};
  m.maxPressure = max_pressure;
  m.callbackIdx = callback;
  m.onBlockedCB = on_blocked;
  m.onUnblockedCB = on_unblocked;
  s->handle_message(static_cast<int32_t>(SimMessageHash::AddElementEmitter), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

void SendModifyEmitter(Backend* s, const Tables& t, int32_t handle, int game_cell,
                       int32_t hash, float interval, float mass, float temperature,
                       float max_pressure, uint8_t max_depth = 1, uint8_t disease_idx = 0xFF,
                       int32_t disease_count = 0) {
  ModifyElementEmitterMessage m{};
  m.handle = handle;
  m.cellIdx = game_cell;
  m.emitInterval = interval;
  m.emitMass = mass;
  m.emitTemperature = temperature;
  m.maxPressure = max_pressure;
  m.diseaseCount = disease_count;
  m.elementIdx = static_cast<uint16_t>(t.IndexOf(hash));
  m.maxDepth = max_depth;
  m.diseaseIdx = disease_idx;
  s->handle_message(static_cast<int32_t>(SimMessageHash::ModifyElementEmitter), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

// The radiation emitter's three messages, plus the two the sim's own radiation array
// answers to. None of the five had ever been sent by a scenario.
void SendAddRadiationEmitter(Backend* s, int game_cell, int16_t rx, int16_t ry, float rads,
                             float rate, float speed, float direction, float angle,
                             int32_t type, int32_t callback) {
  AddRadiationEmitterMessage m{};
  m.callbackIdx = callback;
  m.cell = game_cell;
  m.emitRadiusX = rx;
  m.emitRadiusY = ry;
  m.emitRads = rads;
  m.emitRate = rate;
  m.emitSpeed = speed;
  m.emitDirection = direction;
  m.emitAngle = angle;
  m.emitType = type;
  s->handle_message(static_cast<int32_t>(SimMessageHash::AddRadiationEmitter), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

void SendModifyRadiationEmitter(Backend* s, int32_t handle, int game_cell, int16_t rx,
                                int16_t ry, float rads, float rate, float speed,
                                float direction, float angle, int32_t type,
                                int32_t callback = -1) {
  ModifyRadiationEmitterMessage m{};
  m.handle = handle;
  m.cell = game_cell;
  m.callbackIdx = callback;
  m.emitRadiusX = rx;
  m.emitRadiusY = ry;
  m.emitRads = rads;
  m.emitRate = rate;
  m.emitSpeed = speed;
  m.emitDirection = direction;
  m.emitAngle = angle;
  m.emitType = type;
  s->handle_message(static_cast<int32_t>(SimMessageHash::ModifyRadiationEmitter), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

void SendRemoveRadiationEmitter(Backend* s, int32_t handle, int32_t callback) {
  RemoveRadiationEmitterMessage m{};
  m.handle = handle;
  m.callbackIdx = callback;
  s->handle_message(static_cast<int32_t>(SimMessageHash::RemoveRadiationEmitter), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

void SendCellRadiation(Backend* s, int game_cell, float delta, int32_t callback = -1) {
  CellRadiationModification m{};
  m.cellIdx = game_cell;
  m.radiationDelta = delta;
  m.callbackIdx = callback;
  s->handle_message(static_cast<int32_t>(SimMessageHash::CellRadiationModification), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

void SendRadiationParams(Backend* s, int32_t type, float value) {
  RadiationParamsModification m{};
  m.RadiationParamsType = type;
  m.value = value;
  s->handle_message(static_cast<int32_t>(SimMessageHash::RadiationParamsModification),
                    sizeof(m), reinterpret_cast<const uint8_t*>(&m));
}

// `MassConsumption`, which the suite has sent before but never with a radius or a height —
// every previous send took the message's default zero for both, which is the flood with
// `maxDepth = 0`, i.e. no cells at all.
void SendMassConsumption(Backend* s, const Tables& t, int game_cell, int32_t hash, float mass,
                         uint8_t radius, uint8_t height, int32_t callback = -1) {
  MassConsumptionMessage m{};
  m.cellIdx = game_cell;
  m.callbackIdx = callback;
  m.mass = mass;
  m.elementIdx = static_cast<uint16_t>(t.IndexOf(hash));
  m.radius = radius;
  m.height = height;
  s->handle_message(static_cast<int32_t>(SimMessageHash::MassConsumption), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

void SendMassEmission(Backend* s, const Tables& t, int game_cell, int32_t hash, float mass,
                      float temperature, int32_t callback = -1, uint8_t disease_idx = 0xFF,
                      int32_t disease_count = 0) {
  MassEmissionMessage m{};
  m.cellIdx = game_cell;
  m.callbackIdx = callback;
  m.mass = mass;
  m.temperature = temperature;
  m.diseaseCount = disease_count;
  m.elementIdx = static_cast<uint16_t>(t.IndexOf(hash));
  m.diseaseIdx = disease_idx;
  s->handle_message(static_cast<int32_t>(SimMessageHash::MassEmission), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

// `ModifyElementChunkEnergy`. Klei floors the result at zero and does **not** ceiling it,
// which is the opposite of the building version — so the interesting case is the one that
// tries to take out more energy than the chunk has. The chunk lands at exactly 0 K, which is
// below water's low transition and therefore starts announcing, and 0 is not negative so it
// keeps exchanging with the rock afterwards rather than being refused.
void ChunkEnergy(Backend* s, const Tables& t, const World& w) {
  SendChunk(s, t, kWater, 10.0f, 300.0f, 1.0f, 1.0f, 1.0f, w.Cell(7, 7));
  SendChunk(s, t, kWater, 10.0f, 300.0f, 1.0f, 1.0f, 1.0f, w.Cell(11, 7));
}

void ChunkEnergyTick(Backend* s, const Tables&, const World&, int tick) {
  if (tick == 3) SendChunkEnergy(s, 0, 5000.0f);
  if (tick == 6) SendChunkEnergy(s, 0, -1.0e9f);   // floors at 0 K
  if (tick == 9) SendChunkEnergy(s, 1, -100.0f);   // an ordinary withdrawal
}

// The allocator, the slot table and the two message arms nothing else reaches. Four chunks
// go in with real callback indices, so `componentStateChangedMessages` carries something for
// the first time; then one is moved, one is rewritten, one is removed, and a fifth is
// registered into the freed slot. That last step is the point: the handle it gets back has
// to be slot 0 with version 1, on both sides, and the info array has to still be indexed by
// slot rather than by position.
void ChunkChurn(Backend* s, const Tables& t, const World& w) {
  SendChunk(s, t, kCopper, 100.0f, 400.0f, 1.0f, 1.0f, 1.0f, w.Cell(7, 7), 10);
  SendChunk(s, t, kCopper, 100.0f, 350.0f, 1.0f, 1.0f, 1.0f, w.Cell(9, 7), 11);
  SendChunk(s, t, kCopper, 100.0f, 250.0f, 1.0f, 1.0f, 1.0f, w.Cell(11, 7), 12);
  SendChunk(s, t, kCopper, 100.0f, 200.0f, 1.0f, 1.0f, 1.0f, w.Cell(13, 7), 13);
}

void ChunkChurnTick(Backend* s, const Tables& t, const World& w, int tick) {
  // Handles are slot | (version << 24), and the first four registrations are slots 0..3 at
  // version 0, so the handle *is* the slot until something is freed.
  if (tick == 4) SendChunkMove(s, 0, w.Cell(7, 11));
  if (tick == 6) SendChunkData(s, 1, 450.0f, 900.0f);
  if (tick == 8) SendChunkRemove(s, 0, 20);
  // Slot 0 again, now at version 1. Registering into a freed slot is also what puts the
  // compacted vector's swap-with-last through its paces, because removing slot 0 moved the
  // last live chunk into position 0 of the dense array and update order changed with it.
  if (tick == 10) SendChunk(s, t, kCopper, 100.0f, 600.0f, 1.0f, 1.0f, 1.0f, w.Cell(7, 7), 21);
  if (tick == 12) SendChunkRemove(s, 2, 22);
}

// The conductivity branch. `ExchangeHeatEnergyWithWorld` takes the **smaller** of the
// chunk's conductivity and the cell's insulation-attenuated one, so a scenario needs both
// sides to win somewhere or half the branch is dead:
//
//   * copper chunk (60) in granite (3.39): the cell wins, and with the insulation byte
//     turned down it wins by a wider margin still;
//   * water chunk (0.609) in copper (60): the chunk wins.
//
// The insulation byte itself is 255 in most scenarios and is the second
// thing this world varies.
World ChunkInsulation(const Tables& t) {
  World w;
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t copper = static_cast<uint16_t>(t.IndexOf(kCopper));
  w.Init(24, 16, granite, 2000.0f, 293.15f);
  // A copper column for the chunk whose own conductivity is the smaller of the two.
  for (int y = 5; y < 9; ++y) w.Set(15, y, copper, 2000.0f, 293.15f);
  // Insulated granite under and around the first chunk.
  for (int y = 6; y < 9; ++y) {
    for (int x = 6; x < 9; ++x) w.insulation[w.Cell(x, y)] = 32;
  }
  return w;
}

void ChunkInsulated(Backend* s, const Tables& t, const World& w) {
  SendChunk(s, t, kCopper, 100.0f, 400.0f, 1.0f, 1.0f, 1.0f, w.Cell(7, 7));   // insulated rock
  SendChunk(s, t, kCopper, 100.0f, 400.0f, 1.0f, 1.0f, 1.0f, w.Cell(11, 7));  // bare rock
  SendChunk(s, t, kWater, 100.0f, 400.0f, 1.0f, 1.0f, 1.0f, w.Cell(15, 7));   // chunk wins
}

// The same box with no tile in it at all, so a building standing in it touches nothing and
// the only heat that moves is between buildings.
World BuildingVoid(const Tables& t) {
  World w;
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  w.Init(16, 16, granite, 1000.0f, 300.0f);
  for (int y = 6; y < 10; ++y) {
    for (int x = 6; x < 12; ++x) w.Set(x, y, vacuum, 0.0f, 0.0f);
  }
  return w;
}

// Klei shuffles adjacent gas cells at random (see the tracer scenarios), which perturbs
// every liquid scenario that has an atmosphere in it. Filling the room with vacuum instead
// removes the mechanism entirely and leaves the liquid kernel on its own.
bool g_fill_vacuum = false;

// Granite everywhere, a building's worth of it replaced by vacuum except one tile.
World BuildingBox(const Tables& t) {
  World w;
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  w.Init(16, 16, granite, 1000.0f, 300.0f);
  for (int y = 6; y < 10; ++y) {
    for (int x = 6; x < 12; ++x) w.Set(x, y, vacuum, 0.0f, 0.0f);
  }
  w.Set(7, 7, granite, 1000.0f, 300.0f);
  return w;
}

// Water over air in a sealed box. Klei settles it; the replacement cannot. This is
// expected to diverge, and the harness exists to say how fast and in what.
World Falling(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(g_fill_vacuum ? kVacuum : kOxygen));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  World w;
  w.Init(24, 16, oxygen, g_fill_vacuum ? 0.0f : 1.0f, 293.15f);
  for (int x = 0; x < w.width; ++x) {
    w.Set(x, 0, granite, 2000.0f, 293.15f);
    w.Set(x, w.height - 1, granite, 2000.0f, 293.15f);
  }
  for (int y = 0; y < w.height; ++y) {
    w.Set(0, y, granite, 2000.0f, 293.15f);
    w.Set(w.width - 1, y, granite, 2000.0f, 293.15f);
  }
  for (int x = 6; x < 18; ++x) {
    for (int y = 11; y < 14; ++y) w.Set(x, y, water, 900.0f, 350.0f);
  }
  return w;
}

// ------------------------------------------------------- minimal liquid scenarios
//
// `liquid` and `falling` both diverge, and both contain every liquid behaviour at once —
// falling, pouring, settling, gas displacement — so neither says *which* one is wrong. The
// four below each isolate one, on the smallest world that still exercises it, so the first
// one that diverges names the defect.
//
// They share a granite shell built by this helper; a scenario whose fluid can reach the
// world edge is measuring the border ring as much as the kernel.
World LiquidRoom(const Tables& t, int width, int height, uint16_t fill, float mass,
                 float temperature) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  World w;
  w.Init(width, height, fill, mass, temperature);
  for (int x = 0; x < w.width; ++x) {
    w.Set(x, 0, granite, 2000.0f, 293.15f);
    w.Set(x, w.height - 1, granite, 2000.0f, 293.15f);
  }
  for (int y = 0; y < w.height; ++y) {
    w.Set(0, y, granite, 2000.0f, 293.15f);
    w.Set(w.width - 1, y, granite, 2000.0f, 293.15f);
  }
  return w;
}

// The same room with no water in it at all. `drop` diverges in two gas cells against the
// wall, nowhere near the water, which is a claim about the liquid kernel only if the room
// itself is quiet. This is the control.
int g_room_w = 16, g_room_h = 16;
int32_t g_tracer_gas = kOxygen;
// `--draws`: print how far our sim advanced the random stream on every tick. The LCG is
// invertible only by walking it, so this steps the game's constant from the previous state
// until it matches; anything past the cap means the stream was reseeded rather than merely
// advanced. Only our DLL exports its random state, so only our side is printed.
bool g_show_draws = false;
// `--messages`: print both sides' cell-event lists in full, every tick, in the
// order the sims emitted them. The count-only line in `CompareUpdates` fires once, on the
// first diverging tick, which is the right thing for a physics diff and useless here: the
// list is a record of which cells a kernel *touched*, so its order is the sweep order and
// two lists of the same length can still be wrong.
bool g_show_messages = false;

void DumpSubstance(const GameDataUpdate* a, const GameDataUpdate* b, int tick) {
  if (!a->numSubstanceChangeInfo && !b->numSubstanceChangeInfo) return;
  printf("  tick %-4d substance  klei %d, mine %d\n", tick, a->numSubstanceChangeInfo,
         b->numSubstanceChangeInfo);
  const int n = a->numSubstanceChangeInfo > b->numSubstanceChangeInfo
                    ? a->numSubstanceChangeInfo
                    : b->numSubstanceChangeInfo;
  for (int i = 0; i < n; ++i) {
    char ka[48] = "      -", mb[48] = "      -";
    if (i < a->numSubstanceChangeInfo) {
      const SubstanceChangeInfo& s = a->substanceChangeInfo[i];
      snprintf(ka, sizeof ka, "%5d (%2d,%2d) %3u->%-3u", s.cellIdx,
               g_grid_width ? s.cellIdx % g_grid_width : 0,
               g_grid_width ? s.cellIdx / g_grid_width : 0, s.oldElemIdx, s.newElemIdx);
    }
    if (i < b->numSubstanceChangeInfo) {
      const SubstanceChangeInfo& s = b->substanceChangeInfo[i];
      snprintf(mb, sizeof mb, "%5d (%2d,%2d) %3u->%-3u", s.cellIdx,
               g_grid_width ? s.cellIdx % g_grid_width : 0,
               g_grid_width ? s.cellIdx / g_grid_width : 0, s.oldElemIdx, s.newElemIdx);
    }
    const bool same = i < a->numSubstanceChangeInfo && i < b->numSubstanceChangeInfo &&
                      !memcmp(&a->substanceChangeInfo[i], &b->substanceChangeInfo[i],
                              sizeof(SubstanceChangeInfo));
    printf("      [%2d] klei %-26s mine %-26s %s\n", i, ka, mb, same ? "" : "<--");
  }
}

// The other half of the unstable path, and the half `--gameside` is for. Every field is
// compared, not just the cell: `mass` and `temperature` are read off the tile *before* it
// is cleared, so a kernel that cleared first and reported second would still get the cell
// index right and every number wrong.
void DumpUnstable(const GameDataUpdate* a, const GameDataUpdate* b, int tick) {
  if (!a->numUnstableCellInfo && !b->numUnstableCellInfo) return;
  printf("  tick %-4d unstable   klei %d, mine %d\n", tick, a->numUnstableCellInfo,
         b->numUnstableCellInfo);
  const int n = a->numUnstableCellInfo > b->numUnstableCellInfo ? a->numUnstableCellInfo
                                                                : b->numUnstableCellInfo;
  for (int i = 0; i < n; ++i) {
    char ka[80] = "      -", mb[80] = "      -";
    if (i < a->numUnstableCellInfo) {
      const UnstableCellInfo& u = a->unstableCellInfo[i];
      snprintf(ka, sizeof ka, "%5d (%2d,%2d) e%-3u f%u d%3u %9.3f kg %9.4f K", u.cellIdx,
               g_grid_width ? u.cellIdx % g_grid_width : 0,
               g_grid_width ? u.cellIdx / g_grid_width : 0, u.elemIdx, u.fallingInfo,
               u.diseaseIdx, u.mass, u.temperature);
    }
    if (i < b->numUnstableCellInfo) {
      const UnstableCellInfo& u = b->unstableCellInfo[i];
      snprintf(mb, sizeof mb, "%5d (%2d,%2d) e%-3u f%u d%3u %9.3f kg %9.4f K", u.cellIdx,
               g_grid_width ? u.cellIdx % g_grid_width : 0,
               g_grid_width ? u.cellIdx / g_grid_width : 0, u.elemIdx, u.fallingInfo,
               u.diseaseIdx, u.mass, u.temperature);
    }
    const bool same = i < a->numUnstableCellInfo && i < b->numUnstableCellInfo &&
                      !memcmp(&a->unstableCellInfo[i], &b->unstableCellInfo[i],
                              sizeof(UnstableCellInfo));
    printf("      [%2d] klei %-52s mine %-52s %s\n", i, ka, mb, same ? "" : "<--");
  }
}

// The falling-liquid records side by side, for `--messages`. Same shape as `DumpUnstable`.
void DumpFallingLiquid(const GameDataUpdate* a, const GameDataUpdate* b, int tick) {
  if (!a->numSpawnFallingLiquidInfo && !b->numSpawnFallingLiquidInfo) return;
  printf("  tick %-4d falling    klei %d, mine %d\n", tick, a->numSpawnFallingLiquidInfo,
         b->numSpawnFallingLiquidInfo);
  const int n = a->numSpawnFallingLiquidInfo > b->numSpawnFallingLiquidInfo
                    ? a->numSpawnFallingLiquidInfo : b->numSpawnFallingLiquidInfo;
  const int lim = n < 24 ? n : 24;
  for (int i = 0; i < lim; ++i) {
    char ka[80] = "      -", mb[80] = "      -";
    if (i < a->numSpawnFallingLiquidInfo) {
      const SpawnFallingLiquidInfo& u = a->spawnFallingLiquidInfo[i];
      snprintf(ka, sizeof ka, "%5d (%2d,%2d) e%-3u d%3u %9.4f kg %9.4f K %d", u.cellIdx,
               g_grid_width ? u.cellIdx % g_grid_width : 0,
               g_grid_width ? u.cellIdx / g_grid_width : 0, u.elemIdx, u.diseaseIdx, u.mass,
               u.temperature, u.diseaseCount);
    }
    if (i < b->numSpawnFallingLiquidInfo) {
      const SpawnFallingLiquidInfo& u = b->spawnFallingLiquidInfo[i];
      snprintf(mb, sizeof mb, "%5d (%2d,%2d) e%-3u d%3u %9.4f kg %9.4f K %d", u.cellIdx,
               g_grid_width ? u.cellIdx % g_grid_width : 0,
               g_grid_width ? u.cellIdx / g_grid_width : 0, u.elemIdx, u.diseaseIdx, u.mass,
               u.temperature, u.diseaseCount);
    }
    const bool same = i < a->numSpawnFallingLiquidInfo && i < b->numSpawnFallingLiquidInfo &&
                      !memcmp(&a->spawnFallingLiquidInfo[i], &b->spawnFallingLiquidInfo[i],
                              sizeof(SpawnFallingLiquidInfo));
    printf("      [%2d] klei %-58s mine %-58s %s\n", i, ka, mb, same ? "" : "<--");
  }
}

// The world-damage records side by side, for `--messages`.
void DumpWorldDamage(const GameDataUpdate* a, const GameDataUpdate* b, int tick) {
  if (!a->numWorldDamageInfo && !b->numWorldDamageInfo) return;
  printf("  tick %-4d damage     klei %d, mine %d\n", tick, a->numWorldDamageInfo,
         b->numWorldDamageInfo);
  const int n = a->numWorldDamageInfo > b->numWorldDamageInfo ? a->numWorldDamageInfo
                                                              : b->numWorldDamageInfo;
  const int lim = n < 24 ? n : 24;
  for (int i = 0; i < lim; ++i) {
    char ka[48] = "      -", mb[48] = "      -";
    if (i < a->numWorldDamageInfo) {
      const WorldDamageInfo& u = a->worldDamageInfo[i];
      snprintf(ka, sizeof ka, "%5d (%2d,%2d) from %5d", u.gameCell,
               g_grid_width ? u.gameCell % g_grid_width : 0,
               g_grid_width ? u.gameCell / g_grid_width : 0, u.damageSourceOffset);
    }
    if (i < b->numWorldDamageInfo) {
      const WorldDamageInfo& u = b->worldDamageInfo[i];
      snprintf(mb, sizeof mb, "%5d (%2d,%2d) from %5d", u.gameCell,
               g_grid_width ? u.gameCell % g_grid_width : 0,
               g_grid_width ? u.gameCell / g_grid_width : 0, u.damageSourceOffset);
    }
    const bool same = i < a->numWorldDamageInfo && i < b->numWorldDamageInfo &&
                      !memcmp(&a->worldDamageInfo[i], &b->worldDamageInfo[i],
                              sizeof(WorldDamageInfo));
    printf("      [%2d] klei %-32s mine %-32s %s\n", i, ka, mb, same ? "" : "<--");
  }
}

// The melted-tile records side by side, for `--messages`.
void DumpCellMelted(const GameDataUpdate* a, const GameDataUpdate* b, int tick) {
  if (!a->numCellMeltedInfos && !b->numCellMeltedInfos) return;
  printf("  tick %-4d melted     klei %d, mine %d\n", tick, a->numCellMeltedInfos,
         b->numCellMeltedInfos);
  const int n = a->numCellMeltedInfos > b->numCellMeltedInfos ? a->numCellMeltedInfos
                                                              : b->numCellMeltedInfos;
  const int lim = n < 24 ? n : 24;
  for (int i = 0; i < lim; ++i) {
    char ka[32] = "      -", mb[32] = "      -";
    if (i < a->numCellMeltedInfos) {
      const int c = a->cellMeltedInfos[i].gameCell;
      snprintf(ka, sizeof ka, "%5d (%2d,%2d)", c, g_grid_width ? c % g_grid_width : 0,
               g_grid_width ? c / g_grid_width : 0);
    }
    if (i < b->numCellMeltedInfos) {
      const int c = b->cellMeltedInfos[i].gameCell;
      snprintf(mb, sizeof mb, "%5d (%2d,%2d)", c, g_grid_width ? c % g_grid_width : 0,
               g_grid_width ? c / g_grid_width : 0);
    }
    const bool same = i < a->numCellMeltedInfos && i < b->numCellMeltedInfos &&
                      a->cellMeltedInfos[i].gameCell == b->cellMeltedInfos[i].gameCell;
    printf("      [%2d] klei %-20s mine %-20s %s\n", i, ka, mb, same ? "" : "<--");
  }
}

int StepsBetween(uint32_t from, uint32_t to) {
  if (from == to) return 0;
  uint32_t s = from;
  for (int n = 1; n <= 4000000; ++n) {
    s = s * 214013u + 2531011u;
    if (s == to) return n;
  }
  return -1;
}
float g_tracer_mass = 1.0f;

// A room in which, by construction, **nothing may happen**.
//
// Every cell holds the same gas at the same mass, so `dm = flow * (m_src - m_dst)` is zero
// for every pair and no mass can move. Every neighbour pair differs in temperature by well
// under the 1 K conduction dead zone, including the granite shell, so no pair may conduct
// either. The temperatures are all distinct, which makes each cell a tracer: if a value
// turns up anywhere other than where it started, something moved it, and the pattern says
// what.
//
// Built because `gasroom` showed Klei cooling a cell two away from the wall, which
// conduction cannot do at any strength — so either mass or temperature is being carried
// sideways by something we do not model.
World Tracer(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(g_tracer_gas));
  World w;
  w.Init(g_room_w, g_room_h, oxygen, g_tracer_mass, 300.0f);
  for (int y = 0; y < w.height; ++y) {
    for (int x = 0; x < w.width; ++x) {
      const bool edge = x == 0 || y == 0 || x == w.width - 1 || y == w.height - 1;
      // 0.001 K apart: distinct to the float, and two orders of magnitude under the dead
      // zone even across the diagonal of the whole room.
      const float temp = 300.0f + 0.001f * (y * w.width + x);
      if (edge) {
        w.Set(x, y, granite, 2000.0f, temp);
      } else {
        w.Set(x, y, oxygen, g_tracer_mass, temp);
      }
    }
  }
  return w;
}

World GasRoom(const Tables& t) {
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  return LiquidRoom(t, g_room_w, g_room_h, oxygen, 1.0f, 300.0f);
}

// The still room with a granite slab across the middle, splitting it into two stacked gas
// pockets. The shuffle skips the bottom two rows of the room; this asks whether "bottom"
// is measured from the world or from the pocket the gas is in.
World TracerSlab(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  World w;
  w.Init(g_room_w, g_room_h, oxygen, 1.0f, 300.0f);
  const int slab = g_room_h / 2;
  for (int y = 0; y < w.height; ++y) {
    for (int x = 0; x < w.width; ++x) {
      const bool edge = x == 0 || y == 0 || x == w.width - 1 || y == w.height - 1;
      const float temp = 300.0f + 0.001f * (y * w.width + x);
      if (edge || y == slab) w.Set(x, y, granite, 2000.0f, temp);
      else w.Set(x, y, oxygen, 1.0f, temp);
    }
  }
  return w;
}

// The same still room, but half the gas is a second element at the identical mass. If the
// shuffle moves elements as well as temperatures, the two gases interpenetrate here and the
// element field alone proves it — with equal masses there is no pressure to drive flow, and
// two different elements are the one case our kernel says can never exchange at all.
World TracerMix(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t co2 = static_cast<uint16_t>(t.IndexOf(1960575215));  // CarbonDioxide
  World w;
  w.Init(g_room_w, g_room_h, oxygen, 1.0f, 300.0f);
  for (int y = 0; y < w.height; ++y) {
    for (int x = 0; x < w.width; ++x) {
      const bool edge = x == 0 || y == 0 || x == w.width - 1 || y == w.height - 1;
      const float temp = 300.0f + 0.001f * (y * w.width + x);
      if (edge) w.Set(x, y, granite, 2000.0f, temp);
      else w.Set(x, y, x < w.width / 2 ? oxygen : co2, 1.0f, temp);
    }
  }
  return w;
}

// -------------------------------------------------------------------- the layout probe
//
// A hand-spelled world inside a granite shell: `--layout` is the interior, rows separated
// by `/` and written **bottom row first**, which is the order the sim sweeps them in. It
// exists for `--draws` and nothing else.
//
// The post-process sweep is `for y ascending: for x ascending`, so the layout string is
// read in exactly the order the cells are visited. That is what makes it an instrument:
// our draw count is necessarily right for every cell before the first one we get wrong,
// and meaningless for every cell after it, so extending the layout one character at a time
// prices each cell on its own. `--layout G` against `--layout GW` is the whole cost of a
// water cell sitting on gas, with nothing after it to contaminate the number.
//
// Letters: G oxygen at `--columnmass` (default 2 kg, over `DoSublimation`'s 1.8 kg gate so
// a gas cell never draws for its solid neighbours), g a **wisp** — 0.0005 kg, under
// `PostProcessCell`'s 0.001 kg evaporation gate — W water at 1000 kg, V vacuum, S granite.
// A **digit** `1`..`9` is oxygen at that many tenths of a kilogram, which is how a stack of
// *unequal* gas masses gets spelled: `G` alone makes every gas cell the same weight, and a
// column of equal masses over a vacuum cell resolves identically in both sims, so the digits
// are the difference between a probe that reproduces the open defect and one that does not.
// The shell is always granite. Rows may be ragged; short rows are padded with granite.
//
// The wisp is carbon dioxide rather than oxygen, and that is the point of it: two different
// elements never exchange mass in the flow kernel, so a CO2 wisp among oxygen is the only
// way to hold a cell under 0.001 kg long enough for the post-process sweep to see it. Made
// of oxygen it is refilled by its neighbours before the sweep runs and the path is
// unreachable.
//
// Two cautions, both learned the hard way here. Klei's boot tick sometimes runs the first
// physics frame and sometimes does not, so **read a steady tick, not tick 1** — build a
// layout in which nothing can move and every frame costs the same. And a gas cell only
// stays put if it has nowhere to expand into, so vacuum in the layout makes it transient.
const char* g_layout = "GG";
float g_column_gas_mass = 2.0f;
float g_column_water_mass = 1000.0f;
bool g_column_flat = false;
float g_column_water_temp = 350.0f;

World Column(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  const uint16_t co2 = static_cast<uint16_t>(t.IndexOf(1960575215));  // CarbonDioxide

  std::vector<std::string> rows;
  {
    const char* p = g_layout;
    for (const char* q = p;; ++q) {
      if (*q == '/' || *q == '\0') {
        rows.emplace_back(p, static_cast<size_t>(q - p));
        if (*q == '\0') break;
        p = q + 1;
      }
    }
  }
  size_t widest = 1;
  for (const std::string& r : rows) widest = r.size() > widest ? r.size() : widest;

  World w;
  w.Init(static_cast<int>(widest) + 2, static_cast<int>(rows.size()) + 2, granite, 2000.0f,
         293.15f);
  for (size_t y = 0; y < rows.size(); ++y) {
    for (size_t x = 0; x < rows[y].size(); ++x) {
      // 0.001 K apart so no pair can conduct and every cell stays its own tracer. With
      // `--columnflat` the grading is dropped for `falling`'s two flat temperatures
      // instead, because a graded field is not the same world: it is what lets conduction
      // run, and a probe meant to reproduce a *flat* scenario has to be flat too.
      const float temp = g_column_flat
                             ? 293.15f
                             : 300.0f + 0.001f * static_cast<float>(y * widest + x);
      const float wtemp = g_column_flat ? g_column_water_temp : temp;
      const int cx = static_cast<int>(x) + 1, cy = static_cast<int>(y) + 1;
      switch (rows[y][x]) {
        case 'G': w.Set(cx, cy, oxygen, g_column_gas_mass, temp); break;
        case 'g': w.Set(cx, cy, co2, 0.0005f, temp); break;
        case 'W': w.Set(cx, cy, water, g_column_water_mass, wtemp); break;
        case 'V': w.Set(cx, cy, vacuum, 0.0f, 0.0f); break;
        default:
          if (rows[y][x] >= '1' && rows[y][x] <= '9') {
            w.Set(cx, cy, oxygen, 0.1f * static_cast<float>(rows[y][x] - '0'), temp);
          }
          break;  // 'S' and anything else: leave the granite Init put there
      }
    }
  }
  return w;
}

// One cell of water in a room of gas. The whole-cell swap and nothing else: no neighbour
// to pour into, no second column to interleave with, one cell of gas displaced per substep.
World LiquidDrop(const Tables& t) {
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(g_fill_vacuum ? kVacuum : kOxygen));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  World w = LiquidRoom(t, 16, 16, oxygen, g_fill_vacuum ? 0.0f : 1.0f, 300.0f);
  w.Set(8, 12, water, 1000.0f, 310.0f);
  return w;
}

// Two adjacent columns of water falling side by side. Same mechanism as `drop`, plus the
// one thing `drop` cannot see: the `++x` stride that a successful swap applies to the next
// column. If our stride is wrong this diverges and `drop` does not.
World LiquidPair(const Tables& t) {
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(g_fill_vacuum ? kVacuum : kOxygen));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  World w = LiquidRoom(t, 16, 16, oxygen, g_fill_vacuum ? 0.0f : 1.0f, 300.0f);
  for (int x = 6; x < 10; ++x) w.Set(x, 12, water, 1000.0f, 310.0f);
  return w;
}

// One water cell that can only leave by falling, for `SpawnFallingLiquid`. It hangs over
// an open gas column, its left neighbour is open and the cell under that is granite, so
// the first permeability probe sends it down the spawn branch on every substep: it stands
// still until the sim is allowed to hand it to the game, and then it is gone. Sent
// non-headless (`World::gameside`), because a headless sim never hands anything over --
// which is why floating water could get past every other scenario here. The tick it leaves
// on is the visibility grid's start-up lag; the record it leaves is the whole payload.
// A second cell further along stands on granite and spreads sideways over the edge of it,
// which is the sideways spawn branch.
World LiquidHang(const Tables& t) {
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(g_fill_vacuum ? kVacuum : kOxygen));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  World w = LiquidRoom(t, 16, 12, oxygen, g_fill_vacuum ? 0.0f : 1.0f, 300.0f);
  w.gameside = true;
  w.Set(3, 7, granite, 2000.0f, 300.0f);
  w.Set(4, 8, water, 400.0f, 305.0f);
  w.Set(10, 5, granite, 2000.0f, 300.0f);
  w.Set(10, 6, water, 600.0f, 290.0f);
  return w;
}

// Hot gas against cold ice, for `DoPartialMelt`: each hot oxygen cell next to the ice pays
// for melting 5 kg off the ice's face without the block reaching its melting point. The block
// sits on the floor, so its top and sides are melted from above, left and right, and a single
// cell hangs in mid-air so the cell above it is melted from below too. The top-centre cell
// carries germs, for the share the melt takes with it. Headless, every melt is the refusal
// branch: the oxygen is displaced and the cell becomes 5 kg of water at the melting point
// plus 3 K. `meltfall` is the same world sent non-headless, where the game takes each 5 kg as
// a falling-liquid record instead and the oxygen only cools.
World PartialMelt(const Tables& t) {
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t ice = static_cast<uint16_t>(t.IndexOf(kIce));
  World w = LiquidRoom(t, 16, 12, oxygen, 5.0f, 500.0f);
  for (int x = 5; x < 10; ++x) {
    for (int y = 1; y < 4; ++y) w.Set(x, y, ice, 1000.0f, 268.0f);
  }
  w.SetDisease(7, 3, kFoodPoisoning, 20000000);
  w.Set(12, 7, ice, 800.0f, 267.0f);
  return w;
}

World PartialMeltFalling(const Tables& t) {
  World w = PartialMelt(t);
  w.gameside = true;
  return w;
}

// `melt` with the ice cold enough for the meltwater to freeze again: each 5 kg the melt
// leaves is far under 80% of Ice's default 1000 kg, so it refreezes as ore through the
// small-freeze rule, not as a thin ice cell.
World PartialMeltCold(const Tables& t) {
  World w = PartialMelt(t);
  const uint16_t ice = static_cast<uint16_t>(t.IndexOf(kIce));
  for (int x = 5; x < 10; ++x) {
    for (int y = 1; y < 4; ++y) w.Set(x, y, ice, 1000.0f, 250.0f);
  }
  w.Set(12, 7, ice, 800.0f, 250.0f);
  return w;
}

// The small-freeze rule in `DoStateTransition`'s low branch: a liquid that freezes while holding
// no more than 80% of its solid's default mass goes to the game as ore of that solid, germs and
// all, and the cell is cleared. Each liquid sits in its own one-cell pocket between granite
// pillars on a cold granite floor, so nothing flows and each freezes on its own. Water (Ice's
// default mass is 1000 kg) at 300 kg with germs, at exactly 800 kg (the `<= 0.8` edge) and at
// 900 kg; Polluted Water (Polluted Ice's is 500 kg) at 350 and 450 kg. The last water cell
// starts already under the freezing point, so it freezes on the first tick, while the game's
// visibility mask is still the zeroed one: `SpawnOre` refuses an unseen cell, and it freezes
// in place.
World SmallFreeze(const Tables& t) {
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  const uint16_t dirty = static_cast<uint16_t>(t.IndexOf(kDirtyWater));
  World w = LiquidRoom(t, 16, 8, oxygen, 1.0f, 150.0f);
  for (int x = 1; x < 15; ++x) w.Set(x, 1, granite, 2000.0f, 150.0f);
  for (int x = 2; x < 15; x += 2) w.Set(x, 2, granite, 2000.0f, 150.0f);
  w.Set(3, 2, water, 300.0f, 270.5f);
  w.SetDisease(3, 2, kFoodPoisoning, 1000000);
  w.Set(5, 2, water, 800.0f, 270.5f);
  w.Set(7, 2, water, 900.0f, 270.5f);
  w.Set(9, 2, dirty, 350.0f, 250.5f);
  w.Set(11, 2, dirty, 450.0f, 250.5f);
  w.Set(13, 2, water, 300.0f, 269.0f);
  return w;
}

// Condensation rain, the other half of `DoStateTransition`'s low branch: a gas that condenses
// into a liquid, in a sim that is not headless, goes to the game whole as a falling-liquid
// record and the cell is cleared, the way steam on a cold ceiling drips rather than leaving a
// water tile. Each steam cell sits in its own one-cell granite pocket, so no gas moves and each
// condenses on its own tick as the granite cools it. Klei's gate reads only the cell below's
// `kLiquidImpermeable` byte, not whether it is solid, so a pocket on a plain granite floor
// still drips. Left to right: 5, 10, 20 and 50 kg of steam at 500, 600, 700 and 800 K on plain
// granite (the 10 kg cell with germs), hot enough that each crosses after the game's visibility
// mask has arrived; 10 kg at 600 K on a floor marked liquid-impermeable (condenses in place as
// water); and 1 kg that starts at 365 K and so condenses on the first tick, while the mask is
// still the zeroed one (condenses in place too). Sent non-headless.
World Rain(const Tables& t) {
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t steam = static_cast<uint16_t>(t.IndexOf(kSteam));
  World w = LiquidRoom(t, 16, 8, oxygen, 1.0f, 300.0f);
  w.gameside = true;
  for (int x = 1; x < 15; ++x) {
    w.Set(x, 1, granite, 2000.0f, 300.0f);
    w.Set(x, 3, granite, 2000.0f, 300.0f);
  }
  for (int x = 2; x < 15; x += 2) w.Set(x, 2, granite, 2000.0f, 300.0f);
  w.Set(1, 2, steam, 5.0f, 500.0f);
  w.Set(3, 2, steam, 10.0f, 600.0f);
  w.SetDisease(3, 2, kFoodPoisoning, 1000000);
  w.Set(5, 2, steam, 20.0f, 700.0f);
  w.Set(7, 2, steam, 50.0f, 800.0f);
  w.Set(9, 2, steam, 10.0f, 600.0f);
  w.SetProperties(9, 1, kLiquidImpermeable);
  w.Set(11, 2, steam, 1.0f, 365.0f);
  return w;
}

// Over-full water against thin walls, for `DoPressureBreak`, and over-full columns under a
// lighter liquid, for the `DisplaceLiquid` that follows it. Granite everywhere; every liquid
// sits in a sealed one-cell pocket, so nothing flows and only the pressure branch acts.
//
// Top row, left to right, water against the wall on its right with a 1 kg oxygen pocket
// behind it (every other direction is three granite deep or the border, which never breaks):
//   * 1500 kg against one natural cell (strength byte 0: 1 + 0.1 < 1.5, breaks), two (breaks,
//     two records), three (never breaks);
//   * 3000 kg against a built tile's byte, `SetStrength(weight 0, x1)` = 4: 1 + 1.5 + 0.1
//     < 3, breaks; and against weight 1 (byte 0x84) over 1840 kg of granite, the mass term;
//   * 2000 kg against the same built tile, which holds;
//   * 1500 kg against a cell marked `Unbreakable`;
//   * 1500 kg against one cell with 1500 kg of water behind it (backpressure 1.5, holds both
//     ways) and with 200 kg behind it (1.3 left over, breaks).
// Bottom, three columns of 1800 kg water, above each a liquid and then oxygen: 100 kg of
// water, 300 kg of Polluted Water, and 1790 kg of water. The last starts inside the 1.01
// margin, but the flow pours most of the upper cell down before post-process reads it, so it
// displaces as well, with the other two.
//
// No germs. The over-full move's germ share matches Klei (checked with 1,000,000 germs in
// the Polluted Water column: 497507 up, 502483 left, at tick 2). Germs riding on the liquid
// the mover carries afterwards are `germflow`'s and `germliq`'s subject, so this scenario
// stays about the break.
World PressureBreak(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t dirty = static_cast<uint16_t>(t.IndexOf(kDirtyWater));
  World w;
  w.Init(60, 16, granite, 2000.0f, 300.0f);
  const int y = 11;
  w.Set(3, y, water, 1500.0f, 300.0f);
  w.Set(5, y, oxygen, 1.0f, 300.0f);
  w.Set(9, y, water, 1500.0f, 300.0f);
  w.Set(12, y, oxygen, 1.0f, 300.0f);
  w.Set(16, y, water, 1500.0f, 300.0f);
  w.Set(20, y, oxygen, 1.0f, 300.0f);
  w.Set(24, y, water, 3000.0f, 300.0f);
  w.Set(26, y, oxygen, 1.0f, 300.0f);
  w.Set(30, y, water, 3000.0f, 300.0f);
  w.Set(31, y, granite, 1840.0f, 300.0f);
  w.Set(32, y, oxygen, 1.0f, 300.0f);
  w.Set(36, y, water, 2000.0f, 300.0f);
  w.Set(38, y, oxygen, 1.0f, 300.0f);
  w.Set(42, y, water, 1500.0f, 300.0f);
  w.SetProperties(43, y, kUnbreakable);
  w.Set(44, y, oxygen, 1.0f, 300.0f);
  w.Set(48, y, water, 1500.0f, 300.0f);
  w.Set(50, y, water, 1500.0f, 300.0f);
  w.Set(54, y, water, 1500.0f, 300.0f);
  w.Set(56, y, water, 200.0f, 300.0f);
  const struct { int x; uint16_t e; float m; } columns[3] = {
      {3, water, 100.0f}, {9, dirty, 300.0f}, {15, water, 1790.0f}};
  for (const auto& c : columns) {
    w.Set(c.x, 2, water, 1800.0f, 300.0f);
    w.Set(c.x, 3, c.e, c.m, 300.0f);
    w.Set(c.x, 4, oxygen, 1.0f, 300.0f);
    w.Set(c.x, 5, oxygen, 1.0f, 300.0f);
  }
  return w;
}

// The built-tile strength bytes `PressureBreak` reads, sent the way `SimCellOccupier` and
// `MakeBaseSolid` send them: the byte is `(weight << 7) | (int)(multiplier * 4)`, and the
// message carries it as a float the sim truncates.
void PressureBreakSetup(Backend* s, const Tables&, const World& w) {
  SendCellFloat(s, SimMessageHash::SetStrengthValue, w.Cell(25, 11), 4.0f);
  SendCellFloat(s, SimMessageHash::SetStrengthValue, w.Cell(31, 11), 132.0f);
  SendCellFloat(s, SimMessageHash::SetStrengthValue, w.Cell(37, 11), 4.0f);
}

// Melting tiles, for `DoStateTransition`'s melt report. Every cell sits in its own one-cell
// pocket of Neutronium, which never transitions, so nothing flows and only transitions act.
// Left to right on one row:
//   * Ice at 290 K marked `kNotifyOnMelt`, melting on the first substep: reported;
//   * the same Ice unmarked: melts, not reported (the control);
//   * 900 kg of Water at 250 K marked: freezes (above the small-freeze share, so in place),
//     not reported, because only the high branch reads the bit;
//   * Ice at 265 K marked, beside Copper at 1000 K: melts part way through the run (tick 8);
//   * Copper at 1400 K marked, melting to molten copper: a solid with a hot melting point;
//   * Water at 400 K marked: boils, and is reported. The test is the high branch, not the
//     solid, and the sim never clears the bit (the game does, when it destroys the tile), so
//     a marked cell's meltwater that later boils is reported a second time.
World TileMelt(const Tables& t) {
  const uint16_t neutronium = static_cast<uint16_t>(t.IndexOf(kNeutronium));
  const uint16_t ice = static_cast<uint16_t>(t.IndexOf(kIce));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  const uint16_t copper = static_cast<uint16_t>(t.IndexOf(kCopper));
  World w;
  w.Init(20, 8, neutronium, 2000.0f, 270.0f);
  const int y = 4;
  w.Set(3, y, ice, 500.0f, 290.0f);
  w.SetProperties(3, y, kNotifyOnMelt);
  w.Set(6, y, ice, 500.0f, 290.0f);
  w.Set(9, y, water, 900.0f, 250.0f);
  w.SetProperties(9, y, kNotifyOnMelt);
  w.Set(12, y, ice, 500.0f, 265.0f);
  w.SetProperties(12, y, kNotifyOnMelt);
  w.Set(13, y, copper, 2000.0f, 1000.0f);
  w.Set(16, y, copper, 500.0f, 1400.0f);
  w.SetProperties(16, y, kNotifyOnMelt);
  w.Set(18, y, water, 900.0f, 400.0f);
  w.SetProperties(18, y, kNotifyOnMelt);
  return w;
}

// Germs riding on liquid. Klei's liquid mover hands each transfer
// `(int)((amount / mass) * count)` of the source's germs, priced against the running mass and
// count of the substep's first copy of the grid, and the destination merges them in with the
// mass. The germ pair sweep then prices against the copy taken AFTER the liquid moved. Three
// rooms of one world, walled apart with granite, 1 kg of oxygen everywhere else:
//   * a pool spreading on the floor, two germ types in it (left, right and down);
//   * water on a shelf pouring off its edge, and a block of water falling through the oxygen
//     (down into gas, down into the same liquid);
//   * a full cell at the foot of a one-wide shaft, rising into the gas above (up).
// The shaft alone told the two copies apart: Klei's 93,749 germs above the shaft on tick 2
// are the flow's 75,000 plus a diffusion share of the post-flow difference, not of the start
// count.
World GermFlow(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  World w = LiquidRoom(t, 30, 14, oxygen, 1.0f, 300.0f);
  for (int y = 1; y < 13; ++y) {
    w.Set(10, y, granite, 2000.0f, 293.15f);
    w.Set(20, y, granite, 2000.0f, 293.15f);
  }
  // The pool.
  for (int x = 3; x <= 6; ++x) {
    for (int y = 1; y <= 4; ++y) w.Set(x, y, water, 1000.0f, 295.0f);
  }
  w.SetDisease(3, 4, kFoodPoisoning, 1000000);
  w.SetDisease(6, 1, kFoodPoisoning, 50000);
  w.SetDisease(5, 2, 1, 200000);
  // The shelf and the falling block.
  for (int x = 11; x <= 15; ++x) w.Set(x, 5, granite, 2000.0f, 293.15f);
  for (int x = 12; x <= 14; ++x) {
    for (int y = 6; y <= 7; ++y) w.Set(x, y, water, 600.0f, 295.0f);
  }
  w.SetDisease(12, 7, kFoodPoisoning, 300000);
  for (int x = 17; x <= 18; ++x) {
    for (int y = 9; y <= 10; ++y) w.Set(x, y, water, 400.0f, 295.0f);
  }
  w.SetDisease(17, 10, kFoodPoisoning, 80000);
  // The shaft.
  for (int x = 21; x <= 28; ++x) {
    for (int y = 1; y < 13; ++y) {
      if (x != 25) w.Set(x, y, granite, 2000.0f, 293.15f);
    }
  }
  w.Set(25, 1, water, 1500.0f, 295.0f);
  w.SetDisease(25, 1, kFoodPoisoning, 900000);
  return w;
}

// The other two germ carriers, both in the second liquid sweep: the displacement
// slice (an eighth of the source's start count moves with an eighth of its mass) and
// `DisplaceLiquidDirectional`'s same-liquid merge (the whole cell's germs). Water beside
// `sunliquid`'s heavier liquid, germs of two types in both. Taking out either carrier moves the
// first divergence to tick 16. It was once a known gap at tick 20 with no germs
// involved: the second liquid sweep read its source element off the live grid, so a cell the
// mover had just turned into water displaced a neighbour as water.
World GermLiquids(const Tables& t) {
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t heavy = static_cast<uint16_t>(t.IndexOf(-1412059381));
  World w = LiquidRoom(t, 10, 14, oxygen, 1.0f, 300.0f);
  for (int y = 1; y <= 5; ++y) {
    for (int x = 2; x <= 4; ++x) w.Set(x, y, water, 1000.0f, 293.15f);
    for (int x = 5; x <= 7; ++x) w.Set(x, y, heavy, 870.0f, 293.15f);
  }
  w.SetDisease(4, 3, kFoodPoisoning, 500000);
  w.SetDisease(5, 3, 1, 400000);
  w.SetDisease(3, 1, kFoodPoisoning, 20000);
  return w;
}

// A settled column of water on the floor with gas above it. No falling: the water starts
// where it ends up, so what runs is the vertical pressure branch (the 1.01 gradient) and
// the horizontal spread, with a gas/liquid surface for them to work against.
World LiquidPool(const Tables& t) {
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(g_fill_vacuum ? kVacuum : kOxygen));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  World w = LiquidRoom(t, 16, 12, oxygen, g_fill_vacuum ? 0.0f : 1.0f, 300.0f);
  for (int x = 4; x < 12; ++x) {
    for (int y = 1; y < 5; ++y) w.Set(x, y, water, 1000.0f, 295.0f);
  }
  return w;
}

// Water pouring sideways into gas along a floor: the displacement path, where a liquid
// shoves a gas cell aside — up if it can, onward if it cannot. This is the one that the
// `liquid` scenario's first divergence pointed at, and it is deliberately one cell tall in
// the middle so that "up" is available for some cells and not others.
World LiquidPour(const Tables& t) {
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(g_fill_vacuum ? kVacuum : kOxygen));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  World w = LiquidRoom(t, 20, 10, oxygen, g_fill_vacuum ? 0.0f : 1.0f, 300.0f);
  for (int y = 1; y < 4; ++y) {
    for (int x = 9; x < 12; ++x) w.Set(x, y, water, 1000.0f, 295.0f);
  }
  return w;
}

// One over-pressured water cell at the bottom of a sealed one-wide shaft, with gas above
// it. The **upward** direction and nothing else: the walls are granite so left and right
// are refused, below is granite so the pour and the swap never run, and 1,500 kg is over
// water's 1,000 kg `maxMass` so the up branch has something to move.
//
// It exists to settle whether a liquid rising into gas displaces that gas. `UpdateLiquid`
// gates its up direction exactly the way it gates left and right, and both end in the same mover, which is what
// calls `DisplaceGas` — so it should. Nothing else in the suite can see it, because every
// other probe's liquid sits at or under `maxMass` and moves nothing upward at all.
World LiquidSqueeze(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(g_fill_vacuum ? kVacuum : kOxygen));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  World w;
  w.Init(16, 12, granite, 2000.0f, 293.15f);
  for (int y = 1; y < 9; ++y) {
    w.Set(8, y, oxygen, g_fill_vacuum ? 0.0f : 1.0f, 300.0f);
  }
  w.Set(8, 1, water, 1500.0f, 295.0f);
  return w;
}

// Solid world with a vacuum shaft open to the top. Built to exercise the sunlight
// texture, which is zero everywhere in a sealed world and so proves nothing there.
//
// It matches Klei through the first frame and then diverges at tick 2, for a reason
// worth recording: Klei zeroes the temperature of *massless* cells and announces it as a
// substanceChangeInfo, but not immediately — the shaft still reads 293.15 K after one
// tick and drops to 0 K on the second. That is a physics step, so a build with no
// physics cannot match it, and pretending otherwise by zeroing them eagerly would just
// diverge in the opposite direction on tick 1.
World Sunlit(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(24, 16, granite, 2000.0f, 293.15f);
  for (int y = 6; y < w.height; ++y) {
    for (int x = 10; x < 14; ++x) w.Set(x, y, vacuum, 0.0f, 293.15f);
  }
  return w;
}

// ------------------------------------------------------------------- sublimation
//
// `DoSublimation` and the liquid off-gas path have been implemented for a long time and
// **never once executed under `diffsim`**: granite is the only solid in every other
// scenario and its `sublimateIndex` is 0xffff, so the kernel walks its four neighbours and
// returns. Three `ChangeSubstance` sites hung off that — two in `DoSublimation` and the
// off-gas one — and nothing scored them until this scenario.
//
// OxyRock sublimates at probability 1.00, so it acts on every visit and the arithmetic is
// on show; ToxicSand at 0.05 acts almost never but **draws every time**, which is the half
// that decides the stream. ToxicMud is both sublimating and Unstable, so it is the one
// cell in the suite where the falling path and the sublimation path meet.
//
// **What it found, on the first run: `DoSublimation` displaces the free cell's gas before
// it writes the product, and this project overwrote it.** That is fixed, along with three
// larger things the same reading turned up — the solid being consumed where it stands, the
// vacuum test branching on the caller's captured phase, and the plain `+=` write. What is
// left here is a placement difference from tick 8 on; `sublvac` is the instrument for it.
// The original measurement, kept because it is what the scenario was built to show: Klei's
// ToxicSand at (15,3)
// gives up 0.02 kg; (16,3) becomes 0.0040 kg of ContaminatedOxygen — 0.02 at the 0.2
// efficiency — and the 1.5 kg of oxygen that was in (16,3) is found in (17,3), one cell
// further from the solid. The mechanism: `DoSublimation` calls
// `DisplaceGas` and only writes the product on the branch
// after it. `DisplaceGas` is one of the two callers of `DoDisplacement`, which is the
// source-destination-beyond eviction the gas displacement sweep uses.
//
// The draws agree tick for tick until the grids have diverged, so the probability gates,
// the visit order and the four-neighbour loop are all correct, and the product mass and
// element are correct too.
World Sublimate(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t oxyrock = static_cast<uint16_t>(t.IndexOf(kOxyRock));
  const uint16_t toxicsand = static_cast<uint16_t>(t.IndexOf(kToxicSand));
  const uint16_t toxicmud = static_cast<uint16_t>(t.IndexOf(kToxicMud));
  World w;
  w.Init(20, 16, granite, 2000.0f, 293.15f);
  // Two chambers. The left one is vacuum, so the free cell is under 1.8 kg from the start
  // and the kernel fires immediately; the right one is already full of oxygen, which is the
  // case where the mass gate has to close it.
  for (int y = 3; y < 13; ++y) {
    for (int x = 2; x < 9; ++x) w.Set(x, y, vacuum, 0.0f, 0.0f);
    for (int x = 11; x < 18; ++x) w.Set(x, y, oxygen, 1.5f, 293.15f);
  }
  for (int y = 3; y < 6; ++y) {
    w.Set(4, y, oxyrock, 400.0f, 293.15f);
    w.Set(13, y, oxyrock, 400.0f, 293.15f);
    w.Set(6, y, toxicsand, 1500.0f, 293.15f);
    w.Set(15, y, toxicsand, 1500.0f, 293.15f);
  }
  // Resting on the floor, so it sublimates for a while before anything about it falls, and
  // floating, so it does both.
  w.Set(7, 3, toxicmud, 1200.0f, 293.15f);
  w.Set(3, 9, toxicmud, 1200.0f, 293.15f);
  w.Set(16, 9, toxicmud, 1200.0f, 293.15f);
  return w;
}

// The displacement inside `DoSublimation`, on its own.
//
// `sublimate` proved the mechanism but is a crowded world: two sublimating solids, a
// falling unstable one, a vacuum chamber and an atmosphere, all interacting. This is the
// same kernel with everything else taken out. A single OxyRock tile stands in the middle of
// a room of ContaminatedOxygen; OxyRock sublimates to *Oxygen*, at probability 1.00, so
// every one of its four free neighbours holds a gas that is neither the product nor vacuum
// and every visit has to take the `DisplaceGas` branch. Nothing in this world falls, melts
// or changes state, so anything that differs here is the displacement.
World SublimateDisplace(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  // ContaminatedOxygen has no hash constant here, and does not need one: it is exactly
  // what ToxicSand sublimates into, so the table names it.
  const uint16_t contam =
      t.At(t.IndexOf(kToxicSand))->sublimateIndex;
  const uint16_t oxyrock = static_cast<uint16_t>(t.IndexOf(kOxyRock));
  World w;
  w.Init(12, 12, granite, 2000.0f, 293.15f);
  for (int y = 2; y < 10; ++y) {
    for (int x = 2; x < 10; ++x) w.Set(x, y, contam, 1.5f, 293.15f);
  }
  w.Set(5, 5, oxyrock, 400.0f, 293.15f);
  return w;
}

// The same displacement with the room taken away.
//
// `subldisp` still has a full atmosphere in it, so a divergence there could be the
// displacement or could be the sweeps that move gas past it. Here the room is vacuum and
// there is exactly one movable gas cell: the ContaminatedOxygen sitting between the OxyRock
// and the empty half of the room. The tile's other three neighbours are vacuum and take the
// product by a plain write, so the only cell whose fate is in question is that one.
World SublimateVacuum(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  const uint16_t contam = t.At(t.IndexOf(kToxicSand))->sublimateIndex;
  const uint16_t oxyrock = static_cast<uint16_t>(t.IndexOf(kOxyRock));
  World w;
  w.Init(12, 12, granite, 2000.0f, 293.15f);
  for (int y = 2; y < 10; ++y) {
    for (int x = 2; x < 10; ++x) w.Set(x, y, vacuum, 0.0f, 0.0f);
  }
  w.Set(5, 5, oxyrock, 400.0f, 293.15f);
  w.Set(4, 5, contam, 1.0f, 293.15f);
  return w;
}

// `sublvac`'s tick 6, lifted out of `sublvac`.
//
// Five ticks of sublimation are not the subject; the four cells they happen to produce are.
// This world is those four cells written down directly, at the masses `sublvac` holds at the
// end of tick 5, so the disagreement lands on tick 1 with one pressure sweep and one
// displacement sweep between the two grids instead of six frames of everything else.
//
// The row is ContaminatedOxygen at (4,9), Oxygen at (5,9) and (6,9), and ContaminatedOxygen
// underneath at (5,8) -- a mixed-gas horizontal pair with somewhere for the middle cell to be evicted
// to, sitting on top of a same-element vertical pair. Both are trace masses in a vacuum
// room, which is the regime nothing before `sublvac` covered.
World TraceRow(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  const uint16_t contam = t.At(t.IndexOf(kToxicSand))->sublimateIndex;
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  World w;
  w.Init(12, 12, granite, 2000.0f, 293.15f);
  for (int y = 2; y < 10; ++y) {
    for (int x = 2; x < 10; ++x) w.Set(x, y, vacuum, 0.0f, 0.0f);
  }
  w.Set(4, 9, contam, 0.00110592f, 293.15f);
  w.Set(5, 9, oxygen, 6.912e-05f, 293.15f);
  w.Set(6, 9, oxygen, 6.912e-05f, 293.15f);
  w.Set(5, 8, contam, 0.001152f, 293.15f);
  return w;
}

// `tracerow` with the degeneracy taken out.
//
// `sublvac`'s two candidate explanations for the same cell produce the *same number* there,
// because the two source masses happen to sit in the ratio 0.96 = 0.12 / 0.125: the pressure
// sweep's `flow * (mc - mn)` from below and the displacement sweep's `0.125 * start.mass`
// from the side both come to 0.00013824 kg. Raising the cell underneath to a mass that is
// not 0.96 of the one beside it separates them, and that is the only difference from
// `tracerow`.
World TraceFlow(const Tables& t) {
  World w = TraceRow(t);
  w.mass[w.Cell(5, 8)] = 0.002f;
  return w;
}

// The same row in a taller world, to tell a constant cutoff from one that scales.
//
// The displacement sweep's top bound is the one thing `tracerow` cannot pin down: at 12 high
// it only shows that row 9 is swept. Here the world is 16 high and the row sits at 13, which
// is `height - 3` again. If the bound is a fixed row this diverges; if it is measured from
// the top of the world it does not.
World TraceTall(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  const uint16_t contam = t.At(t.IndexOf(kToxicSand))->sublimateIndex;
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  World w;
  w.Init(16, 16, granite, 2000.0f, 293.15f);
  for (int y = 2; y < 14; ++y) {
    for (int x = 2; x < 14; ++x) w.Set(x, y, vacuum, 0.0f, 0.0f);
  }
  w.Set(4, 13, contam, 0.00110592f, 293.15f);
  w.Set(5, 13, oxygen, 6.912e-05f, 293.15f);
  w.Set(6, 13, oxygen, 6.912e-05f, 293.15f);
  w.Set(5, 12, contam, 0.002f, 293.15f);
  return w;
}

// The same displacement pushed against each side wall, to bound the sweep horizontally.
//
// Two independent copies of `traceflow`'s row, three rows apart so one substep cannot carry
// anything from one to the other. The lower one has its source on the room's **first**
// interior column and pushes right; the upper one has its source on the room's **last**
// interior column and pushes left. `x0 = max(min_x, 3)` refuses the first, `x1 = min(max_x,
// width - 3)` refuses the second, and Klei answers both.
World TraceWall(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  const uint16_t contam = t.At(t.IndexOf(kToxicSand))->sublimateIndex;
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  World w;
  w.Init(12, 12, granite, 2000.0f, 293.15f);
  for (int y = 2; y < 10; ++y) {
    for (int x = 2; x < 10; ++x) w.Set(x, y, vacuum, 0.0f, 0.0f);
  }
  // Source on x = 2, the first interior column, pushing right.
  w.Set(2, 8, contam, 0.00110592f, 293.15f);
  w.Set(3, 8, oxygen, 6.912e-05f, 293.15f);
  w.Set(4, 8, oxygen, 6.912e-05f, 293.15f);
  // Source on x = 9, the last interior column, pushing left.
  w.Set(9, 5, contam, 0.00110592f, 293.15f);
  w.Set(8, 5, oxygen, 6.912e-05f, 293.15f);
  w.Set(7, 5, oxygen, 6.912e-05f, 293.15f);
  return w;
}


// The liquid half of the same thing: DirtyWater is the only element in the table with a
// nonzero `offGasProbability`, so this scenario is the only one that can reach
// `DoLiquidOffGas` at all — its draw, its mass loss and its `ChangeSubstance`.
World OffGas(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t dirty = static_cast<uint16_t>(t.IndexOf(kDirtyWater));
  World w;
  w.Init(16, 16, granite, 2000.0f, 293.15f);
  for (int y = 2; y < 13; ++y) {
    for (int x = 2; x < 14; ++x) w.Set(x, y, oxygen, 1.0f, 293.15f);
  }
  // A settled pool with headroom above it, so the off-gas product has somewhere to go and
  // the cell-converts-in-place branch is not the only one exercised.
  for (int y = 2; y < 6; ++y) {
    for (int x = 2; x < 14; ++x) w.Set(x, y, dirty, 900.0f, 293.15f);
  }
  return w;
}

// The germ carriers of gas, sublimation, off-gassing and ore, one scenario per path, each seeded with germs
// of two types so that a merge that keeps the wrong type shows as well as a count.
//
// `germdisp`: `DoGasPressureDisplacement` (the destination's germs go on into `beyond` and
// the destination is cleared of them, then an eighth of the source's start count follows
// its gas in) and `DisplaceGas` (a gas shoved aside by falling water takes its germs with it
// rather than losing them). `gasmix`'s rows, germ-seeded, and a chamber of carbon dioxide
// that a block of water falls through.
World GermGasDisplace(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t co2 = static_cast<uint16_t>(t.IndexOf(1960575215));  // CarbonDioxide
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  World w;
  w.Init(24, 16, granite, 2000.0f, 293.15f);
  w.Set(5, 6, oxygen, 3.0f, 310.0f);
  w.Set(6, 6, co2, 1.0f, 290.0f);
  w.Set(7, 6, co2, 1.0f, 290.0f);
  w.SetDisease(5, 6, kFoodPoisoning, 100000);
  w.SetDisease(6, 6, kSlimelung, 40000);
  w.SetDisease(7, 6, kFoodPoisoning, 5000);
  w.Set(10, 3, oxygen, 3.0f, 320.0f);
  w.Set(10, 4, co2, 1.0f, 280.0f);
  w.Set(10, 5, co2, 1.0f, 280.0f);
  w.SetDisease(10, 3, kSlimelung, 70000);
  w.SetDisease(10, 4, kSlimelung, 30000);
  // The chamber.
  for (int y = 2; y < 13; ++y) {
    for (int x = 15; x < 22; ++x) w.Set(x, y, co2, 1.0f, 300.0f);
  }
  for (int x = 17; x <= 19; ++x) {
    for (int y = 10; y <= 11; ++y) w.Set(x, y, water, 400.0f, 295.0f);
  }
  for (int y = 2; y < 10; ++y) w.SetDisease(18, y, kFoodPoisoning, 60000 + 1000 * y);
  w.SetDisease(17, 5, kSlimelung, 90000);
  return w;
}

// `germsubl`: `DoSublimation` with germs on every path: the merge into the same gas (the
// oxygen chamber, some of it already carrying the other germ type), the write into vacuum,
// and the write after `DisplaceGas` (ToxicSand's product into oxygen); plus the solid paying
// its share. `sublimate`'s world. The solid's full clear under one germ is not scored: a share
// is at most half the count, so only a germ type with a zero count reaches it, and that type
// is cleared elsewhere in the same substep whether or not the rule is there.
World GermSublimate(const Tables& t) {
  World w = Sublimate(t);
  for (int y = 3; y < 6; ++y) {
    w.SetDisease(4, y, kFoodPoisoning, 200000);
    w.SetDisease(13, y, kFoodPoisoning, 150000);
    w.SetDisease(6, y, kSlimelung, 90000);
    w.SetDisease(15, y, kSlimelung, 3);
  }
  w.SetDisease(12, 4, kSlimelung, 50000);
  w.SetDisease(14, 3, kFoodPoisoning, 20000);
  w.SetDisease(7, 3, kFoodPoisoning, 400000);
  return w;
}

// `germoffgas`: the liquid off-gas path with germs. Polluted water in the game usually
// carries them, so this is an ordinary vanilla case. The pool off-gasses into oxygen that
// holds no germs, the same type, or the other type (which decides whether any are carried),
// and a lone few grams of the liquid in a pocket converts in place.
World GermOffGas(const Tables& t) {
  World w = OffGas(t);
  const uint16_t dirty = static_cast<uint16_t>(t.IndexOf(kDirtyWater));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  for (int x = 2; x < 14; ++x) {
    for (int y = 2; y < 6; ++y) w.SetDisease(x, y, kFoodPoisoning, 50000 + 7000 * x);
  }
  for (int x = 2; x < 6; ++x) w.SetDisease(x, 6, kFoodPoisoning, 3000);
  for (int x = 8; x < 12; ++x) w.SetDisease(x, 6, kSlimelung, 4000);
  // The pocket, in the granite band above the room.
  w.Set(7, 14, dirty, 0.004f, 293.15f);
  w.SetDisease(7, 14, kFoodPoisoning, 9000);
  (void)oxygen;
  return w;
}

// `germore`: a transition that names an ore hands the ore its share of the cell's germs,
// `(int)((float)count * share)`, and the cell pays them; and the ore is spawned only above
// 1 g. Toxic Mud boils into a gas with a transition ore; insulated so the product stays hot.
World GermOre(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t mud = static_cast<uint16_t>(t.IndexOf(kToxicMud));
  World w;
  w.Init(20, 12, granite, 1000.0f, 293.15f);
  w.Set(5, 5, mud, 100.0f, 380.0f);
  w.SetDisease(5, 5, kFoodPoisoning, 300000);
  w.SetInsulation(5, 5, 0);
  // Under 1 g of ore whatever the conversion, so the gate refuses it.
  w.Set(10, 5, mud, 0.0008f, 380.0f);
  w.SetDisease(10, 5, kSlimelung, 800000);
  w.SetInsulation(10, 5, 0);
  w.Set(15, 5, mud, 30.0f, 380.0f);
  w.SetInsulation(15, 5, 0);
  return w;
}

// The vertical site of `DoLiquidPressureDisplacement`'s call: the cell above is tested
// against the snapshot like the two sideways ones, but the source's `maxMass` gate reads the
// LIVE element and mass. Two water cells whose mass crosses 1000 kg within the substep, pushed
// by their neighbours: the left one starts under it and is pushed over, the right one starts
// over it and drains under. Above each, a short column of the heavier liquid for it to push.
World LiquidVerticalGate(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  const uint16_t heavy = static_cast<uint16_t>(t.IndexOf(-1412059381));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  World w;
  w.Init(16, 10, granite, 2000.0f, 293.15f);
  auto column = [&](int x, float side, float mid) {
    w.Set(x - 1, 1, water, side, 293.15f);
    w.Set(x, 1, water, mid, 293.15f);
    w.Set(x + 1, 1, water, side, 293.15f);
    w.Set(x, 2, heavy, 20.0f, 293.15f);
    w.Set(x, 3, heavy, 20.0f, 293.15f);
    for (int y = 4; y < 9; ++y) w.Set(x, y, oxygen, 1.0f, 293.15f);
  };
  column(4, 1040.0f, 995.0f);
  column(11, 960.0f, 1005.0f);
  return w;
}

// The world's **top row**, with gas in it that has somewhere to go.
//
// `StepGasPressure` reads the active region one row further than every other sweep, which
// was recovered from `sunlit`'s announce counts alone: `sunlit` puts a vacuum shaft on the
// top row, Klei announces those cells, and the half-open mask everything else uses does not
// reach them. An announce is not a flow, though, and no scenario had movable gas up there,
// so the two were indistinguishable. This is the scenario that separates them: a pressure
// gradient lying entirely along row `height - 1`, where the only kernel that can act on it
// is the one whose region is in question.
World TopRow(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(16, 16, granite, 2000.0f, 293.15f);
  // A shaft to the roof, so the top row is not sealed off from the rest of the world and a
  // vertical transfer has somewhere to come from.
  for (int y = 10; y < w.height; ++y) {
    for (int x = 4; x < 12; ++x) w.Set(x, y, vacuum, 0.0f, 0.0f);
  }
  // The gradient itself, all of it on the last row. Ten kilograms at one end and nothing at
  // the other: if the top row is simulated at all, this cannot stay where it is.
  for (int x = 4; x < 8; ++x) w.Set(x, w.height - 1, oxygen, 10.0f, 293.15f);
  for (int x = 8; x < 12; ++x) w.Set(x, w.height - 1, oxygen, 0.001f, 293.15f);
  return w;
}

// --------------------------------------------------------- regions and substeps
//
// Two things this file has never sent, both of which the game does send.
//
// **More than one active region.** `SimBase::UpdateData` is one loop over the
// region list, and the entire frame body sits
// inside it, past the outer
// `CellSOA::CopyFrom` but before conduction. Klei is region-outer, sweep-inner; we iterate
// the rectangles inside each sweep, which is sweep-outer, region-inner. One region makes
// the two the same walk, and one region is what every other scenario here sends.
//
// **More than one substep in a frame.** `kSubstepSeconds` is 0.2 and every scenario ticks
// at 0.2, so the substep loop has run exactly once per frame for the whole life of this
// harness. In Klei's sim the substep loop is *outside* `UpdateData`: the pressure
// direction is negated once, before the region loop, and the outer snapshot
// is taken once. Both of those are per-substep quantities in our model, so
// a frame carrying five substeps is the thing that checks that reading.
//
// One world serves all of it: two sealed rooms side by side, separated by a granite column,
// each holding the same pressure gradient and the same temperature ramp. Because the two
// rooms are built identical, an asymmetry between them in the output is itself a result —
// it means the two rooms were not simulated the same way, and with disjoint regions the
// only thing that can do that is the order the regions ran in.
inline constexpr int kRoomsWidth = 33;   // 15 + wall + 15, inside a granite shell
inline constexpr int kRoomsHeight = 16;
inline constexpr int kRoomsWall = 16;    // the granite column between the two rooms

World TwoRooms(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  World w;
  w.Init(kRoomsWidth, kRoomsHeight, granite, 2000.0f, 293.15f);
  for (int y = 1; y < kRoomsHeight - 1; ++y) {
    for (int x = 1; x < kRoomsWidth - 1; ++x) {
      if (x == kRoomsWall) continue;  // the wall stays granite
      // Position within the room the cell is in, so both rooms get the same gradient.
      const int rx = x < kRoomsWall ? x - 1 : x - kRoomsWall - 1;
      // Ten kilograms against a tenth of one: enough pressure to keep the flow sweep busy
      // for the whole run, and enough for the shuffle to have gas to move.
      const float mass = rx < 7 ? 10.0f : 0.1f;
      // A ramp steep enough to conduct, so the deterministic kernel is exercised too.
      const float temp = 280.0f + 4.0f * rx + 2.0f * y;
      w.Set(x, y, oxygen, mass, temp);
    }
  }
  return w;
}

// Two regions, disjoint and not even adjacent: the wall column belongs to neither, so no
// pair, snapshot or neighbour read can cross from one to the other. This is the shape a
// two-asteroid game actually sends, and the claim it tests is that region-outer and
// sweep-inner cannot be told apart when the regions do not interact — every kernel that
// draws lives in `StepPostProcess`, so both orderings spend the stream as A then B.
std::vector<Rect> RegionsSplit(const World& w) {
  return {Rect{1, 1, kRoomsWall, w.height - 1},
          Rect{kRoomsWall + 1, 1, w.width - 1, w.height - 1}};
}

// Between disjoint and overlapping sits touching: two regions that share a boundary and
// cover every cell exactly once. Nothing is processed twice, but a pair, a snapshot or a
// neighbour read at the seam has one end in each region, which is the cheapest way a
// region-outer sim can differ from a region-inner one.
std::vector<Rect> RegionsAdjacent(const World& w) {
  return {Rect{0, 0, kRoomsWall, w.height - 1},
          Rect{kRoomsWall, 0, w.width, w.height - 1}};
}

// The other shape: two regions that overlap across the wall and into both rooms. Klei runs
// the whole frame body twice over the overlap — conduction, then state change, then the
// fluid sweeps, then post-process, and only then the same seven again — while we run each
// sweep twice back to back. Nothing about that agrees, and it is deliberately the case with
// no random numbers in the disagreement: the overlap cells conduct twice in both sims, and
// what differs is what has happened to their neighbours in between.
std::vector<Rect> RegionsOverlap(const World& w) {
  return {Rect{1, 1, 21, w.height - 1}, Rect{12, 1, w.width - 1, w.height - 1}};
}

// The control: one region, spelled out rather than defaulted. It must score the same as the
// same world with no region function at all, which is what proves the payload is being
// built the way the single-struct path built it.
std::vector<Rect> RegionsOne(const World& w) {
  return {Rect{0, 0, w.width, w.height - 1}};
}

// The second control, and the one that matters. `regionsplit` first diverges on mass in the
// column at each region's `min_x` — which is a statement about a region's *edge*, not about
// there being two of them. This is one region with the same inset edges and both rooms
// inside it, so it changes only that. If it diverges too, the edge is the finding and the
// ordering is not implicated at all.
std::vector<Rect> RegionsInset(const World& w) {
  return {Rect{1, 1, w.width - 1, w.height - 1}};
}

// And the same inset applied to one room only, so the low edge in question is an interior
// column with gas on both sides of it rather than the shell.
std::vector<Rect> RegionsLeft(const World& w) {
  return {Rect{1, 1, kRoomsWall, w.height - 1}};
}

// The same two rooms with the gas replaced by granite at the identical temperatures, so
// nothing can flow and conduction is the only kernel left. `heatblock` cannot answer this
// question: its shell sits at the block's own temperature, so whether the shell column is
// in the region or out of it changes nothing there. Here the shell is 293.15 K against a
// ramp that runs 280 to 340, and the low edge is a pair that carries real heat.
World TwoRoomsSolid(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  World w = TwoRooms(t);
  for (size_t i = 0; i < w.Count(); ++i) {
    w.element[i] = granite;
    w.mass[i] = 2000.0f;
  }
  return w;
}

// The minimal shape of the `min_x` finding: two gas columns against the shell, a vertical
// mass gradient to drive the pressure sweep, and every temperature within a thousandth of a
// kelvin of its neighbours so conduction's 1 K dead zone is never crossed. Whatever moves
// mass here is the gas pressure sweep and nothing else, and the whole world fits in a dump.
World EdgeColumn(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  World w;
  w.Init(6, 10, granite, 2000.0f, 300.0f);
  for (int y = 1; y < 9; ++y) {
    for (int x = 1; x < 5; ++x) {
      // Heavy at the bottom, light at the top: a gradient the sweep must level vertically.
      const float mass = 10.0f - 1.0f * static_cast<float>(y);
      w.Set(x, y, oxygen, mass, 300.0f + 0.001f * static_cast<float>(y * 6 + x));
    }
  }
  return w;
}

// `regioninset` moves both low edges at once, which cannot say which one matters. These
// move one each.
std::vector<Rect> RegionsInsetX(const World& w) {
  return {Rect{1, 0, w.width, w.height - 1}};
}
std::vector<Rect> RegionsInsetY(const World& w) {
  return {Rect{0, 1, w.width, w.height - 1}};
}
// And the two far edges, pulled in by one. Between these four every edge of a region has
// been moved off the edge of the world exactly once, which is the only way to find out
// which of them the sim gets wrong.
std::vector<Rect> RegionsTrimX(const World& w) {
  return {Rect{0, 0, w.width - 1, w.height - 1}};
}
std::vector<Rect> RegionsTrimY(const World& w) {
  return {Rect{0, 0, w.width, w.height - 2}};
}

// ------------------------------------------------------------ unstable solids
//
// `PostProcessCell`'s solid branch, which nothing else in this file reaches: every other
// scenario's only solid is granite, and granite is stable. Sand tiles with nothing under
// them fall, and the kernel that decides *when* draws from the same `rand()` the gas
// shuffle draws from, so a world containing one is a stream test as much as a physics one.
//
// Vacuum around the sand, deliberately. The point of this scenario is that the only draws
// in the world are the unstable ones, so a divergence is unambiguous; `sandgas` below is
// the same world with an atmosphere, where the fall's draws and the shuffle's have to
// interleave correctly or nothing matches.
World SandFall(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  const uint16_t sand = static_cast<uint16_t>(t.IndexOf(kSand));
  World w;
  w.Init(20, 16, granite, 2000.0f, 293.15f);
  for (int y = 2; y < 14; ++y) {
    for (int x = 2; x < 18; ++x) w.Set(x, y, vacuum, 0.0f, 0.0f);
  }
  // Four drops of different lengths, so the fall loop is exercised at more than one
  // distance and the counters cannot all roll in step.
  w.Set(4, 12, sand, 1500.0f, 293.15f);
  w.Set(7, 9, sand, 1500.0f, 320.0f);
  w.Set(10, 6, sand, 800.0f, 293.15f);
  w.Set(13, 12, sand, 1500.0f, 293.15f);
  // A stack of two: the upper tile's support is the lower one until the lower one moves,
  // which is the case where the reset-on-support branch has to fire and then stop firing.
  w.Set(16, 4, sand, 1200.0f, 293.15f);
  w.Set(16, 5, sand, 1200.0f, 293.15f);
  // One resting on the floor already. It must never move and never draw.
  w.Set(6, 2, sand, 1500.0f, 293.15f);
  return w;
}

// The same sand in an oxygen atmosphere. Every gas cell draws, the falling tiles draw
// between them, and the sand falls *through* the gas rather than through nothing.
World SandGas(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t sand = static_cast<uint16_t>(t.IndexOf(kSand));
  World w;
  w.Init(20, 16, granite, 2000.0f, 293.15f);
  for (int y = 2; y < 14; ++y) {
    for (int x = 2; x < 18; ++x) w.Set(x, y, oxygen, 1.0f, 293.15f);
  }
  w.Set(4, 12, sand, 1500.0f, 293.15f);
  w.Set(7, 9, sand, 1500.0f, 320.0f);
  w.Set(10, 6, sand, 800.0f, 293.15f);
  w.Set(13, 12, sand, 1500.0f, 293.15f);
  w.Set(16, 4, sand, 1200.0f, 293.15f);
  w.Set(16, 5, sand, 1200.0f, 293.15f);
  w.Set(6, 2, sand, 1500.0f, 293.15f);
  return w;
}

// Conduction in isolation. Solid granite everywhere, so nothing can move and the only
// thing happening is heat transfer; a hot block in the middle relaxes outward. This is a
// must-match scenario: if it diverges, the conduction kernel is wrong, and nothing else
// can be blamed.
World HeatBlock(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t copper = static_cast<uint16_t>(t.IndexOf(kCopper));
  World w;
  w.Init(24, 16, granite, 1000.0f, 300.0f);
  for (int x = 8; x < 14; ++x) {
    for (int y = 6; y < 10; ++y) w.Set(x, y, granite, 1000.0f, 450.0f);
  }
  // A copper block too, so the geometric-mean pair rule is exercised across a real
  // material boundary and not just between identical cells.
  for (int x = 3; x < 7; ++x) {
    for (int y = 3; y < 7; ++y) w.Set(x, y, copper, 1000.0f, 320.0f);
  }
  // Varied masses, since the measured law says the rate ignores mass entirely and this
  // is where that claim would break if it were wrong.
  for (int y = 11; y < 14; ++y) {
    for (int x = 4; x < 20; ++x) w.mass[w.Cell(x, y)] = 200.0f + 300.0f * ((x + y) % 5);
  }
  return w;
}

// Cell insulation. Nothing else in the suite carries one — both payload writers used to put
// a literal 255 in every cell — so until this scenario existed the insulation factor was
// always exactly 1.0 and the `min` branch was unreachable. A kernel whose
// only insulated-cell behaviour is unreachable is a kernel nobody has tested.
//
// Granite everywhere, so nothing can move and conduction is the only thing running. The
// arrangement separates the two halves of the rule, because they fail differently:
//
//   * The **scale** half, `(insulation/255)^2`. A spread of values against the same hot
//     neighbour reads several different rates off one pair of elements, which is what
//     catches the byte being used unsquared, or scaled by 1/255 instead of 1/255^2.
//   * The **shape** half, that below 1.0 Klei drops the geometric mean for the minimum of
//     the two scaled conductivities. This is invisible between two cells of one element,
//     where the mean *is* the min, so it needs a material boundary with a wide ratio across
//     it: granite 3.39 against copper 60 has a mean of 14.26 and a min of 3.39. An
//     insulation of 254 is what isolates it — 1.6 % of attenuation, and a factor of four in
//     the rate, which no scaling could account for.
World Insulated(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t copper = static_cast<uint16_t>(t.IndexOf(kCopper));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  World w;
  w.Init(24, 16, granite, 1000.0f, 300.0f);

  // A hot cell with one insulated cell between it and the cold granite beyond, five times
  // over at five levels. 0 must stop heat dead; 254 must barely slow it.
  const uint8_t levels[5] = {0, 64, 128, 200, 254};
  for (int i = 0; i < 5; ++i) {
    const int y = 2 + i * 3;
    w.Set(3, y, granite, 1000.0f, 500.0f);
    w.SetInsulation(4, y, levels[i]);
  }

  // The material boundary. Only the copper side is insulated, so this is also the test that
  // the branch is taken when *either* factor is below 1.0 rather than both.
  for (int y = 2; y < 14; ++y) {
    w.Set(14, y, copper, 1000.0f, 450.0f);
    w.SetInsulation(14, y, 254);
  }

  // Both ends insulated, and unevenly, so `min` has to choose a side rather than land on a
  // value that would come out the same either way.
  w.Set(18, 4, copper, 1000.0f, 500.0f);
  w.SetInsulation(18, 4, 200);
  w.SetInsulation(19, 4, 64);

  // Insulation against a gas. Oxygen carries a solid surface-area multiplier of 25 and both
  // cells' multipliers apply, so this says the `min` branch still ends in the same
  // surface-area product the mean branch does — the easiest thing to drop when writing a
  // second branch that looks nearly like the first.
  for (int x = 18; x < 22; ++x) w.Set(x, 10, oxygen, 1.0f, 300.0f);
  w.Set(17, 10, granite, 1000.0f, 500.0f);
  w.SetInsulation(18, 10, 128);
  return w;
}

// Gas in sealed pockets of solid granite, at a range of pressures. Nothing here can
// change state or fall, so the only thing happening is gas flow.
World GasPockets(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(24, 16, granite, 2000.0f, 293.15f);
  // A long channel with everything at one end, so the propagation shape is exercised.
  for (int x = 2; x < 22; ++x) w.Set(x, 3, vacuum, 0.0f, 300.0f);
  w.Set(2, 3, oxygen, 20.0f, 320.0f);
  // A pocket with an uneven profile, so more than one pair is active at once.
  for (int x = 2; x < 22; ++x) w.Set(x, 8, oxygen, 0.5f + 0.4f * ((x * 7) % 5), 300.0f);
  // A small sealed pair, which is where the rate is cleanest.
  w.Set(4, 12, oxygen, 3.0f, 310.0f);
  w.Set(5, 12, oxygen, 1.0f, 290.0f);
  // Two vacuum pockets with granite between them, disconnected from the channel and from
  // each other. Klei leaves exactly one massless cell holding its seeded temperature while
  // zeroing all the others; these say whether "one" means one per world or one per region.
  for (int x = 8; x <= 10; ++x) w.Set(x, 6, vacuum, 0.0f, 305.0f);
  for (int x = 14; x <= 16; ++x) w.Set(x, 6, vacuum, 0.0f, 305.0f);
  return w;
}

// Two *different* gases, which is the one pair the pressure sweep refuses outright. Only
// `DoGasPressureDisplacement` can move them, and it needs a third cell in line to shove the
// displaced one into, so every group here is a run of three.
//
// The box is 24 wide because that sweep insets itself three cells rather than one; a room
// narrow enough for the inset to swallow it would report a clean pass for the wrong reason.
World GasMix(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t co2 = static_cast<uint16_t>(t.IndexOf(1960575215));  // CarbonDioxide
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(24, 16, granite, 2000.0f, 293.15f);

  // The oscillator: two gases with one empty cell between them. The
  // pressure sweep fills the gap from one side, and from the next substep on the far gas
  // displaces whatever is in it — 0.125 x 2.0 kg — and the cell flips element forever.
  w.Set(5, 3, oxygen, 2.0f, 300.0f);
  w.Set(6, 3, vacuum, 0.0f, 0.0f);
  w.Set(7, 3, co2, 2.0f, 305.0f);

  // Straight displacement into more of the same element: the middle cell is evicted into
  // the one past it, and the oxygen takes its place.
  w.Set(5, 6, oxygen, 3.0f, 310.0f);
  w.Set(6, 6, co2, 1.0f, 290.0f);
  w.Set(7, 6, co2, 1.0f, 290.0f);

  // The same pressure difference with nowhere to shove the middle cell. Nothing may move.
  w.Set(5, 9, oxygen, 3.0f, 310.0f);
  w.Set(6, 9, co2, 1.0f, 290.0f);

  // Vertical, so the down and up call sites are exercised as well as the horizontal pair.
  w.Set(14, 3, oxygen, 3.0f, 320.0f);
  w.Set(14, 4, co2, 1.0f, 280.0f);
  w.Set(14, 5, co2, 1.0f, 280.0f);
  return w;
}

// Liquid doing all the things liquid does: falling through vacuum, falling through gas,
// spreading along a floor, and stacking into a column deep enough to pressurise.
World LiquidWorld(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(g_fill_vacuum ? kVacuum : kOxygen));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(24, 20, granite, 2000.0f, 293.15f);
  // Open room, floor at y = 1.
  for (int y = 2; y < 18; ++y) {
    for (int x = 2; x < 22; ++x) w.Set(x, y, oxygen, g_fill_vacuum ? 0.0f : 1.0f, 300.0f);
  }
  // A slug of water dropped in the middle, and a second one over vacuum.
  for (int x = 8; x < 12; ++x) {
    for (int y = 14; y < 17; ++y) w.Set(x, y, water, 900.0f, 310.0f);
  }
  for (int y = 2; y < 14; ++y) {
    for (int x = 16; x < 19; ++x) w.Set(x, y, vacuum, 0.0f, 300.0f);
  }
  w.Set(17, 13, water, 1000.0f, 340.0f);
  // A pre-made pool at the bottom left, so the pressure branch runs from the first frame.
  for (int x = 2; x < 7; ++x) {
    for (int y = 2; y < 6; ++y) w.Set(x, y, water, 1000.0f, 295.0f);
  }
  return w;
}

// State changes with nothing else moving.
//
// Every sample is a *single* cell sealed on all four sides by granite, so no sample can
// flow, no two samples can touch, and the only two things running are conduction and the
// transition pass. That isolation is the point: the liquid scenarios below already carry a
// known gap in the flow kernel, and a transition scenario that shared it would report the
// flow gap forever and say nothing about transitions.
//
// The granite is at 350 K and the samples are spread from 180 K to 520 K, so over a couple
// of hundred ticks conduction walks most of them across a threshold rather than starting
// them past it — which exercises the boundary, the 1.5 K adjustment and the chaining all
// at once, and would catch an off-by-one-frame in when the transition is applied.
World Boiling(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  World w;
  w.Init(24, 16, granite, 2000.0f, 350.0f);
  const float temps[] = {180.0f, 220.0f, 262.0f, 269.0f, 271.0f, 274.0f, 290.0f,
                         340.0f, 371.0f, 374.0f, 376.0f, 380.0f, 420.0f, 520.0f};
  int i = 0;
  for (int y = 2; y < 15; y += 2) {
    for (int x = 2; x < 22; x += 2) {
      const float temp = temps[i % 14];
      // Alternating masses, because a light sample is dragged across its threshold by
      // conduction in a few frames and a heavy one takes most of the run.
      const float mass = (i % 3 == 0) ? 20.0f : ((i % 3 == 1) ? 200.0f : 1000.0f);
      w.Set(x, y, water, mass, temp);
      ++i;
    }
  }
  return w;
}

// Both sims write the same format, so each should accept the other's blob. A failure
// here means the replacement cannot load an existing save, which is the one
// compatibility promise that cannot be given up.
void CrossLoad(Backend* klei, Backend* mine, const Tables& t, const World& world) {
  const std::vector<uint8_t> payload = WorldPayload(world);

  auto save_from = [&](Backend* s) {
    Boot(s, t, payload);
    std::vector<uint8_t> blob = s->Save(0, 0);
    s->shutdown();
    return blob;
  };
  auto load_into = [&](Backend* s, const std::vector<uint8_t>& blob) {
    s->initialize(&Quiet);
    s->Send(SimMessageHash::Elements_CreateTable, t.elements);
    std::vector<uint8_t> alloc;
    const int32_t w = world.width, h = world.height;
    const uint8_t radiation = world.radiation ? 1 : 0, headless = 1;
    alloc.insert(alloc.end(), reinterpret_cast<const uint8_t*>(&w),
                 reinterpret_cast<const uint8_t*>(&w) + 4);
    alloc.insert(alloc.end(), reinterpret_cast<const uint8_t*>(&h),
                 reinterpret_cast<const uint8_t*>(&h) + 4);
    alloc.push_back(radiation);
    alloc.push_back(headless);
    s->Send(SimMessageHash::AllocateCells, alloc);
    s->Send(SimMessageHash::Disease_CreateTable, t.diseases);
    s->SendEmpty(SimMessageHash::ClearUnoccupiedCells);
    const bool ok = s->Send(SimMessageHash::Load, blob) != nullptr;
    s->shutdown();
    return ok;
  };

  const std::vector<uint8_t> from_klei = save_from(klei);
  const std::vector<uint8_t> from_mine = save_from(mine);
  printf("  klei blob %zu bytes, mine %zu bytes, %s\n", from_klei.size(),
         from_mine.size(),
         from_klei.size() == from_mine.size() ? "same size" : "SIZES DIFFER");
  if (from_klei.size() == from_mine.size() && !from_klei.empty()) {
    size_t bad = 0, first = 0;
    for (size_t i = 0; i < from_klei.size(); ++i) {
      if (from_klei[i] != from_mine[i]) {
        if (!bad) first = i;
        ++bad;
      }
    }
    if (bad) {
      printf("  %zu bytes differ, first at %zu\n", bad, first);
      // Decode the first differing cell record from both sides rather than reporting a
      // byte offset — an offset says nothing about which field is wrong.
      SaveBlob ka, ma;
      std::string err;
      if (DecodeSaveBlob(from_klei.data(), from_klei.size(), &ka, &err) &&
          DecodeSaveBlob(from_mine.data(), from_mine.size(), &ma, &err)) {
        for (size_t i = 0; i < ka.cells.size(); ++i) {
          if (memcmp(&ka.cells[i], &ma.cells[i], sizeof(SaveCell)) == 0) continue;
          const int32_t px = static_cast<int32_t>(i) % ka.width;
          const int32_t py = static_cast<int32_t>(i) / ka.width;
          const bool border = px == 0 || py == 0 || px == ka.width - 1 ||
                              py == ka.height - 1;
          printf("  first differing cell: padded (%d,%d)%s\n", px, py,
                 border ? " [border ring]" : "");
          printf("    klei  hash %11d  temp %8.3f  mass %10.3f  rad %g\n",
                 ka.cells[i].elementHash, ka.cells[i].temperature, ka.cells[i].mass,
                 ka.cells[i].radiation);
          printf("    mine  hash %11d  temp %8.3f  mass %10.3f  rad %g\n",
                 ma.cells[i].elementHash, ma.cells[i].temperature, ma.cells[i].mass,
                 ma.cells[i].radiation);
          break;
        }
        size_t cell_bad = 0, border_bad = 0;
        for (size_t i = 0; i < ka.cells.size(); ++i) {
          if (memcmp(&ka.cells[i], &ma.cells[i], sizeof(SaveCell)) == 0) continue;
          ++cell_bad;
          const int32_t px = static_cast<int32_t>(i) % ka.width;
          const int32_t py = static_cast<int32_t>(i) / ka.width;
          if (px == 0 || py == 0 || px == ka.width - 1 || py == ka.height - 1) {
            ++border_bad;
          }
        }
        printf("  %zu cell records differ, %zu of them in the border ring\n", cell_bad,
               border_bad);
      }
    } else {
      printf("  blobs are byte identical\n");
    }
  }
  printf("  klei loads mine: %s\n", load_into(klei, from_mine) ? "accepted" : "REJECTED");
  printf("  mine loads klei: %s\n", load_into(mine, from_klei) ? "accepted" : "REJECTED");
}

// The five property textures are raw buffers the game hands straight to
// Texture2D.LoadRawTextureData, with formats fixed by PropertyTextures.textureProperties:
// Flow is RGFloat (8 B/cell), Liquid / LiquidData / MaterialData are RGBA32 (4 B/cell),
// ExposedToSunlight is Alpha8 (1 B/cell). What is *in* them is defined by the shaders,
// so read Klei's for a world whose contents are known and correlate.
void ProbeTextures(Backend* klei, const Tables& t, const World& w) {
  const GameDataUpdate* g = Boot(klei, t, WorldPayload(w));
  if (!g) {
    printf("  boot failed\n");
    return;
  }
  std::vector<uint8_t> visible(w.Count(), 1);
  for (int i = 0; i < 20; ++i) g = Tick(klei, w, &visible);
  if (!g) {
    printf("  tick failed\n");
    return;
  }

  printf("  flow          %s\n", g->propertyTextureFlow ? "non-null" : "NULL");
  printf("  liquid        %s\n", g->propertyTextureLiquid ? "non-null" : "NULL");
  printf("  liquidData    %s\n", g->propertyTextureLiquidData ? "non-null" : "NULL");
  printf("  materialData  %s\n", g->propertyTextureMaterialData ? "non-null" : "NULL");
  printf("  sunlight      %s\n", g->propertyTextureExposedToSunlight ? "non-null" : "NULL");
  if (!g->propertyTextureFlow) {
    printf("  the sim produced no textures for this world\n");
    return;
  }

  const auto* flow = static_cast<const float*>(g->propertyTextureFlow);
  const auto* liquid = static_cast<const uint8_t*>(g->propertyTextureLiquid);
  const auto* liquid_data = static_cast<const uint8_t*>(g->propertyTextureLiquidData);
  const auto* material = static_cast<const uint8_t*>(g->propertyTextureMaterialData);
  const auto* sun = static_cast<const uint8_t*>(g->propertyTextureExposedToSunlight);

  printf("  %-12s %-22s %-13s %-13s %-13s %-13s %s\n", "cell", "element/mass",
         "flow(r,g)", "liquid", "liquidData", "material", "sun");
  {
    // Element.colour and the gradient entries are the obvious source for the constant
    // RGB seen in the liquid textures, so print them rather than eyeballing the match.
    const int32_t wi = t.IndexOf(kWater);
    if (wi >= 0) {
      const uint32_t c = t.At(wi)->colour;
      printf("  water colour 0x%08x -> r%u g%u b%u a%u\n", c, (c >> 16) & 0xFF,
             (c >> 8) & 0xFF, c & 0xFF, (c >> 24) & 0xFF);
      for (int k = 0; k < 6; ++k) {
        const uint32_t gc = t.At(wi)->gradientColours[k];
        printf("    gradient[%d] 0x%08x -> r%u g%u b%u a%u\n", k, gc, (gc >> 16) & 0xFF,
               (gc >> 8) & 0xFF, gc & 0xFF, (gc >> 24) & 0xFF);
      }
      printf("  water maxMass %.1f\n", t.At(wi)->maxMass);
    }
  }
  // Alpha against mass, read straight off the settled column: if it is a simple curve
  // this will show it.
  printf("  mass -> liquid.a / liquidData.a, down the water column at x=12\n");
  for (int y = 1; y < 8; ++y) {
    const int c = w.Cell(12, y);
    if (g->mass[c] <= 0) continue;
    const float frac = g->mass[c] / 1000.0f;
    const Element* ep = t.At(g->elementIdx[c]);
    const float tnorm = ep->highTemp > ep->lowTemp
                            ? (g->temperature[c] - ep->lowTemp) /
                                  (ep->highTemp - ep->lowTemp)
                            : 0.0f;
    printf("    y=%2d elem %3u mass %8.2f frac %.4f temp %8.3f | liquid.a %3u vs"
           " 255*frac^0.45 = %6.1f | data.a %3u vs 255*tnorm = %6.1f  rgb %3u,%3u,%3u\n",
           y, g->elementIdx[c], g->mass[c], frac, g->temperature[c], liquid[c * 4 + 3],
           255.0f * std::pow(frac, 0.45f), liquid_data[c * 4 + 3], 255.0f * tnorm,
           liquid_data[c * 4], liquid_data[c * 4 + 1], liquid_data[c * 4 + 2]);
  }
  const int probes[][2] = {{2, 2}, {12, 12}, {12, 8}, {12, 3}, {1, 1}, {12, 14},
                           {0, 15}, {12, 15}, {12, 0}};
  for (const auto& pr : probes) {
    const int c = w.Cell(pr[0], pr[1]);
    printf("  (%2d,%2d) %-6zu %5u %9.2f kg  %5.2f %5.2f  %3u %3u %3u %3u  %3u %3u %3u %3u"
           "  %3u %3u %3u %3u  %3u\n",
           pr[0], pr[1], static_cast<size_t>(c), g->elementIdx[c], g->mass[c],
           flow[c * 2], flow[c * 2 + 1],
           liquid[c * 4], liquid[c * 4 + 1], liquid[c * 4 + 2], liquid[c * 4 + 3],
           liquid_data[c * 4], liquid_data[c * 4 + 1], liquid_data[c * 4 + 2],
           liquid_data[c * 4 + 3],
           material[c * 4], material[c * 4 + 1], material[c * 4 + 2], material[c * 4 + 3],
           sun[c]);
  }
  klei->shutdown();
}

// Measure Klei's conduction kernel exactly.
//
// Solids cannot move, so a world made entirely of solid tiles has exactly one thing
// happening in it: heat transfer. Any temperature change is conduction and nothing else,
// which makes the formula recoverable by watching a single hot cell relax into its
// neighbours instead of by guessing at it.
//
// Two probes: a symmetric one (one hot granite cell in cold granite, so all four
// neighbours are identical) that isolates the rate, and a mixed-material one that shows
// how two different conductivities combine.
void ProbeConduction(Backend* klei, const Tables& t, int ticks) {
  const int32_t gi = t.IndexOf(kGranite);
  const int32_t wi = t.IndexOf(kWater);
  if (gi < 0 || wi < 0) {
    printf("  required elements missing\n");
    return;
  }
  const uint16_t granite = static_cast<uint16_t>(gi);
  printf("  granite: shc %.6f  conductivity %.6f  molarMass %.4f\n",
         t.At(gi)->specificHeatCapacity, t.At(gi)->thermalConductivity,
         t.At(gi)->molarMass);
  printf("  water:   shc %.6f  conductivity %.6f\n", t.At(wi)->specificHeatCapacity,
         t.At(wi)->thermalConductivity);

  World w;
  w.Init(16, 16, granite, 1000.0f, 300.0f);
  const int hot = w.Cell(8, 8);
  w.mass[hot] = 1000.0f;
  w.temperature[hot] = 400.0f;

  const GameDataUpdate* g = Boot(klei, t, WorldPayload(w));
  if (!g) {
    printf("  boot failed\n");
    return;
  }
  std::vector<uint8_t> visible(w.Count(), 1);
  const int right = w.Cell(9, 8), up = w.Cell(8, 9), far_cell = w.Cell(10, 8);

  printf("  one 400 K granite cell in 300 K granite, all cells 1000 kg, dt = 0.2 s\n");
  printf("  %-5s %-12s %-12s %-12s %-12s %s\n", "tick", "hot(8,8)", "right(9,8)",
         "up(8,9)", "far(10,8)", "dT_hot");
  double prev = g->temperature[hot];
  for (int i = 0; i <= ticks; ++i) {
    if (i <= 12 || i % 20 == 0) {
      printf("  %-5d %-12.6f %-12.6f %-12.6f %-12.6f %+.6f\n", i, g->temperature[hot],
             g->temperature[right], g->temperature[up], g->temperature[far_cell],
             g->temperature[hot] - prev);
    }
    prev = g->temperature[hot];
    const GameDataUpdate* n = Tick(klei, w, &visible);
    if (!n) break;
    g = n;
  }

  // Total energy, to confirm conduction alone conserves it and that the sum is the right
  // invariant to test a replacement kernel against.
  double e = 0;
  for (size_t c = 0; c < w.Count(); ++c) {
    e += static_cast<double>(g->mass[c]) *
         t.At(g->elementIdx[c])->specificHeatCapacity * g->temperature[c];
  }
  printf("  final total energy %.3f kJ (started %.3f kJ)\n", e,
         (static_cast<double>(w.Count()) - 1) * 1000.0 *
                 t.At(gi)->specificHeatCapacity * 300.0 +
             1000.0 * t.At(gi)->specificHeatCapacity * 400.0);
  klei->shutdown();
}

// Second probe: an isolated pair.
//
// The first attempt at this put four different neighbours around one hot cell and read
// them on the same tick. That was contaminated — each neighbour also conducts onwards,
// and a copper neighbour drains the hot cell fast enough to move everyone else's
// temperature difference before the measurement is taken. The numbers looked
// non-monotonic in mass, which is the signature of a bad experiment rather than a
// strange kernel.
//
// So: a world of vacuum, which has no mass and therefore cannot conduct, containing
// exactly two adjacent cells. Nothing else in the grid can participate, and every joule
// that leaves one cell has exactly one place to go.
void ProbePair(Backend* klei, const Tables& t, const char* label, int32_t elem_a,
               float mass_a, float temp_a, int32_t elem_b, float mass_b, float temp_b,
               int ticks) {
  const int32_t vi = t.IndexOf(kVacuum);
  if (vi < 0 || elem_a < 0 || elem_b < 0) return;

  World w;
  w.Init(12, 12, static_cast<uint16_t>(vi), 0.0f, 300.0f);
  const int a = w.Cell(5, 5), b = w.Cell(6, 5);
  w.element[a] = static_cast<uint16_t>(elem_a);
  w.mass[a] = mass_a;
  w.temperature[a] = temp_a;
  w.element[b] = static_cast<uint16_t>(elem_b);
  w.mass[b] = mass_b;
  w.temperature[b] = temp_b;

  const GameDataUpdate* g = Boot(klei, t, WorldPayload(w));
  if (!g) {
    printf("  %s: boot failed\n", label);
    return;
  }
  std::vector<uint8_t> visible(w.Count(), 1);

  const double ca = mass_a * t.At(elem_a)->specificHeatCapacity;
  const double cb = mass_b * t.At(elem_b)->specificHeatCapacity;
  const double ka = t.At(elem_a)->thermalConductivity;
  const double kb = t.At(elem_b)->thermalConductivity;
  printf("  %s\n", label);
  printf("    A: k %.4f shc %.4f mass %.1f -> C %.2f kJ/K   B: k %.4f shc %.4f"
         " mass %.1f -> C %.2f kJ/K\n", ka, t.At(elem_a)->specificHeatCapacity, mass_a,
         ca, kb, t.At(elem_b)->specificHeatCapacity, mass_b, cb);

  double prev_a = g->temperature[a];
  for (int i = 0; i <= ticks; ++i) {
    const double ta = g->temperature[a], tb = g->temperature[b];
    const double moved = (prev_a - ta) * ca;  // kJ that left A this tick
    if (i > 0 && moved != 0.0) {
      // k_eff implied by dQ = k_eff * dT * dt, using the difference at the *start* of
      // the tick, which is what the sim would have seen.
      printf("    tick %-3d A %10.5f  B %10.5f  dQ %9.4f kJ  implied k_eff %8.4f\n", i,
             ta, tb, moved, moved / (0.2 * (prev_a - tb)));
    } else if (i <= 3) {
      printf("    tick %-3d A %10.5f  B %10.5f\n", i, ta, tb);
    }
    prev_a = ta;
    const GameDataUpdate* n = Tick(klei, w, &visible);
    if (!n) break;
    g = n;
  }
  klei->shutdown();
}

// ------------------------------------------------------------------- flow probes
//
// Flow is the next kernel, and unlike conduction it cannot be isolated by choosing a
// world where nothing else happens — anything that moves also conducts, changes state and
// displaces. So the isolation here is different: pockets of gas or liquid sealed inside
// solid granite, one pocket per experiment, with the *same* element on both sides so
// there is no state change and no material boundary. Whatever mass moves, flow moved it.
//
// `Element` already carries the parameters the kernel must be using — `flow`, `viscosity`,
// `minHorizontalFlow`, `minVerticalFlow`, `maxMass`. The job is to find how they combine,
// not to guess which ones matter.

// Print the flow-related fields of the elements the probes use, so the measured numbers
// below can be read against the inputs the sim actually has.
void ProbeFlowParams(const Tables& t) {
  const struct { const char* name; int32_t hash; } wanted[] = {
      {"Vacuum", kVacuum}, {"Oxygen", kOxygen}, {"Water", kWater},
      {"Granite", kGranite},
  };
  printf("  %-10s %-6s %8s %8s %8s %8s %8s %8s %8s\n", "element", "state", "flow",
         "viscos", "minHFlow", "minVFlow", "maxMass", "molarM", "shc");
  for (const auto& e : wanted) {
    const int32_t i = t.IndexOf(e.hash);
    if (i < 0) continue;
    const Element* p = t.At(i);
    printf("  %-10s %-6u %8.4f %8.4f %8.4f %8.4f %8.2f %8.4f %8.4f\n", e.name, p->state,
           p->flow, p->viscosity, p->minHorizontalFlow, p->minVerticalFlow, p->maxMass,
           p->molarMass, p->specificHeatCapacity);
  }
  // Carbon dioxide and any other gas, found by scanning, so a two-gas probe has something
  // to work with without hardcoding another hash.
  printf("  other gases in the table (state & 3 == 1), first 8:\n");
  int shown = 0;
  for (int32_t i = 0; i < t.count && shown < 8; ++i) {
    const Element* p = t.At(i);
    if ((p->state & 3) != 1 || p->maxMass <= 0.0f) continue;
    ++shown;
    printf("    idx %3d hash %11d  flow %6.3f visc %6.3f minH %6.3f minV %6.3f"
           " maxMass %7.2f molar %7.3f\n",
           i, p->id, p->flow, p->viscosity, p->minHorizontalFlow, p->minVerticalFlow,
           p->maxMass, p->molarMass);
  }
}

// Run one flow experiment on a fresh sim and print the watched cells every tick.
//
// Each experiment gets its own Backend because the sim has to be shut down between runs
// and a shut-down sim cannot be restarted in place.
void RunFlow(const char* dll, const Tables& t, const char* label, const World& w,
             const std::vector<std::pair<int, int>>& watch, int ticks, float dt = 0.2f) {
  Backend s;
  if (!s.Bind(dll, "flow")) return;
  const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
  if (!g) {
    printf("  %s: boot failed\n", label);
    s.shutdown();
    return;
  }
  std::vector<uint8_t> visible(w.Count(), 1);

  printf("  %s\n", label);
  printf("    %-5s", "tick");
  for (const auto& p : watch) {
    char h[16];
    snprintf(h, sizeof(h), "(%d,%d)", p.first, p.second);
    printf(" %14s", h);
  }
  printf(" %12s\n", "total kg");

  for (int i = 0; i <= ticks; ++i) {
    double total = 0;
    for (size_t c = 0; c < w.Count(); ++c) {
      // Granite walls dominate the total and would hide a gram of gas moving, so only
      // non-solid mass is counted. Conservation of the *pocket* is what matters.
      if ((t.At(g->elementIdx[c])->state & 3) != 3) total += g->mass[c];
    }
    printf("    %-5d", i);
    for (const auto& p : watch) {
      const int c = w.Cell(p.first, p.second);
      printf(" %5u/%8.3f", g->elementIdx[c], g->mass[c]);
    }
    printf(" %12.4f\n", total);
    const GameDataUpdate* n = Tick(&s, w, &visible, dt);
    if (!n) break;
    g = n;
  }
  s.shutdown();
}

// A block of solid granite with a pocket carved out of it. The granite is the apparatus:
// it cannot move, cannot change state at these temperatures, and seals the pocket so no
// mass can leave the experiment.
World SolidBlock(const Tables& t, int width, int height) {
  World w;
  w.Init(width, height, static_cast<uint16_t>(t.IndexOf(kGranite)), 2000.0f, 293.15f);
  return w;
}

void ProbeFlow(const Tables& t, const char* dll) {
  const int32_t gas = t.IndexOf(kOxygen);
  const int32_t liq = t.IndexOf(kWater);
  const int32_t vac = t.IndexOf(kVacuum);
  if (gas < 0 || liq < 0 || vac < 0) {
    printf("  required elements missing\n");
    return;
  }
  const uint16_t oxygen = static_cast<uint16_t>(gas);
  const uint16_t water = static_cast<uint16_t>(liq);
  const uint16_t vacuum = static_cast<uint16_t>(vac);

  // --- gas, two cells side by side, sealed. The only question this asks is how much mass
  // moves per frame against a known difference, and whether it is a fraction or a fixed
  // amount.
  {
    World w = SolidBlock(t, 12, 12);
    w.Set(5, 5, oxygen, 2.0f, 300.0f);
    w.Set(6, 5, oxygen, 1.0f, 300.0f);
    RunFlow(dll, t, "gas pair, horizontal, sealed: 2.0 kg | 1.0 kg", w,
            {{5, 5}, {6, 5}}, 12);
  }
  // Same pocket, one cell empty. A gas expanding into vacuum is the cleanest possible
  // reading of the rate, because the receiving side contributes nothing.
  {
    World w = SolidBlock(t, 12, 12);
    w.Set(5, 5, oxygen, 2.0f, 300.0f);
    w.Set(6, 5, vacuum, 0.0f, 300.0f);
    RunFlow(dll, t, "gas into vacuum, horizontal: 2.0 kg | empty", w, {{5, 5}, {6, 5}}, 12);
  }
  // Vertical, to see whether gravity or minVerticalFlow makes the two directions differ.
  {
    World w = SolidBlock(t, 12, 12);
    w.Set(5, 5, oxygen, 2.0f, 300.0f);
    w.Set(5, 6, oxygen, 1.0f, 300.0f);
    RunFlow(dll, t, "gas pair, vertical, sealed: 2.0 kg below | 1.0 kg above", w,
            {{5, 5}, {5, 6}}, 12);
  }
  // A long horizontal channel with all the gas at one end. This shows the propagation
  // *shape* — how many cells move per frame — which a two-cell probe cannot.
  {
    World w = SolidBlock(t, 16, 12);
    for (int x = 2; x <= 13; ++x) w.Set(x, 5, oxygen, 0.0f, 300.0f);
    for (int x = 2; x <= 13; ++x) w.element[w.Cell(x, 5)] = vacuum;
    w.Set(2, 5, oxygen, 12.0f, 300.0f);
    RunFlow(dll, t, "gas channel 12 wide, all 12 kg at the left end", w,
            {{2, 5}, {3, 5}, {4, 5}, {5, 5}, {6, 5}}, 12);
  }
  // --- liquid falling down a shaft. Whole-cell or fractional is the single most
  // consequential thing to know, because it decides whether a volume-fraction model can
  // reproduce ONI's water at all.
  {
    World w = SolidBlock(t, 12, 14);
    for (int y = 2; y <= 10; ++y) {
      w.Set(5, y, vacuum, 0.0f, 300.0f);
    }
    w.Set(5, 10, water, 1000.0f, 300.0f);
    RunFlow(dll, t, "liquid drop: 1000 kg water at the top of a 9-cell shaft", w,
            {{5, 10}, {5, 8}, {5, 6}, {5, 4}, {5, 2}}, 14);
  }
  // Partial mass, to see whether a light cell falls the same way a full one does.
  {
    World w = SolidBlock(t, 12, 14);
    for (int y = 2; y <= 10; ++y) w.Set(5, y, vacuum, 0.0f, 300.0f);
    w.Set(5, 10, water, 50.0f, 300.0f);
    RunFlow(dll, t, "liquid drop: 50 kg water, same shaft", w,
            {{5, 10}, {5, 8}, {5, 6}, {5, 4}, {5, 2}}, 14);
  }
  // --- liquid spreading along a flat floor. Horizontal liquid flow has its own minimum
  // (`minHorizontalFlow`) and is the thing that makes ONI water level itself.
  {
    World w = SolidBlock(t, 16, 12);
    for (int x = 2; x <= 13; ++x) w.Set(x, 5, vacuum, 0.0f, 300.0f);
    for (int x = 2; x <= 13; ++x) w.Set(x, 6, vacuum, 0.0f, 300.0f);
    w.Set(2, 5, water, 1000.0f, 300.0f);
    RunFlow(dll, t, "liquid spread: 1000 kg at the left end of a 12-wide floor", w,
            {{2, 5}, {3, 5}, {4, 5}, {5, 5}, {6, 5}}, 16);
  }
  // --- a column of full water cells, to find out what happens above maxMass and whether
  // ONI's liquid compresses under its own weight.
  {
    World w = SolidBlock(t, 12, 14);
    for (int y = 2; y <= 10; ++y) w.Set(5, y, water, 1000.0f, 300.0f);
    RunFlow(dll, t, "water column, 9 cells all at 1000 kg (maxMass)", w,
            {{5, 2}, {5, 4}, {5, 6}, {5, 8}, {5, 10}}, 10);
  }
}

// Round two. The first pass gave the shape of both laws; these pin down the constants
// that a single element and a single timestep cannot distinguish between.
void ProbeFlow2(const Tables& t, const char* dll) {
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));

  // Every liquid in the table, so a second liquid can separate `viscosity` from `maxMass`
  // as the source of the transfer cap. Water's viscosity is 125 and its maxMass is 1000,
  // and 1000/8 is also 125, so water alone cannot tell them apart.
  printf("  liquids in the table (state & 3 == 2):\n");
  int shown = 0;
  for (int32_t i = 0; i < t.count && shown < 14; ++i) {
    const Element* p = t.At(i);
    if ((p->state & 3) != 2 || p->maxMass <= 0.0f) continue;
    ++shown;
    printf("    idx %3d hash %11d  flow %7.3f visc %8.3f minH %6.3f minV %6.3f"
           " maxMass %8.2f  maxMass/8 %8.3f\n",
           i, p->id, p->flow, p->viscosity, p->minHorizontalFlow, p->minVerticalFlow,
           p->maxMass, p->maxMass / 8.0f);
  }

  // --- is the coefficient per frame or per second?
  //
  // Conduction scaled with dt. If flow does too, the same pocket at half the timestep
  // moves half the mass; if the constants are per *frame*, it moves the same amount. This
  // is the difference between a rate and a fraction and it cannot be read off a single
  // timestep.
  for (float dt : {0.1f, 0.2f, 0.4f}) {
    World w = SolidBlock(t, 12, 12);
    w.Set(5, 5, oxygen, 2.0f, 300.0f);
    w.Set(6, 5, vacuum, 0.0f, 300.0f);
    char label[96];
    snprintf(label, sizeof(label), "gas into vacuum at dt = %.2f s", dt);
    RunFlow(dll, t, label, w, {{5, 5}, {6, 5}}, 4, dt);
  }

  // --- two different gases side by side. Whose `flow` decides the rate, and can two
  // elements share a cell at all? Oxygen's flow is 0.12; most other gases are 0.10.
  {
    int32_t other = -1;
    for (int32_t i = 0; i < t.count; ++i) {
      const Element* p = t.At(i);
      if ((p->state & 3) == 1 && p->maxMass > 0.0f && p->flow > 0.0f &&
          p->flow != t.At(t.IndexOf(kOxygen))->flow) {
        other = i;
        break;
      }
    }
    if (other >= 0) {
      printf("  second gas: idx %d hash %d flow %.3f molar %.3f\n", other,
             t.At(other)->id, t.At(other)->flow, t.At(other)->molarMass);
      World w = SolidBlock(t, 12, 12);
      w.Set(5, 5, oxygen, 2.0f, 300.0f);
      w.Set(6, 5, static_cast<uint16_t>(other), 1.0f, 300.0f);
      RunFlow(dll, t, "two different gases, horizontal: oxygen 2.0 | other 1.0", w,
              {{5, 5}, {6, 5}}, 10);
      // Vertically, with the heavier gas on top: ONI stratifies gases by molar mass and
      // that has to come from somewhere.
      World v = SolidBlock(t, 12, 12);
      v.Set(5, 5, oxygen, 1.0f, 300.0f);
      v.Set(5, 6, static_cast<uint16_t>(other), 1.0f, 300.0f);
      RunFlow(dll, t, "two gases, equal mass, heavier on top (stratification?)", v,
              {{5, 5}, {5, 6}}, 10);
    }
  }

  // --- a second liquid, to separate viscosity from maxMass in the cap.
  {
    int32_t other = -1;
    for (int32_t i = 0; i < t.count; ++i) {
      const Element* p = t.At(i);
      if ((p->state & 3) != 2 || p->maxMass <= 0.0f) continue;
      if (std::fabs(p->viscosity - p->maxMass / 8.0f) < 0.01f) continue;
      other = i;
      break;
    }
    if (other >= 0) {
      const Element* p = t.At(other);
      printf("  second liquid: idx %d hash %d visc %.3f maxMass %.2f (maxMass/8 = %.3f)\n",
             other, p->id, p->viscosity, p->maxMass, p->maxMass / 8.0f);
      World w = SolidBlock(t, 16, 12);
      for (int x = 2; x <= 13; ++x) w.Set(x, 5, vacuum, 0.0f, 300.0f);
      for (int x = 2; x <= 13; ++x) w.Set(x, 6, vacuum, 0.0f, 300.0f);
      w.Set(2, 5, static_cast<uint16_t>(other), p->maxMass, 300.0f);
      RunFlow(dll, t, "second liquid spreading, seeded at its own maxMass", w,
              {{2, 5}, {3, 5}, {4, 5}}, 8);
    }
  }

  // --- the horizontal dead zone. `minHorizontalFlow` is 0.01 for water; walk a small
  // difference down and find where transfer stops, the way the conduction dead zone was
  // found.
  for (float delta : {1.0f, 0.1f, 0.02f, 0.01f, 0.005f}) {
    World w = SolidBlock(t, 12, 12);
    w.Set(5, 5, water, 500.0f + delta, 300.0f);
    w.Set(6, 5, water, 500.0f, 300.0f);
    char label[96];
    snprintf(label, sizeof(label), "liquid pair, horizontal, difference %.4f kg", delta);
    RunFlow(dll, t, label, w, {{5, 5}, {6, 5}}, 4);
  }

  // --- vertical liquid on its own. Two cells, one above the other, both partly full: how
  // much sinks per frame, and does it depend on the amount or on how full the cell is?
  for (float below : {0.0f, 200.0f, 900.0f, 1000.0f}) {
    World w = SolidBlock(t, 12, 12);
    w.Set(5, 5, water, below, 300.0f);
    if (below <= 0.0f) w.element[w.Cell(5, 5)] = vacuum;
    w.Set(5, 6, water, 500.0f, 300.0f);
    char label[112];
    snprintf(label, sizeof(label), "liquid vertical: %.0f kg below, 500 kg above", below);
    RunFlow(dll, t, label, w, {{5, 5}, {5, 6}}, 6);
  }
}

// Round three. Two things round two left open, plus one it called into question.
//
// The dt sweep is the important one. Conduction was written as `dQ = k * dT * dt`, but if
// the sim actually runs a fixed 0.2 s substep and simply runs *more of them* for a longer
// frame, then that dt is a constant wearing a variable's clothes and the kernel is wrong
// for any frame that is not 0.2 s. Round two already showed dt = 0.1 and dt = 0.2 giving
// identical results, which a true rate cannot do.
void ProbeFlow3(const Tables& t, const char* dll) {
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t copper = static_cast<uint16_t>(t.IndexOf(kCopper));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));

  // --- how many substeps does a frame run?
  for (float dt : {0.05f, 0.15f, 0.2f, 0.3f, 0.6f, 1.0f}) {
    World w = SolidBlock(t, 12, 12);
    w.Set(5, 5, oxygen, 2.0f, 300.0f);
    w.Set(6, 5, vacuum, 0.0f, 300.0f);
    char label[96];
    snprintf(label, sizeof(label), "substep count: gas into vacuum, dt = %.2f s", dt);
    RunFlow(dll, t, label, w, {{5, 5}, {6, 5}}, 3, dt);
  }
  // The same question asked of conduction, which is currently implemented as if dt were a
  // real rate. Two solid cells, so nothing can move and only heat crosses.
  for (float dt : {0.1f, 0.2f, 0.4f}) {
    Backend s;
    if (!s.Bind(dll, "dt")) return;
    World w;
    w.Init(12, 12, static_cast<uint16_t>(t.IndexOf(kVacuum)), 0.0f, 300.0f);
    w.Set(5, 5, granite, 1000.0f, 400.0f);
    w.Set(6, 5, copper, 1000.0f, 300.0f);
    const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
    std::vector<uint8_t> visible(w.Count(), 1);
    printf("  conduction at dt = %.2f s: granite 400 K | copper 300 K\n", dt);
    for (int i = 0; i <= 3; ++i) {
      printf("    tick %d  A %11.6f  B %11.6f\n", i, g->temperature[w.Cell(5, 5)],
             g->temperature[w.Cell(6, 5)]);
      const GameDataUpdate* n = Tick(&s, w, &visible, dt);
      if (!n) break;
      g = n;
    }
    s.shutdown();
  }

  // --- vertical liquid, over the whole range of how full the cell below is. Round two
  // found min(mass above, room below / 2, viscosity) for a partly filled cell below, but
  // a column of cells all at maxMass still moved 5 kg per frame, which that rule says is
  // impossible. One of the two observations is incomplete.
  const struct { float below, above; const char* note; } pairs[] = {
      {1000.0f, 1000.0f, "both full - the case the column showed moving"},
      {1000.0f, 1200.0f, "below full, above over maxMass"},
      {1000.0f, 1001.0f, "below full, above barely over"},
      {700.0f, 500.0f, "room 300, expect min(500, 150, 125) = 125"},
      {950.0f, 500.0f, "room 50, expect min(500, 25, 125) = 25"},
      {900.0f, 20.0f, "above has less than the cap"},
  };
  for (const auto& p : pairs) {
    World w = SolidBlock(t, 12, 12);
    w.Set(5, 5, water, p.below, 300.0f);
    w.Set(5, 6, water, p.above, 300.0f);
    char label[160];
    snprintf(label, sizeof(label), "vertical %.0f below / %.0f above - %s", p.below,
             p.above, p.note);
    RunFlow(dll, t, label, w, {{5, 5}, {5, 6}}, 5);
  }
  // A three-cell column at maxMass, every cell watched. The two-cell probe and the
  // nine-cell column disagree, so the smallest case that reproduces the disagreement is
  // the one to look at.
  {
    World w = SolidBlock(t, 12, 12);
    for (int y = 4; y <= 6; ++y) w.Set(5, y, water, 1000.0f, 300.0f);
    RunFlow(dll, t, "three-cell column, all at maxMass", w, {{5, 4}, {5, 5}, {5, 6}}, 8);
  }
  // And a four-cell one, to see whether the transfer per frame depends on how much is
  // stacked above rather than on the pair.
  {
    World w = SolidBlock(t, 12, 14);
    for (int y = 4; y <= 7; ++y) w.Set(5, y, water, 1000.0f, 300.0f);
    RunFlow(dll, t, "four-cell column, all at maxMass", w,
            {{5, 4}, {5, 5}, {5, 6}, {5, 7}}, 8);
  }

  // --- a second gas that is actually stable at 300 K. Round two picked one by flow value
  // alone and it condensed on the first frame, which made the probe measure a phase
  // change instead of flow.
  {
    int32_t other = -1;
    const Element* ox = t.At(t.IndexOf(kOxygen));
    for (int32_t i = 0; i < t.count; ++i) {
      const Element* p = t.At(i);
      if ((p->state & 3) != 1 || p->maxMass <= 0.0f) continue;
      if (p->lowTemp >= 290.0f || p->highTemp <= 310.0f) continue;  // stable at 300 K
      if (p->flow == ox->flow || p->molarMass == ox->molarMass) continue;
      other = i;
      break;
    }
    if (other < 0) {
      printf("  no second gas is stable at 300 K with a different flow\n");
    } else {
      const Element* p = t.At(other);
      printf("  second gas: idx %d hash %d flow %.3f molar %.3f lowTemp %.1f"
             " highTemp %.1f\n", other, p->id, p->flow, p->molarMass, p->lowTemp,
             p->highTemp);
      World w = SolidBlock(t, 12, 12);
      w.Set(5, 5, oxygen, 2.0f, 300.0f);
      w.Set(6, 5, static_cast<uint16_t>(other), 1.0f, 300.0f);
      RunFlow(dll, t, "two stable gases, horizontal: oxygen 2.0 | other 1.0", w,
              {{5, 5}, {6, 5}}, 8);
      World v = SolidBlock(t, 12, 12);
      v.Set(5, 5, oxygen, 1.0f, 300.0f);
      v.Set(5, 6, static_cast<uint16_t>(other), 1.0f, 300.0f);
      RunFlow(dll, t, "two stable gases, equal mass, vertical (stratification?)", v,
              {{5, 5}, {5, 6}}, 8);
    }
  }
}

// Round four: the cases a real base is actually made of, plus the two caps round three
// could not reach.
void ProbeFlow4(const Tables& t, const char* dll) {
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));

  // Is the pressure branch capped by viscosity? Round three's largest pressure transfer
  // was 106 kg against a 125 kg cap, so it never reached it.
  {
    World w = SolidBlock(t, 12, 12);
    w.Set(5, 5, water, 1000.0f, 300.0f);
    w.Set(5, 6, water, 1400.0f, 300.0f);
    RunFlow(dll, t, "pressure branch vs viscosity: 1000 below / 1400 above"
                    " (uncapped would be 207)", w, {{5, 5}, {5, 6}}, 4);
  }
  // The vertical dead zone. minVerticalFlow is 0.01 for water and the horizontal one
  // turned out to be a minimum on the *transfer*, not on the difference.
  for (float above : {1000.004f, 1000.02f, 1000.1f}) {
    World w = SolidBlock(t, 12, 12);
    w.Set(5, 5, water, 1000.0f, 300.0f);
    w.Set(5, 6, water, above, 300.0f);
    char label[128];
    snprintf(label, sizeof(label), "vertical dead zone: 1000 below / %.3f above"
             " (transfer would be %.4f)", above, (above * 1.01f - 1000.0f) / 2.0f);
    RunFlow(dll, t, label, w, {{5, 5}, {5, 6}}, 4);
  }

  // --- water falling through gas. This is the case the whole game is made of and none of
  // the probes so far cover it: every drop of water in a real base falls through oxygen,
  // not through vacuum.
  {
    World w = SolidBlock(t, 12, 14);
    for (int y = 2; y <= 10; ++y) w.Set(5, y, oxygen, 1.0f, 300.0f);
    w.Set(5, 10, water, 1000.0f, 300.0f);
    RunFlow(dll, t, "1000 kg water falling down a shaft full of oxygen", w,
            {{5, 10}, {5, 9}, {5, 8}, {5, 3}, {5, 2}}, 12);
  }
  // A partial drop, which cannot fill the cell it lands in and so has to share it with
  // the gas that was already there — except it cannot, one element per cell.
  {
    World w = SolidBlock(t, 12, 14);
    for (int y = 2; y <= 10; ++y) w.Set(5, y, oxygen, 1.0f, 300.0f);
    w.Set(5, 10, water, 30.0f, 300.0f);
    RunFlow(dll, t, "30 kg water falling through oxygen", w,
            {{5, 10}, {5, 9}, {5, 8}, {5, 3}, {5, 2}}, 12);
  }
  // Two different liquids side by side, the liquid version of the frozen-gas result.
  {
    int32_t other = -1;
    for (int32_t i = 0; i < t.count; ++i) {
      const Element* p = t.At(i);
      if ((p->state & 3) != 2 || p->maxMass <= 0.0f) continue;
      if (p->lowTemp >= 290.0f || p->highTemp <= 310.0f) continue;
      if (p->id == kWater) continue;
      other = i;
      break;
    }
    if (other >= 0) {
      printf("  second liquid stable at 300 K: idx %d hash %d\n", other, t.At(other)->id);
      World w = SolidBlock(t, 12, 12);
      w.Set(5, 5, water, 800.0f, 300.0f);
      w.Set(6, 5, static_cast<uint16_t>(other), 200.0f, 300.0f);
      RunFlow(dll, t, "two different liquids, horizontal: 800 | 200", w,
              {{5, 5}, {6, 5}}, 6);
      World v = SolidBlock(t, 12, 12);
      v.Set(5, 5, water, 500.0f, 300.0f);
      v.Set(5, 6, static_cast<uint16_t>(other), 500.0f, 300.0f);
      RunFlow(dll, t, "two different liquids, vertical: water below, other above", v,
              {{5, 5}, {5, 6}}, 6);
    }
  }
  // Gas above liquid: does the gas get displaced upward as the liquid settles?
  {
    World w = SolidBlock(t, 12, 12);
    w.Set(5, 4, water, 500.0f, 300.0f);
    w.Set(5, 5, oxygen, 1.0f, 300.0f);
    w.Set(5, 6, water, 500.0f, 300.0f);
    RunFlow(dll, t, "water / oxygen / water stack - can the water merge through the gas?",
            w, {{5, 4}, {5, 5}, {5, 6}}, 8);
  }
  // Two gases with a vacuum cell between them, to find out whether gases ever mix at all
  // or only ever fill empty space.
  {
    int32_t other = -1;
    for (int32_t i = 0; i < t.count; ++i) {
      const Element* p = t.At(i);
      if ((p->state & 3) != 1 || p->maxMass <= 0.0f) continue;
      if (p->lowTemp >= 290.0f || p->highTemp <= 310.0f) continue;
      if (p->id == kOxygen) continue;
      other = i;
      break;
    }
    if (other >= 0) {
      World w = SolidBlock(t, 12, 12);
      w.Set(4, 5, oxygen, 2.0f, 300.0f);
      w.Set(5, 5, vacuum, 0.0f, 300.0f);
      w.Set(6, 5, static_cast<uint16_t>(other), 2.0f, 300.0f);
      RunFlow(dll, t, "two gases with one empty cell between them", w,
              {{4, 5}, {5, 5}, {6, 5}}, 10);
    }
  }
}

// Round five. Vertical swapping between two *different* elements is the one rule left,
// and it is the one that makes oil float on water. Round four showed a swap happening
// between two liquids of equal mass, in the direction that maxMass alone does not predict,
// so the ordering key has to be measured rather than assumed.
void ProbeFlow5(const Tables& t, const char* dll) {
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));

  int32_t liq2 = -1, gas2 = -1;
  for (int32_t i = 0; i < t.count; ++i) {
    const Element* p = t.At(i);
    if (p->maxMass <= 0.0f || p->lowTemp >= 290.0f || p->highTemp <= 310.0f) continue;
    if ((p->state & 3) == 2 && p->id != kWater && liq2 < 0) liq2 = i;
    if ((p->state & 3) == 1 && p->id != kOxygen && gas2 < 0) gas2 = i;
  }
  if (liq2 < 0 || gas2 < 0) {
    printf("  could not find a second stable liquid and gas\n");
    return;
  }
  const Element* wp = t.At(t.IndexOf(kWater));
  const Element* lp = t.At(liq2);
  const Element* op = t.At(t.IndexOf(kOxygen));
  const Element* gp = t.At(gas2);
  printf("  water   maxMass %8.2f molar %8.3f  |  liquid2 idx %d maxMass %8.2f"
         " molar %8.3f\n", wp->maxMass, wp->molarMass, liq2, lp->maxMass, lp->molarMass);
  printf("  oxygen  maxMass %8.2f molar %8.3f  |  gas2    idx %d maxMass %8.2f"
         " molar %8.3f\n", op->maxMass, op->molarMass, gas2, gp->maxMass, gp->molarMass);

  // Two liquids, varying how full each cell is. If the key is mass, the heavier cell
  // sinks; if it is the fill fraction mass/maxMass, the fuller one does. These disagree
  // whenever the two maxMasses differ, which is the point of the pairing.
  const struct { float below, above; } liq[] = {
      {500.0f, 500.0f}, {900.0f, 100.0f}, {100.0f, 900.0f}, {500.0f, 400.0f},
  };
  for (const auto& p : liq) {
    World w = SolidBlock(t, 12, 12);
    w.Set(5, 5, water, p.below, 300.0f);
    w.Set(5, 6, static_cast<uint16_t>(liq2), p.above, 300.0f);
    char label[144];
    snprintf(label, sizeof(label),
             "liquids: water %.0f below (frac %.3f) / liquid2 %.0f above (frac %.3f)",
             p.below, p.below / wp->maxMass, p.above, p.above / lp->maxMass);
    RunFlow(dll, t, label, w, {{5, 5}, {5, 6}}, 4);
  }
  // Gases, same question. Equal masses did not swap in round three even with the heavier
  // molar mass on top, so either gases do not sort or the key is something else.
  const struct { float below, above; } gas[] = {
      {1.0f, 2.0f}, {0.5f, 1.5f}, {1.5f, 0.5f},
  };
  for (const auto& p : gas) {
    World w = SolidBlock(t, 12, 12);
    w.Set(5, 5, oxygen, p.below, 300.0f);
    w.Set(5, 6, static_cast<uint16_t>(gas2), p.above, 300.0f);
    char label[144];
    snprintf(label, sizeof(label), "gases: oxygen %.2f below / gas2 %.2f above", p.below,
             p.above);
    RunFlow(dll, t, label, w, {{5, 5}, {5, 6}}, 4);
  }
  // A liquid sitting under a gas is the one ordering the game never gets wrong, so it is
  // the control: it must not swap.
  {
    World w = SolidBlock(t, 12, 12);
    w.Set(5, 5, water, 500.0f, 300.0f);
    w.Set(5, 6, oxygen, 1.0f, 300.0f);
    RunFlow(dll, t, "control: water below, oxygen above - must not swap", w,
            {{5, 5}, {5, 6}}, 4);
  }
  // Water spreading sideways into a cell that already holds gas. Every puddle in a real
  // base does this, and none of the probes so far cover it: the spread probe used a
  // vacuum channel, where there is no other element to get in the way.
  {
    World w = SolidBlock(t, 16, 12);
    for (int x = 2; x <= 8; ++x) w.Set(x, 5, oxygen, 1.0f, 300.0f);
    w.Set(2, 5, water, 1000.0f, 300.0f);
    RunFlow(dll, t, "water spreading sideways into oxygen", w,
            {{2, 5}, {3, 5}, {4, 5}, {5, 5}}, 10);
  }
  // A wide, one-cell-tall sheet of water sitting on gas. In a 4-wide slug Klei dropped
  // alternate columns — the 1st and 3rd fell through, the 2nd and 4th stayed — and the
  // pattern was identical on three runs and on a second copy of the DLL, so it is a rule
  // and not the nondeterminism. Six cells says whether it is strict parity.
  {
    World w = SolidBlock(t, 16, 12);
    for (int y = 3; y <= 7; ++y) {
      for (int x = 2; x <= 13; ++x) w.Set(x, y, oxygen, 1.0f, 300.0f);
    }
    for (int x = 4; x <= 9; ++x) w.Set(x, 6, water, 900.0f, 300.0f);
    RunFlow(dll, t, "six-wide water sheet on gas: which columns fall?", w,
            {{4, 5}, {5, 5}, {6, 5}, {7, 5}, {8, 5}, {9, 5}}, 4);
    World v = SolidBlock(t, 16, 12);
    for (int y = 3; y <= 7; ++y) {
      for (int x = 2; x <= 13; ++x) v.Set(x, y, oxygen, 1.0f, 300.0f);
    }
    // Same sheet shifted one cell right. If the answer is grid parity the fallen columns
    // stay on the same absolute x; if it is "skip the one after a fall", they shift too.
    for (int x = 5; x <= 10; ++x) v.Set(x, 6, water, 900.0f, 300.0f);
    RunFlow(dll, t, "same sheet shifted one cell right", v,
            {{5, 5}, {6, 5}, {7, 5}, {8, 5}, {9, 5}, {10, 5}}, 4);
  }
  // And the reverse: gas meeting liquid sideways.
  {
    World w = SolidBlock(t, 16, 12);
    for (int x = 2; x <= 8; ++x) w.Set(x, 5, oxygen, 2.0f, 300.0f);
    w.Set(6, 5, water, 500.0f, 300.0f);
    RunFlow(dll, t, "gas meeting a liquid cell sideways", w,
            {{4, 5}, {5, 5}, {6, 5}, {7, 5}}, 8);
  }
}

// Conduction involving a fluid, measured properly.
//
// The isolated-pair trick does not work here: a gas or liquid cell surrounded by vacuum
// flows away before it can be read, which is what made the first attempt at this produce
// a water cell that had emptied itself and a "pair" that was no longer a pair. So the
// fluid is sealed in solid granite instead, and the granite is held at a uniform
// temperature so that every neighbour of the fluid cell is identical.
//
// One fluid cell with four identical solid neighbours gives, on the first transfer frame,
//     dQ_per_pair = C_fluid * dT_fluid / 4
// and the implied pair conductivity follows.
void ProbeFluidConduction(const Tables& t, const char* dll) {
  const int32_t gi = t.IndexOf(kGranite);
  auto one_cell = [&](const char* label, int32_t elem, float mass, float temp,
                      int neighbours) {
    Backend s;
    if (!s.Bind(dll, "fluid")) return;
    World w;
    w.Init(12, 12, static_cast<uint16_t>(gi), 1000.0f, 400.0f);
    w.Set(5, 5, static_cast<uint16_t>(elem), mass, temp);
    const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
    if (!g) {
      s.shutdown();
      return;
    }
    std::vector<uint8_t> visible(w.Count(), 1);
    const int c = w.Cell(5, 5);
    const double shc = t.At(elem)->specificHeatCapacity;
    const double ka = t.At(gi)->thermalConductivity, kb = t.At(elem)->thermalConductivity;
    printf("  %s\n", label);
    printf("    k %.4f (granite %.4f)  shc %.4f  mass %.2f -> C %.4f kJ/K\n", kb, ka, shc,
           mass, mass * shc);
    double prev = g->temperature[c];
    for (int i = 0; i <= 3; ++i) {
      const double now = g->temperature[c];
      if (i > 0) {
        const double dq = (now - prev) * mass * shc;
        const double dt_start = 400.0 - prev;
        printf("    tick %d  T %11.5f  mass %8.3f  dQ %10.4f kJ  per pair %9.4f"
               "  k_eff %9.4f  sqrt(ka*kb) %8.4f  ratio %8.4f\n",
               i, now, g->mass[c], dq, dq / neighbours,
               dt_start > 0 ? dq / neighbours / (0.2 * dt_start) : 0.0, std::sqrt(ka * kb),
               dt_start > 0 ? (dq / neighbours / (0.2 * dt_start)) / std::sqrt(ka * kb)
                            : 0.0);
      } else {
        printf("    tick %d  T %11.5f  mass %8.3f\n", i, now, g->mass[c]);
      }
      prev = now;
      const GameDataUpdate* n = Tick(&s, w, &visible);
      if (!n) break;
      g = n;
    }
    s.shutdown();
  };

  const int32_t oi = t.IndexOf(kOxygen), wi = t.IndexOf(kWater), ci = t.IndexOf(kCopper);
  // Vary the mass, because conduction between solids was completely mass-independent and
  // the same claim has to be tested here rather than assumed.
  one_cell("one oxygen cell (1.0 kg) sealed in 400 K granite", oi, 1.0f, 300.0f, 4);
  one_cell("one oxygen cell (0.5 kg) sealed in 400 K granite", oi, 0.5f, 300.0f, 4);
  one_cell("one water cell (1000 kg) sealed in 400 K granite", wi, 1000.0f, 300.0f, 4);
  one_cell("one water cell (250 kg) sealed in 400 K granite", wi, 250.0f, 300.0f, 4);
  // A solid control through the same probe, so the two paths can be compared directly.
  one_cell("one copper cell sealed in 400 K granite (solid control)", ci, 1000.0f, 300.0f,
           4);
  // A heavy gas cell, so the transfer is not large enough to be clamped and the raw rate
  // can be read. At 1 kg the oxygen jumps the whole 100 K in one frame, which says only
  // "at least this fast".
  one_cell("one oxygen cell (100 kg) sealed in 400 K granite", oi, 100.0f, 300.0f, 4);

  // Only *one* hot neighbour, so a single pair is active and the clamping question is
  // answerable: the four-neighbour probe cannot tell a per-pair limit from a per-cell one.
  auto single_pair = [&](const char* label, int32_t elem, float mass) {
    Backend s;
    if (!s.Bind(dll, "single")) return;
    World w;
    w.Init(12, 12, static_cast<uint16_t>(gi), 1000.0f, 300.0f);
    w.Set(5, 6, static_cast<uint16_t>(gi), 1000.0f, 400.0f);  // the one hot neighbour
    w.Set(5, 5, static_cast<uint16_t>(elem), mass, 300.0f);
    const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
    if (!g) {
      s.shutdown();
      return;
    }
    std::vector<uint8_t> visible(w.Count(), 1);
    const int c = w.Cell(5, 5);
    const double shc = t.At(elem)->specificHeatCapacity;
    const double kb = t.At(elem)->thermalConductivity;
    const double sam = t.At(elem)->solidSurfaceAreaMultiplier;
    const double k_plain = std::sqrt(t.At(gi)->thermalConductivity * kb);
    const double k_sam = std::sqrt(t.At(gi)->thermalConductivity * kb * sam);
    printf("  %s\n", label);
    printf("    predicted dT: no SAM %.4f K, SAM %.1f %.4f K, pair equilibrium %.4f K\n",
           k_plain * 100.0 * 0.2 / (mass * shc), sam, k_sam * 100.0 * 0.2 / (mass * shc),
           (400.0 * 790.0 + 300.0 * mass * shc) / (790.0 + mass * shc));
    for (int i = 0; i <= 2; ++i) {
      printf("    tick %d  T %11.5f  mass %8.3f  hot neighbour %11.5f\n", i,
             g->temperature[c], g->mass[c], g->temperature[w.Cell(5, 6)]);
      const GameDataUpdate* n = Tick(&s, w, &visible);
      if (!n) break;
      g = n;
    }
    s.shutdown();
  };
  single_pair("oxygen 1 kg, one hot granite neighbour", oi, 1.0f);
  single_pair("oxygen 100 kg, one hot granite neighbour", oi, 100.0f);
  single_pair("water 1000 kg, one hot granite neighbour", wi, 1000.0f);

  // Fluid touching fluid. A pocket cannot simply be filled uniformly, because liquid at a
  // uniform mass immediately starts flowing; seeding the column with the 1% gradient it
  // settles at makes it stationary, so conduction can be read on its own.
  auto pocket = [&](const char* label, int32_t elem, float base, bool gradient,
                    float hot) {
    Backend s;
    if (!s.Bind(dll, "pocket")) return;
    World w;
    w.Init(12, 12, static_cast<uint16_t>(gi), 1000.0f, 300.0f);
    for (int y = 4; y <= 6; ++y) {
      const float m = gradient ? base * std::pow(1.01f, static_cast<float>(6 - y)) : base;
      for (int x = 4; x <= 6; ++x) w.Set(x, y, static_cast<uint16_t>(elem), m, 300.0f);
    }
    w.temperature[w.Cell(5, 5)] = hot;   // only the centre is hot
    const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
    if (!g) {
      s.shutdown();
      return;
    }
    std::vector<uint8_t> visible(w.Count(), 1);
    const int c = w.Cell(5, 5);
    const double shc = t.At(elem)->specificHeatCapacity;
    const double k = t.At(elem)->thermalConductivity;
    const double sam = (t.At(elem)->state & 3) == 2 ? t.At(elem)->liquidSurfaceAreaMultiplier
                                                    : t.At(elem)->gasSurfaceAreaMultiplier;
    printf("  %s\n", label);
    printf("    centre has 4 identical neighbours of its own element; k %.4f, own-phase"
           " SAM %.1f\n", k, sam);
    const double d0 = hot - 300.0;
    printf("    predicted centre dT over 4 pairs: SAM 1 %.5f K, SAM %.0f %.5f K,"
           " SAM %.0f (squared) %.5f K\n",
           4 * k * d0 * 0.2 / (base * shc), sam, 4 * k * sam * d0 * 0.2 / (base * shc),
           sam * sam, 4 * k * sam * sam * d0 * 0.2 / (base * shc));
    for (int i = 0; i <= 2; ++i) {
      printf("    tick %d  centre T %11.5f mass %9.3f | up %11.5f | right %11.5f\n", i,
             g->temperature[c], g->mass[c], g->temperature[w.Cell(5, 6)],
             g->temperature[w.Cell(6, 5)]);
      const GameDataUpdate* n = Tick(&s, w, &visible);
      if (!n) break;
      g = n;
    }
    s.shutdown();
  };
  pocket("3x3 oxygen pocket, centre hot (gas touching gas)", oi, 1.0f, false, 400.0f);
  // 340 K, not 400: water boils at 372.15 and the first attempt at this measured a phase
  // change instead of conduction, with the centre cell's mass collapsing to 5 kg.
  pocket("3x3 water pocket seeded with the 1% gradient (liquid touching liquid)", wi,
         1000.0f, true, 340.0f);
}

void ProbeConductionPairs(Backend*, const Tables& t, const char* dll) {
  const int32_t gi = t.IndexOf(kGranite), ci = t.IndexOf(kCopper),
                si = t.IndexOf(kSandStone);
  auto run = [&](const char* label, int32_t ea, float ma, float ta, int32_t eb, float mb,
                 float tb) {
    Backend k;
    if (!k.Bind(dll, "pair")) return;
    ProbePair(&k, t, label, ea, ma, ta, eb, mb, tb, 4);
  };
  run("granite/granite, both 1000 kg", gi, 1000.0f, 400.0f, gi, 1000.0f, 300.0f);
  run("granite/granite, B at 2000 kg", gi, 1000.0f, 400.0f, gi, 2000.0f, 300.0f);
  run("granite/granite, B at 250 kg", gi, 1000.0f, 400.0f, gi, 250.0f, 300.0f);
  run("granite/copper  (k 3.39 vs 60)", gi, 1000.0f, 400.0f, ci, 1000.0f, 300.0f);
  run("granite/sandstone (3.39 vs 2.9)", gi, 1000.0f, 400.0f, si, 1000.0f, 300.0f);
  run("copper/copper (k 60)", ci, 1000.0f, 400.0f, ci, 1000.0f, 300.0f);
  // Same-element pairs conduct at a 100 K difference but visibly do not at a fraction of
  // a degree, so there is a threshold somewhere. Walk it down until transfer stops.
  // Fluids. The first conduction pass left these alone because every solid measured has
  // surface-area multipliers of exactly 1.0, so they could not show up — but water carries
  // liquidSurfaceAreaMultiplier 25 and oxygen solidSurfaceAreaMultiplier 25, and a sealed
  // gas pocket in granite showed Klei plainly conducting between the two. The multiplier
  // has to come out of these.
  const int32_t oi = t.IndexOf(kOxygen), wi = t.IndexOf(kWater);
  if (oi >= 0 && wi >= 0) {
    const Element* op = t.At(oi);
    const Element* wp = t.At(wi);
    printf("  oxygen: k %.4f shc %.4f  SAM solid/liquid/gas %.1f/%.1f/%.1f\n",
           op->thermalConductivity, op->specificHeatCapacity,
           op->solidSurfaceAreaMultiplier, op->liquidSurfaceAreaMultiplier,
           op->gasSurfaceAreaMultiplier);
    printf("  water:  k %.4f shc %.4f  SAM solid/liquid/gas %.1f/%.1f/%.1f\n",
           wp->thermalConductivity, wp->specificHeatCapacity,
           wp->solidSurfaceAreaMultiplier, wp->liquidSurfaceAreaMultiplier,
           wp->gasSurfaceAreaMultiplier);
    printf("  granite SAM solid/liquid/gas %.1f/%.1f/%.1f\n",
           t.At(gi)->solidSurfaceAreaMultiplier, t.At(gi)->liquidSurfaceAreaMultiplier,
           t.At(gi)->gasSurfaceAreaMultiplier);
    // A gas pocket cannot be isolated in vacuum without it flowing away, so these pairs
    // are read on the first transfer frame only, before flow has moved anything much.
    run("granite/oxygen (gas touching solid)", gi, 1000.0f, 400.0f, oi, 1.0f, 300.0f);
    run("granite/water  (liquid touching solid)", gi, 1000.0f, 400.0f, wi, 1000.0f,
        300.0f);
    run("water/water   (liquid pair)", wi, 1000.0f, 400.0f, wi, 1000.0f, 300.0f);
    run("oxygen/oxygen (gas pair)", oi, 1.0f, 400.0f, oi, 1.0f, 300.0f);
    run("water/oxygen  (liquid touching gas)", wi, 1000.0f, 400.0f, oi, 1.0f, 300.0f);
  }
  run("granite/granite dT = 10", gi, 1000.0f, 310.0f, gi, 1000.0f, 300.0f);
  run("granite/granite dT = 2", gi, 1000.0f, 302.0f, gi, 1000.0f, 300.0f);
  run("granite/granite dT = 1", gi, 1000.0f, 301.0f, gi, 1000.0f, 300.0f);
  run("granite/granite dT = 0.5", gi, 1000.0f, 300.5f, gi, 1000.0f, 300.0f);
  run("granite/granite dT = 0.2", gi, 1000.0f, 300.2f, gi, 1000.0f, 300.0f);
  run("granite/granite dT = 0.05", gi, 1000.0f, 300.05f, gi, 1000.0f, 300.0f);
}

// --------------------------------------------------------------- state changes
//
// The element table carries a low and a high transition for every element, plus an
// optional secondary product with its own mass conversion. None of that says *when* the
// sim applies a transition, what the resulting cell's temperature is, or where the
// secondary product goes, so all three have to be measured.
//
// Every probe here seals the sample inside granite **at the sample's own temperature**, so
// conduction has nothing to do (the pair difference is zero) and the only thing that can
// move the cell is the transition itself.

void PrintTransitionRow(const Tables& t, const char* label, int32_t idx) {
  if (idx < 0) return;
  const Element* p = t.At(idx);
  auto name_of = [&](uint16_t i) -> int32_t {
    return i < t.count ? t.At(i)->id : 0;
  };
  printf("  %-14s idx %3d hash %11d state %u shc %7.4f\n", label, idx, p->id, p->state,
         p->specificHeatCapacity);
  printf("      lowTemp  %9.3f -> idx %3d (hash %11d, state %u)"
         "  ore %11d x %.4f\n",
         p->lowTemp, p->lowTempTransitionIdx, name_of(p->lowTempTransitionIdx),
         p->lowTempTransitionIdx < t.count ? t.At(p->lowTempTransitionIdx)->state : 0,
         p->lowTempTransitionOreID, p->lowTempTransitionOreMassConversion);
  printf("      highTemp %9.3f -> idx %3d (hash %11d, state %u)"
         "  ore %11d x %.4f\n",
         p->highTemp, p->highTempTransitionIdx, name_of(p->highTempTransitionIdx),
         p->highTempTransitionIdx < t.count ? t.At(p->highTempTransitionIdx)->state : 0,
         p->highTempTransitionOreID, p->highTempTransitionOreMassConversion);
  printf("      sublimate idx %3d rate %.5f eff %.4f prob %.4f  offGas %.4f  convert %3d\n",
         p->sublimateIndex, p->sublimateRate, p->sublimateEfficiency,
         p->sublimateProbability, p->offGasProbability, p->convertIndex);
}

void ProbeStateParams(const Tables& t) {
  const int32_t wi = t.IndexOf(kWater);
  PrintTransitionRow(t, "Water", wi);
  if (wi >= 0) {
    PrintTransitionRow(t, "  -> high", t.At(wi)->highTempTransitionIdx);
    PrintTransitionRow(t, "  -> low", t.At(wi)->lowTempTransitionIdx);
  }
  PrintTransitionRow(t, "Granite", t.IndexOf(kGranite));
  PrintTransitionRow(t, "Oxygen", t.IndexOf(kOxygen));
  // Vacuum is here because the backwall transition announcement refuses it even at 20000 K,
  // and its row is what says whether that is a range test or a missing transition target.
  PrintTransitionRow(t, "Vacuum", t.IndexOf(kVacuum));

  // Every element that produces a *secondary* element on transition. This is the only
  // mechanism in the table that can create two substances from one, so the list is short
  // and worth having in full.
  printf("  elements with a transition ore:\n");
  for (int32_t i = 0; i < t.count; ++i) {
    const Element* p = t.At(i);
    if (p->lowTempTransitionOreID == 0 && p->highTempTransitionOreID == 0) continue;
    printf("    idx %3d hash %11d state %u  low %8.2f ore %11d x %.4f"
           "  high %8.2f ore %11d x %.4f\n",
           i, p->id, p->state, p->lowTemp, p->lowTempTransitionOreID,
           p->lowTempTransitionOreMassConversion, p->highTemp, p->highTempTransitionOreID,
           p->highTempTransitionOreMassConversion);
  }
  // Sublimating elements, which transition on a *probability* rather than a temperature.
  // 65535, not 0, is "no sublimation" — index 0 is a real element. Filtering on != 0
  // matched the whole table and made every element look like it sublimated.
  printf("  elements that sublimate (sublimateIndex != 65535):\n");
  int shown = 0;
  for (int32_t i = 0; i < t.count && shown < 16; ++i) {
    const Element* p = t.At(i);
    if (p->sublimateIndex == 0xFFFF) continue;
    ++shown;
    printf("    idx %3d hash %11d state %u -> %3d  rate %.5f eff %.4f prob %.4f\n", i,
           p->id, p->state, p->sublimateIndex, p->sublimateRate, p->sublimateEfficiency,
           p->sublimateProbability);
  }
}

// Run one transition experiment and print element, mass and temperature for the watched
// cells, plus every event the frame produced. Events matter as much as the cell state
// here: `Grid.Element[]` is a managed cache and a transition that does not announce
// itself is a permanently stale cell.
void RunState(const char* dll, const Tables& t, const char* label, const World& w,
              const std::vector<std::pair<int, int>>& watch, int ticks,
              float dt = 0.2f) {
  Backend s;
  if (!s.Bind(dll, "state")) return;
  const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
  if (!g) {
    printf("  %s: boot failed\n", label);
    s.shutdown();
    return;
  }
  std::vector<uint8_t> visible(w.Count(), 1);

  printf("  %s\n", label);
  printf("    %-5s", "tick");
  for (const auto& p : watch) {
    char h[24];
    snprintf(h, sizeof(h), "(%d,%d) e/kg/K", p.first, p.second);
    printf(" %26s", h);
  }
  printf("  %14s %14s\n", "pocket kg", "pocket kJ");

  for (int i = 0; i <= ticks; ++i) {
    // Only non-solid mass is summed: the granite apparatus outweighs the sample by orders
    // of magnitude and would hide the whole transition. Energy uses the *current*
    // element's specific heat, which is the point — if the sim conserves energy across a
    // transition this number is flat, and if it conserves temperature it jumps.
    double mass = 0, energy = 0;
    for (size_t c = 0; c < w.Count(); ++c) {
      const Element* e = t.At(g->elementIdx[c]);
      if ((e->state & 3) == 3) continue;
      mass += g->mass[c];
      energy += static_cast<double>(g->mass[c]) * e->specificHeatCapacity *
                g->temperature[c];
    }
    printf("    %-5d", i);
    for (const auto& p : watch) {
      const int c = w.Cell(p.first, p.second);
      printf(" %5u/%9.4f/%9.5f", g->elementIdx[c], g->mass[c], g->temperature[c]);
    }
    printf("  %14.5f %14.4f", mass, energy);
    if (g->numSubstanceChangeInfo) {
      printf("  sub=%d", g->numSubstanceChangeInfo);
      for (int k = 0; k < g->numSubstanceChangeInfo && k < 3; ++k) {
        const SubstanceChangeInfo& s2 = g->substanceChangeInfo[k];
        printf(" [%d:%u->%u]", s2.cellIdx, s2.oldElemIdx, s2.newElemIdx);
      }
    }
    if (g->numSolidSubstanceChangeInfo) printf(" solidsub=%d", g->numSolidSubstanceChangeInfo);
    if (g->numLiquidChangeInfo) printf(" liqchg=%d", g->numLiquidChangeInfo);
    if (g->numSolidInfo) printf(" solid=%d", g->numSolidInfo);
    if (g->numSpawnOreInfo) {
      printf(" ore=%d", g->numSpawnOreInfo);
      for (int k = 0; k < g->numSpawnOreInfo && k < 3; ++k) {
        const SpawnOreInfo& o = g->spawnOreInfo[k];
        printf(" [%d:e%u %.4fkg %.3fK]", o.cellIdx, o.elemIdx, o.mass, o.temperature);
      }
    }
    if (g->numSpawnFXInfo) printf(" fx=%d", g->numSpawnFXInfo);
    if (g->numCellMeltedInfos) printf(" melted=%d", g->numCellMeltedInfos);
    if (g->numSpawnFallingLiquidInfo) printf(" fall=%d", g->numSpawnFallingLiquidInfo);
    printf("\n");
    const GameDataUpdate* n = Tick(&s, w, &visible, dt);
    if (!n) break;
    g = n;
  }
  s.shutdown();
}

void ProbeStates(const Tables& t, const char* dll) {
  const int32_t wi = t.IndexOf(kWater);
  const int32_t gi = t.IndexOf(kGranite);
  if (wi < 0 || gi < 0) {
    printf("  required elements missing\n");
    return;
  }
  const uint16_t water = static_cast<uint16_t>(wi);
  const Element* wp = t.At(wi);
  const float boil = wp->highTemp;
  const float freeze = wp->lowTemp;

  // The apparatus: granite at the same temperature as the sample, so conduction's 1 K dead
  // zone is not even reached and nothing but the transition can act.
  auto sealed_at = [&](float temp) {
    World w;
    w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, temp);
    return w;
  };

  // --- 1. Where exactly is the boundary? Three runs bracketing highTemp answer both
  // "is the comparison strict" and "is it evaluated against the element's own highTemp".
  for (float d : {-0.05f, 0.0f, 0.05f}) {
    World w = sealed_at(boil + d);
    w.Set(5, 5, water, 1000.0f, boil + d);
    char label[128];
    snprintf(label, sizeof(label), "water 1000 kg at highTemp%+.2f = %.4f K", d, boil + d);
    RunState(dll, t, label, w, {{5, 5}}, 3);
  }

  // --- 2. Well past the boundary, to read the product and what happens to temperature.
  // If the sim conserves energy the new temperature is T * shc_old / shc_new; if it
  // conserves temperature the pocket energy jumps by the ratio of the two heat capacities.
  {
    World w = sealed_at(500.0f);
    w.Set(5, 5, water, 1000.0f, 500.0f);
    RunState(dll, t, "water 1000 kg at 500 K (far above boiling)", w, {{5, 5}}, 4);
  }
  // A small sample, in case the transition is mass-gated or partial.
  {
    World w = sealed_at(500.0f);
    w.Set(5, 5, water, 5.0f, 500.0f);
    RunState(dll, t, "water 5 kg at 500 K", w, {{5, 5}}, 4);
  }
  // --- 3. Freezing, the low-temperature direction, which produces a *solid* and so has to
  // announce solidInfo as well as substanceChangeInfo.
  for (float d : {-0.05f, 0.05f}) {
    World w = sealed_at(freeze + d);
    w.Set(5, 5, water, 1000.0f, freeze + d);
    char label[128];
    snprintf(label, sizeof(label), "water 1000 kg at lowTemp%+.2f = %.4f K", d, freeze + d);
    RunState(dll, t, label, w, {{5, 5}}, 3);
  }
  {
    World w = sealed_at(200.0f);
    w.Set(5, 5, water, 1000.0f, 200.0f);
    RunState(dll, t, "water 1000 kg at 200 K (far below freezing)", w, {{5, 5}}, 4);
  }

  // --- 4. A solid melting. Granite is the apparatus everywhere else, so use a different
  // solid as the sample and keep granite cold enough not to melt with it — which it will
  // not, because the sample is sealed at its own temperature and conduction is dead.
  {
    const Element* gp = t.At(gi);
    World w;
    w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
    w.Set(5, 5, static_cast<uint16_t>(gi), 2000.0f, gp->highTemp + 1.0f);
    char label[160];
    snprintf(label, sizeof(label), "granite 2000 kg at highTemp+1 = %.3f K, cold granite around",
             gp->highTemp + 1.0f);
    RunState(dll, t, label, w, {{5, 5}, {5, 6}, {6, 5}}, 6);
  }

  // --- 5. Ordering. Does the transition run before or after conduction inside a substep?
  // A cell placed just *below* the threshold next to a hot neighbour crosses it only after
  // conduction has run; whether it changes on the same tick or the next says which comes
  // first.
  {
    World w;
    w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, boil - 20.0f);
    w.Set(5, 5, water, 1000.0f, boil - 0.5f);
    w.Set(5, 6, static_cast<uint16_t>(gi), 2000.0f, boil + 400.0f);
    RunState(dll, t, "water just under boiling with one very hot granite neighbour", w,
             {{5, 5}, {5, 6}}, 6);
  }

  // --- 6. A transition that produces a secondary element, to see where the ore goes and
  // how the mass splits. The element is picked from the table rather than hardcoded so
  // this keeps working if the table changes.
  {
    int32_t sample = -1;
    for (int32_t i = 0; i < t.count; ++i) {
      const Element* p = t.At(i);
      if (p->highTempTransitionOreID != 0 && p->highTemp > 0.0f && p->highTemp < 3000.0f) {
        sample = i;
        break;
      }
    }
    if (sample >= 0) {
      const Element* p = t.At(sample);
      World w;
      w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
      w.Set(5, 5, static_cast<uint16_t>(sample), 1000.0f, p->highTemp + 1.0f);
      char label[192];
      snprintf(label, sizeof(label),
               "idx %d (hash %d) 1000 kg at highTemp+1 = %.3f K, ore %d x %.4f", sample,
               p->id, p->highTemp + 1.0f, p->highTempTransitionOreID,
               p->highTempTransitionOreMassConversion);
      RunState(dll, t, label, w, {{5, 5}, {5, 6}, {5, 4}}, 5);
    }
  }

  // --- 7. Sublimation: a probabilistic transition with no temperature threshold. Run it
  // twice on fresh sims — if the two runs agree, the probability is seeded and can be
  // reproduced; if they do not, it cannot be, and that is worth knowing before trying.
  {
    int32_t sample = -1;
    for (int32_t i = 0; i < t.count; ++i) {
      if (t.At(i)->sublimateIndex != 0 && (t.At(i)->state & 3) == 3) {
        sample = i;
        break;
      }
    }
    if (sample >= 0) {
      const Element* p = t.At(sample);
      World w;
      w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
      w.Set(5, 5, static_cast<uint16_t>(sample), 100.0f, 293.15f);
      char label[192];
      snprintf(label, sizeof(label), "sublimation run A: idx %d hash %d rate %.5f prob %.4f",
               sample, p->id, p->sublimateRate, p->sublimateProbability);
      RunState(dll, t, label, w, {{5, 5}, {5, 6}}, 8);
      snprintf(label, sizeof(label), "sublimation run B: same world, fresh sim");
      RunState(dll, t, label, w, {{5, 5}, {5, 6}}, 8);
    }
  }
}

// Round two: the boundary is not `T > highTemp`. Water at 372.55 K, granite at 943 K and
// a third element one degree over its own threshold all sat still for three ticks, while
// the same elements far above their thresholds changed on the first physics frame. So
// there is a margin, and this measures it.
//
// Each run is a fresh sim seeded at one temperature, stepped a few ticks, and reduced to a
// single line: did it change, when, into what, and at what temperature.
struct TransitionResult {
  bool changed = false;
  int tick = -1;
  uint16_t element = 0;
  float before = 0.0f;
  float after = 0.0f;
  float mass_after = 0.0f;
};

TransitionResult RunTransition(const char* dll, const Tables& t, uint16_t element,
                               float mass, float temp, int ticks) {
  TransitionResult r;
  r.element = element;
  Backend s;
  if (!s.Bind(dll, "trans")) return r;
  World w;
  w.Init(12, 12, static_cast<uint16_t>(t.IndexOf(kGranite)), 2000.0f, temp);
  w.Set(5, 5, element, mass, temp);
  const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
  if (!g) {
    s.shutdown();
    return r;
  }
  std::vector<uint8_t> visible(w.Count(), 1);
  const int cell = w.Cell(5, 5);
  r.before = g->temperature[cell];
  for (int i = 1; i <= ticks; ++i) {
    g = Tick(&s, w, &visible);
    if (!g) break;
    if (g->elementIdx[cell] != element) {
      r.changed = true;
      r.tick = i;
      r.element = g->elementIdx[cell];
      r.after = g->temperature[cell];
      r.mass_after = g->mass[cell];
      break;
    }
  }
  if (!r.changed) {
    r.after = g ? g->temperature[cell] : 0.0f;
    r.mass_after = g ? g->mass[cell] : 0.0f;
  }
  s.shutdown();
  return r;
}

void SweepTransition(const char* dll, const Tables& t, const char* label, int32_t idx,
                     float mass, float base, const std::vector<float>& offsets, int ticks) {
  const Element* p = t.At(idx);
  printf("  %s: idx %d, lowTemp %.3f highTemp %.3f, %.1f kg\n", label, idx, p->lowTemp,
         p->highTemp, mass);
  printf("    %-12s %-10s %-6s %-8s %-10s %-10s\n", "seeded K", "offset", "tick", "new e",
         "new K", "delta K");
  for (float d : offsets) {
    const TransitionResult r =
        RunTransition(dll, t, static_cast<uint16_t>(idx), mass, base + d, ticks);
    if (!r.changed) {
      printf("    %-12.4f %-+10.4f %-6s %-8s %-10.4f\n", base + d, d, "-", "-", r.after);
    } else {
      printf("    %-12.4f %-+10.4f %-6d %-8u %-10.5f %-+10.5f\n", base + d, d, r.tick,
             r.element, r.after, r.after - r.before);
    }
  }
}

void ProbeStates2(const Tables& t, const char* dll) {
  const int32_t wi = t.IndexOf(kWater), gi = t.IndexOf(kGranite);
  if (wi < 0 || gi < 0) return;
  const Element* wp = t.At(wi);

  // Which frame does the transition land on? The two probes above disagreed — one reported
  // the change after one tick and the other after two — and that is exactly the kind of
  // off-by-one that would make the replacement lag Klei by a frame forever. Run one world
  // and print the raw element index after every tick, with nothing else in the way.
  printf("  which frame: water 1000 kg at 500 K, 8 identical runs\n");
  for (int run = 0; run < 8; ++run) {
    Backend s;
    if (!s.Bind(dll, "when")) break;
    World w;
    w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 500.0f);
    w.Set(5, 5, static_cast<uint16_t>(wi), 1000.0f, 500.0f);
    const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
    std::vector<uint8_t> visible(w.Count(), 1);
    const int cell = w.Cell(5, 5);
    printf("    run %d: start e %u", run, g->elementIdx[cell]);
    for (int i = 1; i <= 4; ++i) {
      g = Tick(&s, w, &visible);
      printf("  t%d e %u/%.4f", i, g->elementIdx[cell], g->temperature[cell]);
    }
    printf("\n");
    s.shutdown();
  }

  // Water boiling. The margin is somewhere between +0.05 (does not change) and +127.5
  // (does), so walk it.
  SweepTransition(dll, t, "water -> steam", wi, 1000.0f, wp->highTemp,
                  {2.9f, 2.95f, 3.0f, 3.05f, 3.1f, 3.2f, 3.25f, 3.3f, 3.4f, 3.5f}, 3);
  // Water freezing, the other direction, to see whether the margin is symmetric.
  SweepTransition(dll, t, "water -> ice", wi, 1000.0f, wp->lowTemp,
                  {-2.9f, -2.95f, -3.0f, -3.05f, -3.1f, -3.2f, -3.25f, -3.3f, -3.5f}, 3);
  // Same boundary at a very different mass: if the margin is an energy threshold rather
  // than a temperature one, 5 kg and 1000 kg will not agree.
  SweepTransition(dll, t, "water -> steam, 5 kg", wi, 5.0f, wp->highTemp,
                  {2.95f, 3.0f, 3.05f, 3.1f, 3.2f}, 3);
  SweepTransition(dll, t, "water -> steam, 0.5 kg", wi, 0.5f, wp->highTemp,
                  {2.95f, 3.0f, 3.05f, 3.1f, 3.2f}, 3);
  // A solid with a much higher threshold and a very different specific heat. If the margin
  // is a fixed number of kelvin it lands in the same place; if it is a fraction of the
  // threshold or an energy, it does not.
  SweepTransition(dll, t, "granite -> magma", gi, 2000.0f, t.At(gi)->highTemp,
                  {2.9f, 2.95f, 3.0f, 3.05f, 3.1f, 3.2f, 3.3f, 3.5f, 3.9f, 4.0f}, 3);
  // A low-threshold solid, so the same absolute margin is a very different fraction.
  {
    int32_t low = -1;
    for (int32_t i = 0; i < t.count; ++i) {
      const Element* p = t.At(i);
      if ((p->state & 3) == 3 && p->highTemp > 200.0f && p->highTemp < 320.0f &&
          p->highTempTransitionIdx < t.count) {
        low = i;
        break;
      }
    }
    if (low >= 0) {
      SweepTransition(dll, t, "low-threshold solid", low, 100.0f, t.At(low)->highTemp,
                      {2.9f, 2.95f, 3.0f, 3.05f, 3.1f, 3.2f, 3.5f, 4.0f}, 3);
    }
  }
}

// Round three. The boundary and the temperature adjustment are known; what is left is
// *when* the transition runs inside a substep, whether it can fire more than once, and
// what happens to the secondary product some elements carry.
void ProbeStates3(const Tables& t, const char* dll) {
  const int32_t wi = t.IndexOf(kWater), gi = t.IndexOf(kGranite);
  if (wi < 0 || gi < 0) return;

  // --- Can a transition chain? Ice at 500 K is above ice's threshold *and* above the
  // threshold of the water it becomes. One transition per substep gives water on the first
  // frame and steam on the second; a loop gives steam immediately, two 1.5 K
  // adjustments deep.
  {
    const int32_t ice = t.At(wi)->lowTempTransitionIdx;
    Backend s;
    if (ice < t.count && s.Bind(dll, "chain")) {
      World w;
      w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 500.0f);
      w.Set(5, 5, static_cast<uint16_t>(ice), 1000.0f, 500.0f);
      const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
      std::vector<uint8_t> visible(w.Count(), 1);
      const int cell = w.Cell(5, 5);
      printf("  chaining: ice (idx %d) 1000 kg at 500 K, two thresholds below it\n", ice);
      printf("    start e %u T %.4f\n", g->elementIdx[cell], g->temperature[cell]);
      for (int i = 1; i <= 5; ++i) {
        g = Tick(&s, w, &visible);
        printf("    tick %d: e %u T %.4f  sub=%d\n", i, g->elementIdx[cell],
               g->temperature[cell], g->numSubstanceChangeInfo);
      }
      s.shutdown();
    }
  }
  // The same world on a 0.4 s frame, which runs two substeps. If the transition is a
  // per-substep step it fires twice in one frame here; if it is a per-frame pass it does
  // not, and that difference is the only way to tell the two apart.
  {
    const int32_t ice = t.At(wi)->lowTempTransitionIdx;
    Backend s;
    if (ice < t.count && s.Bind(dll, "chain2")) {
      World w;
      w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 500.0f);
      w.Set(5, 5, static_cast<uint16_t>(ice), 1000.0f, 500.0f);
      const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
      std::vector<uint8_t> visible(w.Count(), 1);
      const int cell = w.Cell(5, 5);
      printf("  chaining at dt = 0.4 (two substeps per frame)\n");
      printf("    start e %u T %.4f\n", g->elementIdx[cell], g->temperature[cell]);
      for (int i = 1; i <= 5; ++i) {
        g = Tick(&s, w, &visible, 0.4f);
        printf("    tick %d: e %u T %.4f  sub=%d\n", i, g->elementIdx[cell],
               g->temperature[cell], g->numSubstanceChangeInfo);
      }
      s.shutdown();
    }
  }

  // --- Ordering against conduction. The sample starts just under the boundary and a hot
  // neighbour walks it across. The frame index is useless here because the readout races
  // by up to one frame, so read the *values*: if conduction runs first, the reported
  // post-transition temperature is (previous + this frame's conduction) - 1.5, and the
  // step across the boundary is the same size as the steps before it.
  {
    Backend s;
    if (s.Bind(dll, "order")) {
      World w;
      w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
      // The sample sits 0.4 K under the boundary with a very hot neighbour on one side,
      // so it needs a few frames to cross and every step is measurable.
      w.Set(5, 5, static_cast<uint16_t>(wi), 1000.0f, 375.1f);
      w.Set(5, 6, static_cast<uint16_t>(gi), 2000.0f, 1500.0f);
      const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
      std::vector<uint8_t> visible(w.Count(), 1);
      const int cell = w.Cell(5, 5);
      printf("  ordering: water at 375.1 K (boundary 375.5), granite neighbour at 1500 K\n");
      float prev = g->temperature[cell];
      printf("    start e %u T %.5f\n", g->elementIdx[cell], prev);
      for (int i = 1; i <= 14; ++i) {
        g = Tick(&s, w, &visible);
        const float now = g->temperature[cell];
        printf("    tick %2d: e %u T %.5f  step %+.5f%s\n", i, g->elementIdx[cell], now,
               now - prev, g->numSubstanceChangeInfo ? "   <-- changed" : "");
        prev = now;
      }
      s.shutdown();
    }
  }

  // --- The secondary product. Element 31 carries a transition ore at 20% mass; the first
  // attempt seeded it one degree over its threshold, which is inside the 3 K margin, so
  // nothing happened. Seed it properly and watch where the other 20% goes.
  {
    for (int32_t i = 0; i < t.count; ++i) {
      const Element* p = t.At(i);
      if (p->highTempTransitionOreID == 0 || p->highTemp <= 0.0f ||
          p->highTemp > 3000.0f) {
        continue;
      }
      const int32_t ore = t.IndexOf(p->highTempTransitionOreID);
      World w;
      w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
      w.Set(5, 5, static_cast<uint16_t>(i), 1000.0f, p->highTemp + 3.5f);
      char label[224];
      snprintf(label, sizeof(label),
               "ore product: idx %d 1000 kg at %.3f K -> idx %d + ore idx %d (hash %d) x %.4f",
               i, p->highTemp + 3.5f, p->highTempTransitionIdx, ore,
               p->highTempTransitionOreID, p->highTempTransitionOreMassConversion);
      RunState(dll, t, label, w, {{5, 5}, {5, 6}, {5, 4}}, 4);
      break;
    }
  }
  // And the low-temperature direction, which a different element carries.
  {
    for (int32_t i = 0; i < t.count; ++i) {
      const Element* p = t.At(i);
      if (p->lowTempTransitionOreID == 0 || p->lowTemp <= 10.0f) continue;
      const int32_t ore = t.IndexOf(p->lowTempTransitionOreID);
      World w;
      w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
      w.Set(5, 5, static_cast<uint16_t>(i), 1000.0f, p->lowTemp - 3.5f);
      char label[224];
      snprintf(label, sizeof(label),
               "ore product (low): idx %d 1000 kg at %.3f K -> idx %d + ore idx %d x %.4f",
               i, p->lowTemp - 3.5f, p->lowTempTransitionIdx, ore,
               p->lowTempTransitionOreMassConversion);
      RunState(dll, t, label, w, {{5, 5}, {5, 6}, {5, 4}}, 4);
      break;
    }
  }
}

// Round four. The ordering probe above was contaminated — its 1500 K granite neighbour is
// 550 K over granite's own melting point, so the "neighbour" melted into magma on the same
// frame and the sample's temperature jumped 24 K in one step. Redo it with a neighbour
// that cannot transition, and settle the event-emission rules while at it.
void ProbeStates4(const Tables& t, const char* dll) {
  const int32_t wi = t.IndexOf(kWater), gi = t.IndexOf(kGranite);
  if (wi < 0 || gi < 0) return;

  // --- Ordering, done in a way the readout race cannot spoil.
  //
  // The question is not which frame index the change lands on — the snapshot races by up
  // to a frame, as eight identical runs above showed. It is whether any end-of-frame state
  // can ever show the sample *above* its boundary and still unchanged. If the transition
  // runs after conduction inside a substep, no such state exists: the substep that carries
  // the cell over also converts it. If it runs before, exactly one frame shows it over the
  // line and still liquid.
  {
    Backend s;
    if (s.Bind(dll, "order2")) {
      World w;
      w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
      w.Set(5, 5, static_cast<uint16_t>(wi), 1000.0f, 375.1f);
      // 900 K is hot enough to move the sample ~0.03 K a frame and 42 K short of granite's
      // own melting point, so the apparatus stays an apparatus.
      w.Set(5, 6, static_cast<uint16_t>(gi), 2000.0f, 900.0f);
      const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
      std::vector<uint8_t> visible(w.Count(), 1);
      const int cell = w.Cell(5, 5);
      printf("  ordering: water 1000 kg at 375.1 K (boundary 375.5), granite at 900 K\n");
      float prev = g->temperature[cell];
      for (int i = 1; i <= 30; ++i) {
        g = Tick(&s, w, &visible);
        const float now = g->temperature[cell];
        const bool over = g->elementIdx[cell] == wi && now > 375.5f;
        printf("    tick %2d: e %u T %.5f  step %+.5f%s\n", i, g->elementIdx[cell], now,
               now - prev, over ? "   <-- liquid, over the boundary" : "");
        prev = now;
        if (g->elementIdx[cell] != wi) break;
      }
      s.shutdown();
    }
  }

  // --- Which events does a transition emit? Four pairings are needed to separate
  // "solidity changed" from "the cell was ever liquid" from "any change at all", and the
  // liquid ones are already covered, so find a solid -> solid and a gas -> anything.
  auto run_pairing = [&](const char* what, int32_t idx, float temp, float mass) {
    World w;
    w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
    w.Set(5, 5, static_cast<uint16_t>(idx), mass, temp);
    char label[224];
    const Element* p = t.At(idx);
    snprintf(label, sizeof(label), "%s: idx %d state %u -> idx %d state %u, %.1f kg at %.2f K",
             what, idx, p->state & 3,
             temp > p->highTemp ? p->highTempTransitionIdx : p->lowTempTransitionIdx,
             temp > p->highTemp
                 ? (p->highTempTransitionIdx < t.count
                        ? t.At(p->highTempTransitionIdx)->state & 3
                        : 0)
                 : (p->lowTempTransitionIdx < t.count
                        ? t.At(p->lowTempTransitionIdx)->state & 3
                        : 0),
             mass, temp);
    RunState(dll, t, label, w, {{5, 5}}, 3);
  };

  // solid -> solid, if the table has one.
  for (int32_t i = 0; i < t.count; ++i) {
    const Element* p = t.At(i);
    if ((p->state & 3) != 3) continue;
    if (p->highTempTransitionIdx >= t.count) continue;
    if ((t.At(p->highTempTransitionIdx)->state & 3) != 3) continue;
    if (p->highTemp <= 0.0f || p->highTemp > 3000.0f) continue;
    run_pairing("solid -> solid", i, p->highTemp + 3.5f, 1000.0f);
    break;
  }
  // gas -> liquid, the condensation direction.
  for (int32_t i = 0; i < t.count; ++i) {
    const Element* p = t.At(i);
    if ((p->state & 3) != 1) continue;
    if (p->lowTempTransitionIdx >= t.count) continue;
    if ((t.At(p->lowTempTransitionIdx)->state & 3) != 2) continue;
    if (p->lowTemp <= 10.0f) continue;
    run_pairing("gas -> liquid", i, p->lowTemp - 3.5f, 5.0f);
    break;
  }
  // gas -> solid, if one exists.
  for (int32_t i = 0; i < t.count; ++i) {
    const Element* p = t.At(i);
    if ((p->state & 3) != 1) continue;
    if (p->lowTempTransitionIdx >= t.count) continue;
    if ((t.At(p->lowTempTransitionIdx)->state & 3) != 3) continue;
    if (p->lowTemp <= 10.0f) continue;
    run_pairing("gas -> solid", i, p->lowTemp - 3.5f, 5.0f);
    break;
  }

  // --- Is there a minimum mass? A cell with a gram in it either transitions like any
  // other or is left alone, and the difference decides whether a boiling puddle's last
  // grams turn to steam or sit there as impossible super-heated water forever.
  {
    const Element* wp = t.At(wi);
    for (float m : {1.0f, 0.1f, 0.01f, 0.001f, 0.0f}) {
      const TransitionResult r =
          RunTransition(dll, t, static_cast<uint16_t>(wi), m, wp->highTemp + 4.0f, 3);
      printf("  minimum mass: water %.4f kg at %.2f K -> %s (e %u, %.5f kg, %.4f K)\n", m,
             wp->highTemp + 4.0f, r.changed ? "changed" : "unchanged", r.element,
             r.mass_after, r.after);
    }
  }
}

// Round five: where the transition sits relative to *flow*, and the one other way an
// element changes — sublimation, which five elements in the table carry and two of them at
// probability 1.0, so it is not necessarily a coin toss.
void ProbeStates5(const Tables& t, const char* dll) {
  const int32_t wi = t.IndexOf(kWater), gi = t.IndexOf(kGranite),
                vi = t.IndexOf(kVacuum);
  if (wi < 0 || gi < 0 || vi < 0) return;

  // --- Transition against flow. A few kilograms of water at the bottom of a sealed
  // vacuum shaft cannot move: the floor is granite, the walls are granite, and it is far
  // under maxMass so it cannot push upward either. The moment it becomes steam it can,
  // because gas flows into vacuum with no minimum at all. So the first snapshot that shows
  // steam answers the question: still 5 kg means flow ran before the transition, less than
  // 5 kg means after.
  //
  // The whole world sits at the sample's temperature so conduction contributes nothing.
  {
    Backend s;
    if (s.Bind(dll, "flowseq")) {
      World w;
      w.Init(12, 14, static_cast<uint16_t>(gi), 2000.0f, 500.0f);
      for (int y = 3; y <= 10; ++y) w.Set(5, y, static_cast<uint16_t>(vi), 0.0f, 500.0f);
      w.Set(5, 3, static_cast<uint16_t>(wi), 5.0f, 500.0f);
      const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
      std::vector<uint8_t> visible(w.Count(), 1);
      printf("  transition vs flow: 5 kg water at 500 K on the floor of a vacuum shaft\n");
      for (int i = 0; i <= 4; ++i) {
        printf("    tick %d:", i);
        for (int y = 3; y <= 6; ++y) {
          const int c = w.Cell(5, y);
          printf("  y%d e%u/%.4f", y, g->elementIdx[c], g->mass[c]);
        }
        printf("\n");
        g = Tick(&s, w, &visible);
        if (!g) break;
      }
      s.shutdown();
    }
  }

  // --- Sublimation. Two elements carry probability 1.0, which makes them reproducible
  // offline; the other three are coin tosses and are only listed. Watch both the source
  // cell and the cell above it, because the product is a gas and has to go somewhere.
  for (int32_t i = 0; i < t.count; ++i) {
    const Element* p = t.At(i);
    if (p->sublimateIndex == 0xFFFF || p->sublimateProbability < 1.0f) continue;
    World w;
    w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
    // An empty cell above, so the product has room and its mass is unambiguous.
    w.Set(5, 6, static_cast<uint16_t>(vi), 0.0f, 293.15f);
    w.Set(5, 5, static_cast<uint16_t>(i), 100.0f, 293.15f);
    char label[224];
    snprintf(label, sizeof(label),
             "sublimation: idx %d 100 kg -> idx %u, rate %.4f eff %.4f prob %.4f", i,
             p->sublimateIndex, p->sublimateRate, p->sublimateEfficiency,
             p->sublimateProbability);
    RunState(dll, t, label, w, {{5, 5}, {5, 6}}, 8);
    // The same element at half the mass, to see whether the rate is per kilogram, per
    // cell, or proportional to what is there.
    World w2 = w;
    w2.Set(5, 5, static_cast<uint16_t>(i), 50.0f, 293.15f);
    snprintf(label, sizeof(label), "sublimation: idx %d at 50 kg", i);
    RunState(dll, t, label, w2, {{5, 5}, {5, 6}}, 6);
    break;
  }
}

// Round six. Two loose ends from the sublimation run, both about the same thing: a
// transition whose product is a *solid* does not always fill the cell. A 5 kg liquid
// turning solid left the cell empty and handed the game an ore drop instead, while 1000 kg
// of water freezing stayed put as ice. So there is a threshold, and it decides whether the
// grid keeps the mass or the game does.
void ProbeStates6(const Tables& t, const char* dll) {
  const int32_t wi = t.IndexOf(kWater), gi = t.IndexOf(kGranite),
                vi = t.IndexOf(kVacuum);
  if (wi < 0 || gi < 0 || vi < 0) return;
  const Element* wp = t.At(wi);
  const int32_t ice = wp->lowTempTransitionIdx;

  // The elements the sublimation run turned up, printed so the chain can be read rather
  // than guessed at.
  for (int32_t idx : {61, 150, 47}) {
    if (idx < t.count) PrintTransitionRow(t, "sublimation chain", idx);
  }
  if (ice < t.count) {
    printf("  ice idx %d maxMass %.2f, water maxMass %.2f\n", ice, t.At(ice)->maxMass,
           wp->maxMass);
  }

  // Freeze water at a range of masses and record, for each, whether the ice stayed in the
  // cell or left as an ore drop.
  printf("  freezing water at %0.2f K, by mass:\n", wp->lowTemp - 4.0f);
  printf("    %-10s %-8s %-12s %-10s %s\n", "seeded kg", "new e", "cell kg", "ore kg",
         "outcome");
  for (float m : {1000.0f, 700.0f, 500.0f, 400.0f, 350.0f, 300.0f, 200.0f, 100.0f, 50.0f,
                  10.0f, 1.0f}) {
    Backend s;
    if (!s.Bind(dll, "freeze")) break;
    World w;
    w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, wp->lowTemp - 4.0f);
    w.Set(5, 5, static_cast<uint16_t>(wi), m, wp->lowTemp - 4.0f);
    const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
    std::vector<uint8_t> visible(w.Count(), 1);
    const int cell = w.Cell(5, 5);
    float ore_mass = 0.0f;
    uint16_t ore_elem = 0;
    for (int i = 1; i <= 3; ++i) {
      g = Tick(&s, w, &visible);
      if (!g) break;
      for (int k = 0; k < g->numSpawnOreInfo; ++k) {
        if (g->spawnOreInfo[k].cellIdx == cell) {
          ore_mass += g->spawnOreInfo[k].mass;
          ore_elem = g->spawnOreInfo[k].elemIdx;
        }
      }
      if (g->elementIdx[cell] != wi) break;
    }
    printf("    %-10.2f %-8u %-12.4f %-10.4f %s%s\n", m, g->elementIdx[cell],
           g->mass[cell], ore_mass,
           g->elementIdx[cell] == static_cast<uint16_t>(ice) ? "stayed as ice" : "left",
           ore_elem ? " (ore spawned)" : "");
    s.shutdown();
  }

  // The same question in the other direction and with a different pair, so the answer is
  // not a property of water: condense a gas into a liquid at a range of masses and see
  // whether the liquid ever fails to stay.
  for (int32_t i = 0; i < t.count; ++i) {
    const Element* p = t.At(i);
    if ((p->state & 3) != 1 || p->lowTempTransitionIdx >= t.count) continue;
    if ((t.At(p->lowTempTransitionIdx)->state & 3) != 2 || p->lowTemp <= 10.0f) continue;
    printf("  condensing idx %d at %.2f K, by mass (product idx %u is liquid):\n", i,
           p->lowTemp - 4.0f, p->lowTempTransitionIdx);
    for (float m : {100.0f, 10.0f, 1.0f, 0.1f}) {
      const TransitionResult r =
          RunTransition(dll, t, static_cast<uint16_t>(i), m, p->lowTemp - 4.0f, 3);
      printf("    %-10.3f -> e %-6u %.4f kg  %s\n", m, r.element, r.mass_after,
             r.changed ? "changed" : "unchanged");
    }
    break;
  }

  // And where does a sublimation product go when the cell above is not available? If it
  // has to pick a neighbour, the choice is part of the kernel.
  for (int32_t i = 0; i < t.count; ++i) {
    const Element* p = t.At(i);
    if (p->sublimateIndex == 0xFFFF || p->sublimateProbability < 1.0f) continue;
    World w;
    w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
    // Sealed on every side but the left.
    w.Set(4, 5, static_cast<uint16_t>(vi), 0.0f, 293.15f);
    w.Set(5, 5, static_cast<uint16_t>(i), 100.0f, 293.15f);
    char label[192];
    snprintf(label, sizeof(label), "sublimation with only a left neighbour free: idx %d", i);
    RunState(dll, t, label, w, {{5, 5}, {4, 5}, {5, 6}}, 4);
    // Fully sealed: nowhere at all for the product to go.
    World w2;
    w2.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
    w2.Set(5, 5, static_cast<uint16_t>(i), 100.0f, 293.15f);
    snprintf(label, sizeof(label), "sublimation fully sealed: idx %d", i);
    RunState(dll, t, label, w2, {{5, 5}}, 4);
    break;
  }
}

// Round seven. Freezing water stays in its cell down to a kilogram, but 0.04 kg of the
// sublimation product left as an ore drop and 5 kg of a condensed gas did too. Neither of
// those transitions declares a transition ore, so the ore path is not the declared-ore
// mechanism — something else decides. These probes seed the same elements *directly*, at
// masses the sublimation chain never reaches, so the deciding input is visible.
void ProbeStates7(const Tables& t, const char* dll) {
  const int32_t wi = t.IndexOf(kWater), gi = t.IndexOf(kGranite), vi = t.IndexOf(kVacuum);
  if (wi < 0 || gi < 0 || vi < 0) return;
  const Element* wp = t.At(wi);

  for (int32_t idx : {41, 47, 86, 137, 150}) {
    if (idx < t.count) PrintTransitionRow(t, "state flags", idx);
  }

  // Water, well below a kilogram.
  printf("  freezing water below 1 kg:\n");
  for (float m : {1.0f, 0.5f, 0.1f, 0.05f, 0.04f, 0.01f}) {
    const TransitionResult r =
        RunTransition(dll, t, static_cast<uint16_t>(wi), m, wp->lowTemp - 4.0f, 3);
    printf("    %-8.4f kg -> e %-6u %.5f kg\n", m, r.element, r.mass_after);
  }

  // The sublimation product seeded directly, at a mass the chain never produces. If a
  // hundred kilograms of it becomes a solid cell while 0.04 kg becomes an ore, the
  // threshold is a mass; if both become ore, it is a property of the element.
  for (int32_t idx : {150, 137}) {
    if (idx >= t.count) continue;
    const Element* p = t.At(idx);
    const float temp = p->lowTemp - 4.0f;
    if (p->lowTempTransitionIdx >= t.count) continue;
    printf("  idx %d (state %u) freezing to idx %u (state %u) at %.2f K:\n", idx,
           p->state & 3, p->lowTempTransitionIdx,
           t.At(p->lowTempTransitionIdx)->state & 3, temp);
    for (float m : {1000.0f, 100.0f, 10.0f, 1.0f, 0.1f, 0.04f}) {
      Backend s;
      if (!s.Bind(dll, "freeze2")) break;
      World w;
      w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, temp);
      w.Set(5, 5, static_cast<uint16_t>(idx), m, temp);
      const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
      std::vector<uint8_t> visible(w.Count(), 1);
      const int cell = w.Cell(5, 5);
      float ore = 0.0f;
      uint16_t ore_elem = 0;
      for (int i = 1; i <= 3; ++i) {
        g = Tick(&s, w, &visible);
        if (!g) break;
        for (int k = 0; k < g->numSpawnOreInfo; ++k) {
          if (g->spawnOreInfo[k].cellIdx != cell) continue;
          ore += g->spawnOreInfo[k].mass;
          ore_elem = g->spawnOreInfo[k].elemIdx;
        }
        if (g->elementIdx[cell] != idx) break;
      }
      printf("    %-9.3f kg -> cell e %-5u %-10.4f  ore e %-5u %.4f kg\n", m,
             g->elementIdx[cell], g->mass[cell], ore_elem, ore);
      s.shutdown();
    }
  }

  // The direct freeze probe stops at the frame the element changes, which is exactly one
  // frame too early to see what the earlier condensation run saw: a 5 kg solid leaving the
  // cell as an ore drop. Keep ticking.
  for (float m : {1000.0f, 100.0f, 5.0f, 1.0f}) {
    World w;
    w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 384.35f);
    w.Set(5, 5, static_cast<uint16_t>(137), m, 384.35f);
    char label[160];
    snprintf(label, sizeof(label), "freeze idx 137 -> 41, %.2f kg, kept running", m);
    RunState(dll, t, label, w, {{5, 5}}, 5);
  }

  // Exact replication of the one run that ejected a solid as an ore, plus the same world
  // with the sample seeded as the liquid it becomes. If only the first ejects, the history
  // matters; if both do, it is the surroundings.
  {
    World w;
    w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
    w.Set(5, 5, static_cast<uint16_t>(177), 5.0f, 606.65f);
    RunState(dll, t, "replicate: gas 177 5 kg at 606.65 K, granite at 293.15", w,
             {{5, 5}}, 5);
    World w2;
    w2.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
    w2.Set(5, 5, static_cast<uint16_t>(137), 5.0f, 294.65f);
    RunState(dll, t, "same world, seeded as liquid 137 5 kg at 294.65 K", w2, {{5, 5}}, 5);
    World w3;
    w3.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
    w3.Set(5, 5, static_cast<uint16_t>(137), 1000.0f, 294.65f);
    RunState(dll, t, "same world, liquid 137 at 1000 kg", w3, {{5, 5}}, 5);
  }

  // What may a sublimation product displace? A granite neighbour blocked it completely,
  // but 0.04 kg of a solid did not — the sim ejected that solid as an ore drop and took
  // the cell. Two candidate rules fit: the solid is light enough, or it is that particular
  // element. Vary each independently.
  for (int32_t i = 0; i < t.count; ++i) {
    const Element* p = t.At(i);
    if (p->sublimateIndex == 0xFFFF || p->sublimateProbability < 1.0f) continue;
    const uint16_t frozen = 47;  // what the product becomes at room temperature
    printf("  sublimation into an occupied neighbour (source idx %d, product idx %u):\n", i,
           p->sublimateIndex);
    printf("    %-22s %-10s %-14s %-12s %s\n", "neighbour", "kg", "source lost", "cell after",
           "ore");
    struct Case { const char* name; uint16_t elem; float mass; };
    const Case cases[] = {
        {"idx 47 (frozen form)", frozen, 0.04f}, {"idx 47", frozen, 1.0f},
        {"idx 47", frozen, 100.0f},              {"granite", static_cast<uint16_t>(gi), 0.04f},
        {"granite", static_cast<uint16_t>(gi), 1.0f},
        {"granite", static_cast<uint16_t>(gi), 2000.0f},
    };
    for (const Case& c : cases) {
      Backend s;
      if (!s.Bind(dll, "subl")) break;
      World w;
      w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
      w.Set(5, 6, c.elem, c.mass, 293.15f);
      w.Set(5, 5, static_cast<uint16_t>(i), 100.0f, 293.15f);
      const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
      std::vector<uint8_t> visible(w.Count(), 1);
      const int src = w.Cell(5, 5), dst = w.Cell(5, 6);
      float ore = 0.0f;
      uint16_t ore_elem = 0;
      for (int k = 0; k < 3; ++k) {
        g = Tick(&s, w, &visible);
        if (!g) break;
        for (int j = 0; j < g->numSpawnOreInfo; ++j) {
          if (g->spawnOreInfo[j].cellIdx != dst) continue;
          ore += g->spawnOreInfo[j].mass;
          ore_elem = g->spawnOreInfo[j].elemIdx;
        }
      }
      printf("    %-22s %-10.3f %-14.4f e%-4u %-6.4f  e%u %.4f\n", c.name, c.mass,
             100.0f - g->mass[src], g->elementIdx[dst], g->mass[dst], ore_elem, ore);
      s.shutdown();
    }
    break;
  }

  // Where does a sublimation product go when it has a choice? Both earlier runs had
  // exactly one free neighbour, so they only proved it will use whatever is available.
  for (int32_t i = 0; i < t.count; ++i) {
    const Element* p = t.At(i);
    if (p->sublimateIndex == 0xFFFF || p->sublimateProbability < 1.0f) continue;
    World w;
    w.Init(12, 12, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
    w.Set(4, 5, static_cast<uint16_t>(vi), 0.0f, 293.15f);
    w.Set(6, 5, static_cast<uint16_t>(vi), 0.0f, 293.15f);
    w.Set(5, 4, static_cast<uint16_t>(vi), 0.0f, 293.15f);
    w.Set(5, 6, static_cast<uint16_t>(vi), 0.0f, 293.15f);
    w.Set(5, 5, static_cast<uint16_t>(i), 100.0f, 293.15f);
    char label[192];
    snprintf(label, sizeof(label), "sublimation with all four neighbours free: idx %d", i);
    RunState(dll, t, label, w, {{5, 5}, {5, 6}, {5, 4}, {4, 5}, {6, 5}}, 4);
    break;
  }
}

// `NewGameFrame` carries maxX and maxY and this harness has always sent
// `maxY = height - 1` without knowing what the sim does with them. The state-change
// scenario made it matter: Klei held the world's top row at *exactly* its seeded
// temperature while its neighbour below cooled 78 K past it, which no conduction rule
// allows. Either that row is outside the simulated region or the bound means something
// else entirely.
void ProbeFrameBounds(const Tables& t, const char* dll) {
  const int32_t gi = t.IndexOf(kGranite), ci = t.IndexOf(kCopper);
  if (gi < 0 || ci < 0) return;

  // One hot cell in a cold block, run at a series of maxY values, reading the cell itself
  // and the cell above it. If maxY bounds the simulation, there is a value at which the
  // pair stops exchanging heat.
  auto run = [&](int probe_y, int max_x, int max_y) {
    Backend s;
    if (!s.Bind(dll, "bounds")) return;
    World w;
    w.Init(16, 12, static_cast<uint16_t>(gi), 2000.0f, 300.0f);
    w.Set(5, probe_y, static_cast<uint16_t>(ci), 1000.0f, 500.0f);
    const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
    std::vector<uint8_t> visible(w.Count(), 1);
    NewGameFrame f{};
    f.elapsedSeconds = 0.2f;
    f.maxX = max_x;
    f.maxY = max_y;
    for (int i = 0; i < 4; ++i) {
      s.handle_message(static_cast<int32_t>(SimMessageHash::SimFrameManager_NewGameFrame),
                       sizeof(f), reinterpret_cast<const uint8_t*>(&f));
      g = static_cast<const GameDataUpdate*>(s.handle_message(
          static_cast<int32_t>(SimMessageHash::PrepareGameData),
          static_cast<int>(visible.size()), visible.data()));
      if (!g) break;
    }
    const int c = w.Cell(5, probe_y);
    // The row above the topmost cell is off the end of the array, not merely inactive.
    const float above =
        probe_y + 1 < w.height ? g->temperature[c + w.width] : 0.0f;
    printf("    y %2d, maxX %2d maxY %2d:  sample %9.5f  above %9.5f  below %9.5f\n",
           probe_y, max_x, max_y, g->temperature[c], above, g->temperature[c - w.width]);
    s.shutdown();
  };

  printf("  hot copper cell in cold granite, 16 x 12 world, after 4 ticks:\n");
  for (int y : {5, 9, 10, 11}) run(y, 16, 11);
  printf("  the same cell at y = 10, varying maxY:\n");
  for (int my : {8, 9, 10, 11, 12}) run(10, 16, my);
  printf("  and at x = 14, varying maxX:\n");
  {
    for (int mx : {10, 13, 14, 15, 16}) {
      Backend s;
      if (!s.Bind(dll, "bounds")) break;
      World w;
      w.Init(16, 12, static_cast<uint16_t>(gi), 2000.0f, 300.0f);
      w.Set(14, 5, static_cast<uint16_t>(ci), 1000.0f, 500.0f);
      const GameDataUpdate* g = Boot(&s, t, WorldPayload(w));
      std::vector<uint8_t> visible(w.Count(), 1);
      NewGameFrame f{};
      f.elapsedSeconds = 0.2f;
      f.maxX = mx;
      f.maxY = 11;
      for (int i = 0; i < 4; ++i) {
        s.handle_message(
            static_cast<int32_t>(SimMessageHash::SimFrameManager_NewGameFrame), sizeof(f),
            reinterpret_cast<const uint8_t*>(&f));
        g = static_cast<const GameDataUpdate*>(s.handle_message(
            static_cast<int32_t>(SimMessageHash::PrepareGameData),
            static_cast<int>(visible.size()), visible.data()));
        if (!g) break;
      }
      const int c = w.Cell(14, 5);
      printf("    x 14, maxX %2d: sample %9.5f  left %9.5f  right %9.5f\n", mx,
             g->temperature[c], g->temperature[c - 1], g->temperature[c + 1]);
      s.shutdown();
    }
  }
}

// `NewGameFrame` carries `currentSunlightIntensity`, and this harness has sent it as zero
// since the day it was written — every `Tick` here memsets the struct and fills in only
// elapsedSeconds, maxX and maxY. That is almost certainly why offline probes reported no
// sunlight in a vacuum shaft open to space, which was written down as "sunlight is not
// geometric" and then contradicted by a live game's notebook. Send it and find out.
void ProbeSunlightIntensity(const Tables& t, const char* dll) {
  const int32_t gi = t.IndexOf(kGranite), vi = t.IndexOf(kVacuum), oi = t.IndexOf(kOxygen);
  if (gi < 0 || vi < 0 || oi < 0) return;

  auto run = [&](float intensity, bool headless) {
    Backend s;
    if (!s.Bind(dll, "sun")) return;
    World w;
    w.Init(12, 16, static_cast<uint16_t>(gi), 2000.0f, 293.15f);
    // A shaft open all the way to the top of the world, with a gas column beside it, so
    // both "nothing in the way" and "mass in the way" are visible in one run.
    for (int y = 1; y < 16; ++y) {
      w.Set(5, y, static_cast<uint16_t>(vi), 0.0f, 293.15f);
      w.Set(7, y, static_cast<uint16_t>(oi), 1.0f + 0.5f * y, 293.15f);
    }
    const GameDataUpdate* g = Boot(&s, t, WorldPayload(w, headless));
    std::vector<uint8_t> visible(w.Count(), 1);
    NewGameFrame f{};
    f.elapsedSeconds = 0.2f;
    f.maxX = w.width;
    f.maxY = w.height - 1;
    f.currentSunlightIntensity = intensity;
    for (int i = 0; i < 3; ++i) {
      s.handle_message(static_cast<int32_t>(SimMessageHash::SimFrameManager_NewGameFrame),
                       sizeof(f), reinterpret_cast<const uint8_t*>(&f));
      g = static_cast<const GameDataUpdate*>(s.handle_message(
          static_cast<int32_t>(SimMessageHash::PrepareGameData),
          static_cast<int>(visible.size()), visible.data()));
      if (!g) break;
    }
    const auto* sun = static_cast<const uint8_t*>(g->propertyTextureExposedToSunlight);
    printf("  intensity %8.1f headless %d:  vacuum shaft (x=5) top to bottom: ", intensity,
           headless ? 1 : 0);
    for (int y = 15; y >= 1; --y) printf("%4u", sun[w.Cell(5, y)]);
    printf("\n                     gas column   (x=7) top to bottom: ");
    for (int y = 15; y >= 1; --y) printf("%4u", sun[w.Cell(7, y)]);
    printf("\n                     solid rock   (x=2) top to bottom: ");
    for (int y = 15; y >= 1; --y) printf("%4u", sun[w.Cell(2, y)]);
    printf("\n");
    s.shutdown();
  };
  for (float i : {0.0f, 1.0f, 1000.0f, 40000.0f}) run(i, true);
  for (float i : {0.0f, 1.0f, 1000.0f, 40000.0f}) run(i, false);
}

// ------------------------------------------------------ the conduit temperature manager
//
// Every other scenario in this file drives the sim through `SIM_HandleMessage` and compares
// the grid that comes back. Conduits cannot be reached that way at all: their contents are
// not cells, no message creates one, and nothing about them appears in a `GameDataUpdate`
// except the building temperatures they push heat into. So this is a second harness against
// the same two DLLs — the seven `ConduitTemperatureManager_*` exports, called directly, with
// the two sides' returns compared field for field.
//
// Two things make that a stronger check than it sounds. The exports are pure ABI, so there
// is no `SimThread` between the call and the answer and nothing to wait for — the comparison
// bar is bit equality rather than an envelope, and every scenario below is expected to hold
// it exactly. And the loop deliberately runs a real frame between updates, because the only
// way the conduit reaches the rest of the sim is a `ModifyBuildingEnergy` message that the
// next frame drains: comparing the building temperatures after that frame is what proves the
// energy went back, and with the right sign, rather than merely that the contents cooled.
//
// What this still cannot check is the half that lives in the game: whether the handle the
// game files under a pipe is the handle the sim thinks that pipe is. That needs the game.
// `Pack = 4`, exactly as the C# declaration says. Without it x64 pads each int32 out to the
// following pointer's alignment and the struct is 48 bytes instead of 36 — every field after
// the first reads from the wrong place, `numEntries` comes back as a pointer's low half, and
// the harness walks off into an array that is not there. It does not crash; it hangs.
#pragma pack(push, 4)
struct ConduitUpdateData {
  int32_t numEntries;
  float* temperatures;
  int32_t numFrozenHandles;
  int32_t* frozenHandles;
  int32_t numMeltedHandles;
  int32_t* meltedHandles;
};
#pragma pack(pop)
static_assert(sizeof(ConduitUpdateData) == 36, "ConduitUpdateData layout drift");

int RunConduits(Backend* klei, Backend* mine, const Tables& t, bool live) {
  // Line buffering, because this harness calls into Klei's DLL with arguments the physics
  // scenarios never send and a call that does not come back leaves a block-buffered stdout
  // with nothing in it — which says nothing about where it stopped.
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (!klei->BindConduits()) {
    printf("the klei DLL is missing a ConduitTemperatureManager export\n");
    return 1;
  }
  if (!mine->BindConduits()) {
    printf("our DLL is missing a ConduitTemperatureManager export\n");
    return 1;
  }

  int failures = 0;
  const char* step = "";
  auto fail = [&](const char* what, const char* detail) {
    printf("  MISMATCH  %-22s %s  %s\n", step, what, detail);
    ++failures;
  };
  auto cmp_i = [&](const char* what, int a, int b) {
    if (a == b) return;
    char buf[128];
    snprintf(buf, sizeof(buf), "klei %d, mine %d", a, b);
    fail(what, buf);
  };
  // Bit equality, not a tolerance: same inputs, same arithmetic, one thread. Anything less
  // than exact here would hide the two branches this kernel is made of.
  auto cmp_f = [&](const char* what, float a, float b) {
    if (memcmp(&a, &b, sizeof(float)) == 0) return;
    char buf[160];
    snprintf(buf, sizeof(buf), "klei %.9g, mine %.9g  (delta %.6g)", static_cast<double>(a),
             static_cast<double>(b), static_cast<double>(b) - static_cast<double>(a));
    fail(what, buf);
  };

  World w;
  w.Init(16, 12, static_cast<uint16_t>(t.IndexOf(kGranite)), 2000.0f, 293.15f);
  const std::vector<uint8_t> payload = WorldPayload(w);
  std::vector<uint8_t> visible(w.Count(), 1);

  // Four buildings for the conduits to exchange against, chosen to put one conduit on each
  // side of every branch: two mild ones, one cold enough to freeze water and one hot enough
  // to boil it. In the default mode they are a table of numbers this harness writes; in
  // `--conduits-live` they are real registrations and the array comes back from the sim.
  enum {
    kMildBuilding = 0,
    kWarmBuilding = 1,
    kColdBuilding = 2,
    kHotBuilding = 3,
    // Held just above water's freezing point on purpose. A conduit against this one
    // approaches it from above and never leaves the band between it and `lowTemp`, which is
    // the only place the three-degree transition margin is observable: inside the margin a
    // conduit is cold enough to have crossed `lowTemp` and still must not be reported.
    kNearFreezingBuilding = 4,
    kBuildingCount = 5,
  };
  const float kStartTemperature[kBuildingCount] = {300.0f, 340.0f, 100.0f, 500.0f, 271.5f};

  // Why there are two modes at all.
  //
  // Klei's sim runs on its own thread and a `PrepareGameData` that arrives before that
  // thread has finished reports zero frames — a "spin", which the physics suite already
  // measures and absorbs with envelopes. This harness cannot absorb one: a tick where Klei
  // ran no frame is a tick where the conduit's energy message was not drained, so Klei's
  // building ends up exactly one application behind ours and *stays* there. Measured at one
  // spin, always on the same tick, and longer waits do not remove it — it is not a race, it
  // is something about the frame after `Start`.
  //
  // So the default mode takes the buildings out of the loop entirely: both sims are handed
  // the *same* table of building temperatures, no frame is run, and the only thing under
  // test is the conduit kernel. That is the pass that is expected to be bit-exact, and it is
  // the one that would catch an arithmetic defect. `--conduits-live` puts the real buildings
  // back so the `ModifyBuildingEnergy` round trip is exercised, and reports the drift the
  // spin causes rather than pretending it is a defect.
  struct Side {
    Backend* s;
    const GameDataUpdate* u = nullptr;
  };
  Side sides[2] = {{klei}, {mine}};

  // Advance one side by exactly one *frame*, which is not the same as one tick. Klei's sim
  // runs on its own thread and a tick that arrives before it has finished reports zero
  // frames processed; ours always reports one. Ticking both sides the same number of times
  // therefore steps their buildings a different number of times, and since a conduit's
  // energy is applied by a frame, everything downstream is then compared out of phase — the
  // whole of what the first version of this pass was reporting.
  //
  // Ticking the slower side again is safe here in a way it is not in the physics suite: the
  // suite must not re-ask because that makes Klei run a frame our sim never ran, and here
  // that is exactly the point. Returns the frames the tick produced, so a call that comes
  // back with two — Klei can finish two queued frames at once — is visible to the caller
  // rather than quietly leaving the two sides one apart again.
  // Frames each side has actually processed since boot, which is the only clock the two
  // have in common. Ticks are not: Klei can answer a tick with zero frames (its thread had
  // not finished) or with two (it had two queued), and ours always answers with one.
  long long processed[2] = {0, 0};
  auto tick_once = [&](int k) {
    sides[k].u = Tick(sides[k].s, w, &visible);
    processed[k] += sides[k].u ? sides[k].u->numFramesProcessed : 0;
  };
  // Bring both sides to the same frame count, ticking whichever is behind. Returns false if
  // it cannot get there — Klei answering a tick with two frames can overshoot past the other
  // side, and there is no way to un-run a frame, so the honest answer is to stop rather than
  // to carry on comparing two sims that are no longer on the same step.
  auto equalise = [&]() {
    for (int guard = 0; guard < 32; ++guard) {
      if (processed[0] == processed[1]) return true;
      tick_once(processed[0] < processed[1] ? 0 : 1);
    }
    return processed[0] == processed[1];
  };
  auto advance_both = [&](long long target) {
    for (int k = 0; k < 2; ++k) {
      for (int guard = 0; guard < 32 && processed[k] < target; ++guard) tick_once(k);
    }
    return equalise();
  };
  std::vector<BuildingTemperatureInfo> synthetic(kBuildingCount);
  for (int b = 0; b < kBuildingCount; ++b) {
    synthetic[b].handle = b;
    synthetic[b].temperature = kStartTemperature[b];
  }
  for (Side& sd : sides) {
    sd.u = Boot(sd.s, t, payload);
    if (live) {
      SendScales(sd.s, 0.001f, 0.001f);
      for (int b = 0; b < kBuildingCount; ++b) {
        static const int kx[kBuildingCount] = {2, 6, 10, 2, 6};
        static const int ky[kBuildingCount] = {2, 2, 2, 8, 8};
        SendBuilding(sd.s, t, kCopper, b < 2 ? 400.0f : 4000.0f, kStartTemperature[b], 1.0f,
                     0.0f, kNeverOverheats, kx[b], ky[b], 2, 2);
      }
      // Three *frames*, not three ticks. Our sim publishes nothing on the frame that
      // registers a building — that frame's physics is skipped — and Klei publishes an
      // already-stepped temperature on it, so the two only line up once both have stepped
      // the same number of times.
      // Placeholder: the frames are advanced below, once both sides exist, because getting
      // them onto the same frame is a two-sided operation.
    }
    sd.s->ct_initialize();
  }
  // Three frames each, not three ticks each. Our sim publishes nothing on the frame that
  // registers a building — that frame's physics is skipped — and Klei publishes an
  // already-stepped temperature on it, so the two only line up once both have stepped the
  // same number of times.
  if (live && !advance_both(3)) {
    printf("  could not get both sims onto the same frame before starting; klei %lld,"
           " mine %lld\n", processed[0], processed[1]);
    return 1;
  }

  // Before a single conduit has billed anything, are the two sims' buildings even in the
  // same place? If not, nothing the live pass reports afterwards can be attributed to the
  // conduits, and saying so here is the difference between an instrument and a noise
  // generator. Measured: they are not — 0.16 K apart on a 4000 kg copper
  // building sitting at 100 K in 293 K granite after four frames. That is a *building*
  // exchange divergence at a temperature gap the physics suite's building scenarios never
  // reach (theirs is 400 kg at 400 K), and it is a lead for that subsystem, not this one.
  double setup_spread = 0.0;
  if (live) {
    for (int k = 0; k < 2; ++k) {
      printf("  %s after setup (%lld frames):", sides[k].s->name, processed[k]);
      for (int b = 0; sides[k].u && b < sides[k].u->numBuildingTemperatures; ++b) {
        printf("  %.6f", sides[k].u->buildingTemperatures[b].temperature);
      }
      printf("\n");
    }
    if (sides[0].u && sides[1].u &&
        sides[0].u->numBuildingTemperatures == sides[1].u->numBuildingTemperatures) {
      for (int b = 0; b < sides[0].u->numBuildingTemperatures; ++b) {
        const double d = std::fabs(static_cast<double>(sides[0].u->buildingTemperatures[b].temperature) -
                                   sides[1].u->buildingTemperatures[b].temperature);
        if (d > setup_spread) setup_spread = d;
      }
    }
    if (setup_spread > 0.0) {
      printf("  the two sims' buildings are already %.6f K apart before any conduit has\n"
             "  billed anything, so this pass is informational: it shows the round trip\n"
             "  running, not that it is exact. The kernel pass (--conduits) is the one that\n"
             "  compares conduits against conduits.\n", setup_spread);
    }
  }

  // Eight conduits, one per branch of `Update`. The comments are the reason each exists;
  // together they are the whole reachable surface of the kernel.
  struct Registration {
    const char* name;
    float temperature;
    float mass;
    int32_t element;
    int32_t building;
    float heat_capacity;
    float conductivity;
    int32_t insulated;
  };
  const Registration regs[] = {
      // The ordinary case: warm water in an uninsulated pipe against a cooler building.
      {"plain", 320.0f, 10.0f, kWater, kMildBuilding, 8000.0f, 5.0f, 0},
      // The same pair insulated, which takes `min` of the two conductivities instead of the
      // mean and so must move strictly less heat.
      {"insulated", 320.0f, 10.0f, kWater, kMildBuilding, 8000.0f, 5.0f, 1},
      // A thimble of water against a large building, far apart, with a conductivity high
      // enough that the unlimited transfer overshoots. This is the only registration that
      // reaches the equilibrium branch, and without it that branch is as
      // untested as the insulation branch once was.
      {"overshoot", 500.0f, 0.02f, kWater, kColdBuilding, 40000.0f, 500.0f, 0},
      // Zero mass is zero heat capacity: skipped, but its temperature still copied through
      // to the output array rather than left behind.
      {"massless", 300.0f, 0.0f, kWater, kMildBuilding, 8000.0f, 5.0f, 0},
      // A structure handle the building side never issued. The one path that writes nothing
      // at all, so its output slot has to keep whatever was there before.
      {"orphan", 300.0f, 10.0f, kWater, -1, 8000.0f, 5.0f, 0},
      // Past the sim's 10000 K ceiling, which `Add` refuses and replaces with the element's
      // default temperature instead of clamping.
      {"too_hot", 20000.0f, 10.0f, kWater, kWarmBuilding, 8000.0f, 5.0f, 0},
      // Just above freezing against a very cold building: reaches `lowTemp - 3` after a few
      // updates and has to appear in `frozenHandles` exactly once it does.
      {"freezing", 272.0f, 2.0f, kWater, kColdBuilding, 400.0f, 80.0f, 0},
      // The mirror, against a very hot one, for `meltedHandles`.
      {"boiling", 371.0f, 2.0f, kWater, kHotBuilding, 400.0f, 80.0f, 0},
      // Settles between `lowTemp - 3` and `lowTemp` and stays there. Water below 273.15 K
      // that must *not* be reported frozen, which is the only thing that distinguishes the
      // three-degree margin from no margin at all — every other conduit here either stays
      // well clear of its transition or shoots straight past it.
      {"near_freezing", 274.0f, 2.0f, kWater, kNearFreezingBuilding, 4000.0f, 80.0f, 0},
      // Two elements that carry a transition on one side only, so the sentinels in
      // `SetTransitions` are reached at all: granite has no low transition, oxygen no high
      // one. Both sit against a building far from either threshold, because what is under
      // test is the sentinel and not the crossing.
      {"granite_pipe", 350.0f, 8.0f, kGranite, kMildBuilding, 8000.0f, 5.0f, 0},
      {"oxygen_pipe", 310.0f, 0.5f, kOxygen, kWarmBuilding, 8000.0f, 5.0f, 0},
  };

  int32_t handles[2][16] = {};
  for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) {
    const Registration& r = regs[i];
    step = r.name;
    for (int k = 0; k < 2; ++k) {
      handles[k][i] = sides[k].s->ct_add(r.temperature, r.mass, r.element, r.building,
                                         r.heat_capacity, r.conductivity, r.insulated);
    }
    cmp_i("handle from Add", handles[0][i], handles[1][i]);
  }

  int32_t recycled[2] = {-1, -1};
  const int kUpdates = 24;
  for (int tick = 0; tick < kUpdates; ++tick) {
    char label[32];
    snprintf(label, sizeof(label), "update %d", tick);
    step = label;

    // The three mutations, spread out so each one is observed for several updates
    // afterwards rather than only on the update it happened.
    if (tick == 6) {
      // A pipe that filled with something else. Only the contents change; the pipe's own
      // heat capacity and conductivity are not re-sent and must survive.
      for (int k = 0; k < 2; ++k) {
        sides[k].s->ct_set(handles[k][0], 350.0f, 25.0f, kOxygen);
      }
    }
    if (tick == 9) {
      for (int k = 0; k < 2; ++k) sides[k].s->ct_remove(handles[k][1]);
    }
    if (tick == 12) {
      // The sharp end of the release queue: this must land on the slot the removed conduit
      // gave up, with its version byte bumped, and only because two frames have gone by. A
      // sim that freed the slot immediately would hand back the same handle it did before,
      // and one that never freed it would hand back a fresh slot.
      for (int k = 0; k < 2; ++k) {
        recycled[k] = sides[k].s->ct_add(310.0f, 5.0f, kWater, kWarmBuilding, 8000.0f, 5.0f, 0);
      }
      cmp_i("handle after recycle", recycled[0], recycled[1]);
    }
    if (tick == 15) {
      // A stale handle. Both sims must ignore it rather than write through it, and the
      // conduit that now owns the slot must be untouched — which the temperature comparison
      // below is what actually checks.
      for (int k = 0; k < 2; ++k) sides[k].s->ct_set(handles[k][1], 999.0f, 1.0f, kWater);
    }

    // The buildings move whether or not a sim is moving them, so the kernel is driven over
    // a range of gradients rather than one settled pair. The cold one warms and the hot one
    // cools, which is what walks the freezing and boiling conduits across their thresholds.
    if (!live) {
      synthetic[kMildBuilding].temperature += 0.05f;
      synthetic[kWarmBuilding].temperature -= 0.03f;
      synthetic[kColdBuilding].temperature += 1.0f;
      synthetic[kHotBuilding].temperature -= 1.5f;
    }

    const ConduitUpdateData* out[2] = {nullptr, nullptr};
    for (int k = 0; k < 2; ++k) {
      void* bt = synthetic.data();
      if (live) bt = sides[k].u ? sides[k].u->buildingTemperatures : nullptr;
      out[k] = static_cast<const ConduitUpdateData*>(sides[k].s->ct_update(0.2f, bt));
    }
    if (!out[0] || !out[1]) {
      fail("Update returned null", out[0] ? "mine" : (out[1] ? "klei" : "both"));
      break;
    }

    cmp_i("numEntries", out[0]->numEntries, out[1]->numEntries);
    const int n = out[0]->numEntries < out[1]->numEntries ? out[0]->numEntries
                                                          : out[1]->numEntries;
    for (int i = 0; i < n; ++i) {
      char what[48];
      snprintf(what, sizeof(what), "temperature[%d]", i);
      cmp_f(what, out[0]->temperatures[i], out[1]->temperatures[i]);
    }
    cmp_i("numFrozenHandles", out[0]->numFrozenHandles, out[1]->numFrozenHandles);
    const int nf = out[0]->numFrozenHandles < out[1]->numFrozenHandles
                       ? out[0]->numFrozenHandles
                       : out[1]->numFrozenHandles;
    for (int i = 0; i < nf; ++i) {
      cmp_i("frozenHandles", out[0]->frozenHandles[i], out[1]->frozenHandles[i]);
    }
    cmp_i("numMeltedHandles", out[0]->numMeltedHandles, out[1]->numMeltedHandles);
    const int nm = out[0]->numMeltedHandles < out[1]->numMeltedHandles
                       ? out[0]->numMeltedHandles
                       : out[1]->numMeltedHandles;
    for (int i = 0; i < nm; ++i) {
      cmp_i("meltedHandles", out[0]->meltedHandles[i], out[1]->meltedHandles[i]);
    }

    // The frame that drains the energy the update just billed. Comparing the building
    // temperatures after it is the only evidence that the `ModifyBuildingEnergy` messages
    // were sent, were addressed to the right building and carried the right sign — the
    // conduit side of the exchange would look identical if they were never sent at all.
    if (!live) continue;
    const bool aligned = advance_both(processed[0] + 1);
    if (!aligned) {
      printf("  a side ran other than one frame on update %d; the two are no longer on the\n"
             "  same frame, so the live pass stops with %d update%s compared\n",
             tick, tick, tick == 1 ? "" : "s");
      break;
    }
    if (sides[0].u && sides[1].u) {
      cmp_i("numBuildingTemperatures", sides[0].u->numBuildingTemperatures,
            sides[1].u->numBuildingTemperatures);
      const int nb = sides[0].u->numBuildingTemperatures < sides[1].u->numBuildingTemperatures
                         ? sides[0].u->numBuildingTemperatures
                         : sides[1].u->numBuildingTemperatures;
      for (int i = 0; i < nb; ++i) {
        char what[48];
        snprintf(what, sizeof(what), "building[%d] temperature", i);
        cmp_f(what, sides[0].u->buildingTemperatures[i].temperature,
              sides[1].u->buildingTemperatures[i].temperature);
      }
    }
  }

  // The last state of both sides, printed whether or not anything failed: a run that agrees
  // is only worth something if the numbers moved, and this is where "both sims did nothing
  // identically" would show up.
  const ConduitUpdateData* final_out[2] = {nullptr, nullptr};
  for (int k = 0; k < 2; ++k) {
    void* bt = synthetic.data();
    if (live) bt = sides[k].u ? sides[k].u->buildingTemperatures : nullptr;
    final_out[k] = static_cast<const ConduitUpdateData*>(sides[k].s->ct_update(0.0f, bt));
  }
  printf("\n  slot  klei          mine\n");
  if (final_out[0] && final_out[1]) {
    const int n = final_out[0]->numEntries < final_out[1]->numEntries
                      ? final_out[0]->numEntries
                      : final_out[1]->numEntries;
    for (int i = 0; i < n; ++i) {
      printf("  %4d  %-12.6f  %-12.6f\n", i, final_out[0]->temperatures[i],
             final_out[1]->temperatures[i]);
    }
  }

  for (Side& sd : sides) {
    sd.s->ct_clear();
    sd.s->ct_shutdown();
    sd.s->shutdown();
  }
  // Klei's own frame count, the same instrument the physics suite prints. A spin means a
  // tick where its `SimThread` had not finished, so the frame that was supposed to drain a
  // conduit's energy message did not run — which is indistinguishable from a message that
  // was delayed on purpose unless the count is on the page.
  printf("klei frames: %ld over %ld ticks, %ld spins\n", g_frames, g_ticks, g_spins);
  printf("conduits: %d mismatch%s over %d updates\n", failures,
         failures == 1 ? "" : "es", kUpdates);
  // A live pass that started from buildings already apart cannot fail on conduits, so it
  // reports and returns clean. The kernel pass has no such excuse.
  if (live && setup_spread > 0.0) return 0;
  return failures;
}

// ------------------------------------------------------------------- disease
//
// Two worlds where **nothing but germs can move**: solid granite at one temperature, so the
// flow sweeps have nothing to carry, the shuffle has no gas to shuffle and conduction has no
// gradient. Any divergence either of these reports is a disease divergence.
//
// The numbers come from the shipped table (build 744825) rather than from taste. Every
// element these two touch carries the same growth row for Food Poisoning — diffusion scale
// 0.001, `minDiffusionCount` 1,000,000, `minDiffusionInfestationTickCount` 1,
// `populationHalfLife` 300 s, `overPopulationHalfLife` 10 s, and population bounds of
// 0.4 and 1000 germs per kilogram — so a 1000 kg cell is crowded above 1,000,000 germs and
// starving below 400, and one below 1,000,000 cannot diffuse at all.

// Diffusion, and the three gates that stop it.
World GermDiffusion(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  World w;
  w.Init(24, 16, granite, 1000.0f, 293.0f);

  // Plain spread: one loaded cell in clean rock. Its four neighbours are clean, so this is
  // the "one side is 0xFF" branch, and it runs four times a substep on the same cell.
  w.SetDisease(3, 3, kFoodPoisoning, 20000000);

  // The same cell with a fresh infestation. `minDiffusionInfestationTickCount` is 1 here, so
  // it may not export on the substep it is seeded on and may from the next one. Without the
  // counter this cell is indistinguishable from the one above.
  w.SetDisease(3, 9, kFoodPoisoning, 20000000, 0);

  // Two loaded cells of the same disease, unequal. This is the branch that moves an eighth
  // of the *difference* and reads the diffusion rate off the richer of the two.
  w.SetDisease(8, 3, kFoodPoisoning, 20000000);
  w.SetDisease(9, 3, kFoodPoisoning, 4000000);

  // Two different diseases side by side, both loaded. Every disease in the shipped table has
  // a `strength` of exactly 0, so `count * strength` is 0 on both sides, the resident wins
  // by the `>` and the incoming germs are discarded — see the note in `AddDiseaseToCell`.
  // The pair is here to hold that behaviour still, not because it is interesting physics.
  w.SetDisease(14, 3, kFoodPoisoning, 20000000);
  w.SetDisease(15, 3, kSlimelung, 20000000);

  // The phase gate. A single gas cell walled in by granite cannot move, and germs may not
  // cross between a gas and a solid however loaded either side is. Gas rather than liquid
  // only because a liquid cell would also make this scenario report the liquid texture,
  // which diverges for reasons that have nothing to do with germs.
  w.Set(19, 3, oxygen, 1000.0f, 293.0f);
  w.SetDisease(19, 3, kFoodPoisoning, 20000000);
  w.SetDisease(18, 3, kFoodPoisoning, 4000000);

  // Radioactive Contaminants: every temperature bound in its row is +inf and its diffusion
  // scale is 0, so it neither spreads nor decays with temperature. The one disease that must
  // sit perfectly still.
  w.SetDisease(3, 13, kRadContaminants, 20000000);
  return w;
}

// Growth and decay, with diffusion switched off by keeping every count under
// `minDiffusionCount`. Insulation 0 everywhere so the temperature blocks cannot conduct into
// each other and each column stays at the temperature it was seeded at for the whole run.
World GermGrowth(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  World w;
  w.Init(24, 16, granite, 1000.0f, 293.0f);
  for (int y = 0; y < w.height; ++y) {
    for (int x = 0; x < w.width; ++x) w.SetInsulation(x, y, 0);
  }

  // One column per temperature band of Food Poisoning, whose range is
  // {248.15, 278.15, 313.15, 348.15}: below minViable, between minViable and minGrowth,
  // inside the growth band, between maxGrowth and maxViable, and above maxViable. The five
  // exits of the temperature sweep, one each.
  const float temps[5] = {240.0f, 260.0f, 293.0f, 330.0f, 360.0f};
  for (int i = 0; i < 5; ++i) {
    const int x = 2 + i * 4;
    const float k = temps[i];
    // Starving: 100 germs in 1000 kg is under 0.4/kg, so they die at a flat 2.667 a second
    // rather than on a half-life.
    w.Set(x, 3, granite, 1000.0f, k);
    w.SetDisease(x, 3, kFoodPoisoning, 100);
    // The same, starved hard enough to actually reach zero inside the run. Without this the
    // sweep that clears an emptied cell is never executed: 100 germs take 188 substeps to
    // die and the suite runs 50, so the whole branch scored a green nobody had earned.
    w.Set(x, 14, granite, 1000.0f, k);
    w.SetDisease(x, 14, kFoodPoisoning, 20);
    // Comfortable: 500,000 in 1000 kg is inside both bounds, so `populationHalfLife` runs.
    w.Set(x, 6, granite, 1000.0f, k);
    w.SetDisease(x, 6, kFoodPoisoning, 500000);
    // Crowded: the same 500,000 in 100 kg is over 1000/kg, so `overPopulationHalfLife` runs
    // instead — a tenth of the mass rather than a different germ count, which is the point.
    w.Set(x, 9, granite, 100.0f, k);
    w.SetDisease(x, 9, kFoodPoisoning, 500000);
    // The disease with no temperature bounds at all, at the same five temperatures. Its
    // half-lives are +inf, so the temperature factor must be exactly 1 in all five columns
    // and only the population rule may touch it.
    w.Set(x, 12, granite, 1000.0f, k);
    w.SetDisease(x, 12, kRadContaminants, 500000);
  }
  return w;
}

// The `properties` byte, which no scenario had ever set to anything but zero. Ten branches
// in `physics.h` read it — seven on `kGasImpermeable`, two on `kLiquidImpermeable`, one on
// `kSolidImpermeable` — and until this world existed every one of them was unreachable, so
// a green suite was silent about all of them. Exactly the hole cell insulation sat in.
//
// Gas and solid only, on purpose: a liquid cell would make this scenario report the liquid
// texture, which diverges for reasons that have nothing to do with cell properties. The two
// `kLiquidImpermeable` branches are still unreached and are noted as such.
// `ModifyCellEnergy`, the third busiest message the game sends and the only one of the top
// five that is not a component. Insulation 0 everywhere, so no cell can conduct into its
// neighbour and every probe below stands alone for the whole run: whatever a cell reads at
// tick 50 is what this message did to it and nothing else.
//
// Granite everywhere at 293.15 K, except for the three cells that test a gate.
World CellEnergy(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(24, 16, granite, 1000.0f, 293.15f);
  for (int y = 0; y < w.height; ++y)
    for (int x = 0; x < w.width; ++x) w.SetInsulation(x, y, 0);

  // Already hotter than the ceiling it will be given. The ceiling is `max(T, maxT)`, so
  // this cell must not be pulled back down to 400 K.
  w.Set(7, 3, granite, 1000.0f, 800.0f);
  // The phase gate: `state & 3`, tested before the mass is even read. Energy poured into
  // vacuum is discarded rather than banked. Seeded at 0 K rather than at the world
  // temperature because a massless cell's temperature is one of the places Klei's answer
  // is not reproducible, and this scenario is not the place to argue about that.
  w.Set(13, 3, vacuum, 0.0f, 0.0f);
  // The mass gate, `mass > 0.001f` strictly. Half a gram of granite is under it.
  w.Set(15, 3, granite, 0.0005f, 293.15f);
  // The 10000 K clamp needs a cell that can be taken there and survive it. Neutronium is
  // the one element in the table with no transition on either side, so this probe reads the
  // clamp and nothing else; granite here would melt at 1210 K and the scenario would be
  // about magma.
  w.Set(17, 3, static_cast<uint16_t>(t.IndexOf(kNeutronium)), 1000.0f, 293.15f);
  return w;
}

// Every arm of the handler, sent on three separate ticks so the one-tick message latency is
// exercised as well. Tick 2 is the ordinary business; tick 5 re-sends into cells that have
// already been written once; tick 8 is the pair that must do nothing.
void CellEnergyTick(Backend* s, const Tables&, const World& w, int tick) {
  if (tick == 2) {
    SendCellEnergy(s, w.Cell(3, 3), 5000.0f, 1000.0f, 1);      // below the ceiling
    SendCellEnergy(s, w.Cell(5, 3), 1.0e9f, 400.0f, 2);        // clamped by the ceiling
    SendCellEnergy(s, w.Cell(7, 3), 5000.0f, 400.0f, 3);       // already past the ceiling
    SendCellEnergy(s, w.Cell(9, 3), 5000.0f, 0.0f, 4);         // no ceiling: a no-op
    SendCellEnergy(s, w.Cell(11, 3), -5000.0f, 1000.0f, 5);    // cooling, ceiling irrelevant
    SendCellEnergy(s, w.Cell(13, 3), 5000.0f, 1000.0f, 6);     // vacuum: the phase gate
    SendCellEnergy(s, w.Cell(15, 3), 5000.0f, 1000.0f, 7);     // 0.5 g: the mass gate
    SendCellEnergy(s, w.Cell(17, 3), 1.0e12f, 1.0e9f, 8);      // past 10000 K: the clamp
  }
  if (tick == 5) {
    // The same cell twice more in one frame. Both are applied, in order, and the second
    // reads the temperature the first left — the ceiling is recomputed per message.
    SendCellEnergy(s, w.Cell(3, 3), 5000.0f, 1000.0f, 9);
    SendCellEnergy(s, w.Cell(3, 3), 5000.0f, 320.0f, 10);
    // Cool one that the clamp has parked at 10000 K.
    SendCellEnergy(s, w.Cell(17, 3), -1.0e9f, 1.0e9f, 11);
  }
  if (tick == 8) {
    // Negative kilojoules have no floor of their own: the only thing that stops this is the
    // `next > 0` test, which refuses the write outright rather than clamping to 1 K.
    SendCellEnergy(s, w.Cell(11, 3), -1.0e9f, 1000.0f, 12);
    // Zero energy against a live ceiling still takes the ceiling path, so a cell below
    // `maxT` is left exactly where it was rather than raised to it.
    SendCellEnergy(s, w.Cell(5, 3), 0.0f, 900.0f, 13);
  }
}

// The other half of the handler: Klei calls `DoStateTransition` on the cell inside the
// drain, before any kernel of the frame has looked at it. So a cell this message boils is
// already steam when the first substep runs, and it conducts as steam for that substep.
//
// Insulation is left at 255 here, which is what makes the two orderings distinguishable:
// with conduction switched off, a transition in the drain and a transition in
// `StepStateChange` would land in the same place.
World CellEnergyPhase(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  World w;
  w.Init(24, 16, granite, 1000.0f, 293.15f);
  // Solid -> solid, so the liquid textures stay out of it: Peat crosses at 500 K.
  w.Set(5, 8, static_cast<uint16_t>(t.IndexOf(kPeat)), 100.0f, 400.0f);
  // The same arm two elements further along, at 1200 K rather than 500 K: Clay -> Ceramic.
  w.Set(11, 8, static_cast<uint16_t>(t.IndexOf(kClay)), 100.0f, 1100.0f);
  // Toxic Mud crosses at 372.84 K into a gas **and names a transition ore**, so this is the
  // probe that says a transition taken inside the drain still gets its `spawnOreInfo` into
  // the frame — `ClearFrameEvents` runs before the drain, and if it did not the ore would
  // be swept away before anything published it. It is also Unstable, and it stays put here
  // only because the cell under it is granite.
  w.Set(17, 8, static_cast<uint16_t>(t.IndexOf(kToxicMud)), 100.0f, 350.0f);
  // ...and its insulation is 0, so the steam it becomes cannot conduct into the granite
  // around it. With conduction on it cools back through 372.65 K a few ticks later and
  // condenses, and this scenario would be about the liquid textures instead.
  w.SetInsulation(17, 8, 0);
  return w;
}

void CellEnergyPhaseTick(Backend* s, const Tables&, const World& w, int tick) {
  if (tick == 2) {
    SendCellEnergy(s, w.Cell(5, 8), 20000.0f, 5000.0f, 1);      // Peat, past 500 K
    SendCellEnergy(s, w.Cell(11, 8), 40000.0f, 5000.0f, 2);     // Clay, past 1200 K
    SendCellEnergy(s, w.Cell(17, 8), 5000.0f, 5000.0f, 3);      // Toxic Mud, past 372.84 K
  }
}

// `SetInsulationValue`, which **no scenario had ever sent**. The suite's insulation
// coverage all arrives in the world payload, where the value is already a byte, so the
// float-to-byte conversion in the message handler had never been scored at all — and it was
// wrong in three separate ways at once (it rounded, it clamped, and its sibling scaled
// strength by 255 when Klei stores strength raw).
//
// Granite everywhere so conduction is the only kernel running, and the insulated cell is the
// only thing between a hot cell and the cold rock beyond it, exactly as in `insulated`. Each
// row gets a different value, and the three that matter are the ones a rounding
// implementation gets wrong by exactly one:
//
//   0.5  -> 127 truncated, 128 rounded
//   0.25 ->  63 truncated,  64 rounded
//   1.5  -> 382, which is 126 in the low byte — a clamp would say 255
//  -0.5  -> -127, which is 129 in the low byte — a clamp would say 0
World SetInsulation(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  World w;
  w.Init(24, 16, granite, 1000.0f, 300.0f);
  for (int i = 0; i < 5; ++i) w.Set(3, 2 + i * 3, granite, 1000.0f, 500.0f);
  return w;
}

void SetInsulationTick(Backend* s, const Tables&, const World& w, int tick) {
  if (tick != 2) return;
  const float values[5] = {0.5f, 0.25f, 1.0f, 1.5f, -0.5f};
  for (int i = 0; i < 5; ++i) {
    SendCellFloat(s, SimMessageHash::SetInsulationValue, w.Cell(4, 2 + i * 3), values[i]);
    // Sent alongside, and unobservable: the strength byte is projected but nothing in
    // `GameDataUpdate` publishes it. It is here so the message is at least exercised.
    SendCellFloat(s, SimMessageHash::SetStrengthValue, w.Cell(6, 2 + i * 3), values[i] * 100.0f);
  }
}

// The drain **order**. Klei does not process queued messages in the order they arrived: it
// sorts them into per-category vectors on the way in and `SimFrameManager::ProcessFrame`
// walks those categories in a fixed order — insulation and strength, then
// `ProcessCellEnergyModifications`, then properties, mass consumption, mass emission,
// disease, radiation, dig points, and `ProcessCellModifications` last of all.
//
// So a `ModifyCellEnergy` always lands **before** a `ModifyCell` from the same frame, no
// matter which the game sent first. This scenario is the only thing in the suite that can
// tell an arrival-order drain from a category-order one, because it is the only one that
// sends two categories in one frame at the same cell.
World MessageOrder(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  World w;
  w.Init(24, 16, granite, 1000.0f, 300.0f);
  for (int y = 0; y < w.height; ++y)
    for (int x = 0; x < w.width; ++x) w.SetInsulation(x, y, 0);
  return w;
}

void MessageOrderTick(Backend* s, const Tables& t, const World& w, int tick) {
  if (tick == 2) {
    // Sent modify-then-energy. Klei runs them energy-then-modify, so the energy lands on
    // the granite that is about to be thrown away and the cell ends up at exactly 500 K.
    SendModifyCell(s, t, w.Cell(5, 5), kCopper, 500.0f, 500.0f);
    SendCellEnergy(s, w.Cell(5, 5), 100000.0f, 5000.0f, 1);
    // Sent energy-then-modify, which is the order Klei runs anyway. Both orderings agree
    // here, which is what makes the pair a control rather than a single reading.
    SendCellEnergy(s, w.Cell(9, 5), 100000.0f, 5000.0f, 2);
    SendModifyCell(s, t, w.Cell(9, 5), kCopper, 500.0f, 500.0f);
    // The same question one category further out: an insulation write is processed before
    // the energy modification whichever way round it is sent, and the insulated cell's
    // conduction for the rest of the run is the proof.
    SendCellEnergy(s, w.Cell(13, 5), 100000.0f, 5000.0f, 3);
    SendCellFloat(s, SimMessageHash::SetInsulationValue, w.Cell(13, 5), 1.0f);
  }
}

// `ModifyCell`, the three `replaceType` values and the clamp in front of them. Solid world,
// no liquids anywhere: the add-path displacement scenarios below need fluids, but the three
// *replacement* paths and the message's own arithmetic do not, and keeping them out means
// this scenario is not also a test of the liquid textures.
//
// Insulation is zeroed so conduction runs. A message that writes the wrong temperature into
// one cell shows up in its neighbours a tick later, which is what makes a wrong clamp
// visible at all — the cell itself would be overwritten again by the next transition.
World ModifyCellPaths(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  World w;
  w.Init(24, 16, granite, 1000.0f, 300.0f);
  for (int y = 0; y < w.height; ++y)
    for (int x = 0; x < w.width; ++x) w.SetInsulation(x, y, 0);
  return w;
}

void ModifyCellPathsTick(Backend* s, const Tables& t, const World& w, int tick) {
  if (tick == 2) {
    // replaceType 1, a different element: the ordinary case.
    SendModifyCell(s, t, w.Cell(2, 3), kCopper, 500.0f, 500.0f, 1);
    // replaceType 1 with the element the cell already holds. The cell is still overwritten
    // and — the part a guess gets wrong — still announced as a substance change.
    SendModifyCell(s, t, w.Cell(4, 3), kGranite, 800.0f, 400.0f, 1);
    // replaceType 1 writing Vacuum. Both the mass and the temperature are forced to zero
    // whatever the message asked for.
    SendModifyCell(s, t, w.Cell(6, 3), kVacuum, 500.0f, 500.0f, 1);
    // replaceType 2 into a solid. There is no fluid to displace, so it should land on the
    // same answer as replaceType 1 — the control for the pair below.
    SendModifyCell(s, t, w.Cell(8, 3), kCopper, 500.0f, 500.0f, 2);
    // The two clamps, which are not the same clamp. replaceType 2 clamps unconditionally
    // inside `ReplaceAndDisplaceElement`; replaceType 1 relies on the caller's, which only
    // fires when the message is already out of range. Both end at 10000 K here.
    // Neutronium rather than copper: copper melts at 1357 K, and a molten probe would make
    // this a test of the liquid textures instead. Neutronium's only transition points back
    // at itself, so 10000 K leaves it exactly where the clamp put it.
    SendModifyCell(s, t, w.Cell(10, 3), kNeutronium, 500.0f, 20000.0f, 2);
    SendModifyCell(s, t, w.Cell(12, 3), kNeutronium, 500.0f, 20000.0f, 1);
    // Negative temperature with positive mass trips the caller's clamp the other way: the
    // temperature goes to zero and the mass is kept.
    SendModifyCell(s, t, w.Cell(14, 3), kCopper, 100.0f, -50.0f, 1);
    // replaceType 0 with a negative mass is a *removal*, and it only acts when the phase of
    // the element named in the message matches the phase of the element in the cell.
    SendModifyCell(s, t, w.Cell(16, 3), kGranite, -400.0f, 300.0f, 0);
    // Same removal, wrong phase. Oxygen is a gas and the cell is solid, so nothing happens.
    SendModifyCell(s, t, w.Cell(18, 3), kOxygen, -400.0f, 300.0f, 0);
    // replaceType 0 adding the element already there, `OnlyIfSameElement`. A plain merge.
    SendModifyCell(s, t, w.Cell(20, 3), kGranite, 500.0f, 900.0f, 0, 1);
    // `OnlyIfSameElement` with a *different* solid refuses the message outright.
    SendModifyCell(s, t, w.Cell(2, 7), kCopper, 500.0f, 900.0f, 0, 1);
    // A zero-mass removal. Solids enter the removal branch only on a strictly negative
    // mass, so this one is a no-op — the gas branch would have cleared the cell.
    SendModifyCell(s, t, w.Cell(4, 7), kGranite, 0.0f, 300.0f, 0);
    // Adding Vacuum is dropped on the floor: `ProcessCellModifications` reports "Invalid
    // replacement type" and returns.
    SendModifyCell(s, t, w.Cell(6, 7), kVacuum, 500.0f, 500.0f, 0);
    // An `addSubType` outside the two the enum defines makes `AddSolid` a no-op. 7 is not
    // a value the game ever sends; the point is that the sim ignores it silently.
    SendModifyCell(s, t, w.Cell(8, 7), kGranite, 500.0f, 900.0f, 0, 7);
    // Where the 1 K floor lives, and what its shape is. Every one of these is inside the
    // range the caller's clamp tests, so that clamp never fires and `ReplaceElement` writes
    // the number straight into the cell — whatever comes back has been changed by something
    // else in the frame.
    const float probe[6] = {0.1f, 0.5f, 0.9f, 1.0f, 1.5f, 2.0f};
    for (int i = 0; i < 6; ++i) {
      SendModifyCell(s, t, w.Cell(10 + 2 * i, 7), kGranite, 100.0f, probe[i], 1);
    }
  }
  if (tick == 6) {
    // Does the floor clamp the grid, or only what the grid publishes? Mixing a known mass
    // into the 0.5 K cell answers it: 100 kg at 300.5 K on top of 100 kg comes out at
    // 150.5 K if the cell really held 0.5, and at 150.75 K if something had already raised
    // it to 1. Nothing else in the suite can ask this question, because nothing else has
    // ever put a cell below 1 K.
    SendModifyCell(s, t, w.Cell(12, 7), kGranite, 100.0f, 300.5f, 0, 1);
    // Take the rest of the cell away. The remainder lands under the smallest normal float,
    // so the cell is cleared to vacuum and announced.
    SendModifyCell(s, t, w.Cell(16, 3), kGranite, -600.0f, 300.0f, 0);
  }
}

// `ModifyCell` with `replaceType` 0 and a **gas**: `AddGas` and everything it does when the
// cell it was aimed at will not take the gas. Nothing here is reachable from any other
// message — the emitters all pour into cells that already hold their own gas, which is the
// one branch of this function that is a plain merge.
World CellModGas(const Tables& t) {
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t hydrogen = static_cast<uint16_t>(t.IndexOf(kHydrogen));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(24, 16, oxygen, 1.0f, 293.0f);
  // A lone solid with a vacuum on its left: the blocked scan should fill the vacuum.
  w.Set(11, 3, granite, 100.0f, 300.0f);
  w.Set(10, 3, vacuum, 0.0f, 0.0f);
  // A lone solid with the incoming gas already standing on its right and nothing on its
  // left but ordinary oxygen. The scan runs left before right, so this scores the order.
  w.Set(15, 3, granite, 100.0f, 300.0f);
  w.Set(16, 3, hydrogen, 3.0f, 300.0f);
  // Two sealed blocks, so the scan finds no vacuum and no matching gas and has to fall into
  // the tail that destroys mass. The first is heavier than the gas being pushed at it and
  // the second is lighter, which are the tail's two different answers.
  for (int y = 2; y <= 4; ++y)
    for (int x = 17; x <= 19; ++x) w.Set(x, y, granite, 1000.0f, 300.0f);
  w.Set(18, 3, granite, 100.0f, 300.0f);
  for (int y = 9; y <= 11; ++y)
    for (int x = 4; x <= 6; ++x) w.Set(x, y, granite, 1000.0f, 300.0f);
  w.Set(5, 10, granite, 5.0f, 300.0f);
  return w;
}

void CellModGasTick(Backend* s, const Tables& t, const World& w, int tick) {
  if (tick == 2) {
    // The merge. Same gas, so no displacement and no substance announcement.
    SendModifyCell(s, t, w.Cell(3, 3), kOxygen, 2.0f, 500.0f, 0);
    // A different gas into a gas cell: the oxygen is displaced into a neighbour and the
    // cell becomes hydrogen. This is the branch that makes `AddGas` a mover rather than an
    // accumulator.
    SendModifyCell(s, t, w.Cell(7, 3), kHydrogen, 2.0f, 500.0f, 0);
    // Into a solid, which is never displaced. The scan is left, right, up — and a vacuum
    // neighbour is *overwritten* outright rather than merged into, so this cell ends at
    // exactly 2 kg and 500 K rather than at a mixture.
    SendModifyCell(s, t, w.Cell(11, 3), kHydrogen, 2.0f, 500.0f, 0);
    // Same again, but the only taker is a neighbour already holding hydrogen, and it is on
    // the right. A merge, so the temperature comes out between the two.
    SendModifyCell(s, t, w.Cell(15, 3), kHydrogen, 2.0f, 500.0f, 0);
    // Sealed, and the gas weighs less than the rock in the way: the gas is destroyed and
    // the rock is thinned by exactly the mass that was pushed at it.
    SendModifyCell(s, t, w.Cell(18, 3), kHydrogen, 10.0f, 500.0f, 0);
    // Sealed, and the gas outweighs the rock: the rock is emptied, the cell takes the gas,
    // and only the remainder — 10 kg less the 5 kg displaced — is actually added.
    SendModifyCell(s, t, w.Cell(5, 10), kHydrogen, 10.0f, 500.0f, 0);
    // The removal branch, which for a gas is entered on `mass <= 0` rather than `mass < 0`.
    SendModifyCell(s, t, w.Cell(10, 10), kOxygen, -0.5f, 293.0f, 0);
    // The same removal taking the whole cell: the remainder lands under the smallest normal
    // float, so the cell is cleared to vacuum and announced.
    SendModifyCell(s, t, w.Cell(14, 10), kOxygen, -1.0f, 293.0f, 0);
    // A removal aimed at a cell whose phase does not match the message's element. Nothing
    // happens: the gas branch checks the *cell's* phase before it subtracts.
    SendModifyCell(s, t, w.Cell(18, 10), kOxygen, -0.5f, 293.0f, 0);
  }
}

// `AddSolid`, which is the only one of the three add paths that reads `addSubType`, and the
// two modes have nothing in common.
World CellModSolid(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(24, 16, granite, 1000.0f, 300.0f);
  // Open shafts above three of the probes, so the upward scan has somewhere to arrive.
  for (int y = 4; y < 14; ++y) {
    w.Set(3, y, vacuum, 0.0f, 0.0f);
    w.Set(7, y, oxygen, 1.0f, 300.0f);
  }
  // A column of the same element under `maxMass`, which is what the vertical mode tops up
  // before it goes looking for a cell to land in. Granite's cap is 1840 kg.
  w.Set(11, 2, granite, 1700.0f, 400.0f);
  w.Set(11, 3, granite, 1800.0f, 400.0f);
  // A gas cell for `OnlyIfSameElement`, which displaces the gas and then never writes the
  // element it was given.
  w.Set(15, 3, oxygen, 1.0f, 300.0f);
  return w;
}

void CellModSolidTick(Backend* s, const Tables& t, const World& w, int tick) {
  if (tick == 2) {
    // Vertical mode into solid rock with a vacuum shaft above: the two top-up cells hold a
    // different element, so all 500 kg goes to the first non-solid cell the upward scan
    // reaches, as an overwrite rather than an add.
    SendModifyCell(s, t, w.Cell(3, 3), kGranite, 500.0f, 900.0f, 0, 0);
    // The same, but the shaft holds oxygen: the gas is displaced out of the way first.
    SendModifyCell(s, t, w.Cell(7, 3), kGranite, 500.0f, 900.0f, 0, 0);
    // The top-up half. `y - 1` and `y` both already hold granite below the cap, so 140 kg
    // fills the lower one and the remaining 360 goes into the upper one, which can take 40
    // — and only what is left after that goes looking for a cell.
    SendModifyCell(s, t, w.Cell(11, 3), kGranite, 500.0f, 900.0f, 0, 0);
    // `OnlyIfSameElement` into a gas cell. The gas is displaced and the mass is added, but
    // Klei never writes the element, so the cell is left as the vacuum the displacement put
    // there — holding 500 kg of it.
    SendModifyCell(s, t, w.Cell(15, 3), kGranite, 500.0f, 900.0f, 0, 1);
  }
}

// `AddLiquid`, and with it the two primitives nothing else in the project reaches:
// `DisplaceLiquid` and `DisplaceLiquidSimple`. A liquid pushed
// at an occupied cell splits evenly between whichever neighbours will take it, and the
// path that gets there has a step no other add path has — a liquid aimed at solid rock
// **retargets to a neighbour** before anything else happens.
//
// Solid rock everywhere with pockets carved into it, so each probe is isolated from the
// next and the liquid kernels have nowhere to carry a mistake.
World CellModLiquid(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t other = static_cast<uint16_t>(t.IndexOf(kWideLiquid));
  World w;
  w.Init(24, 16, granite, 1000.0f, 300.0f);
  // The merge control.
  w.Set(3, 3, water, 100.0f, 300.0f);
  // A gas cell with gas around it, so the displacement has somewhere to go.
  w.Set(7, 3, oxygen, 1.0f, 300.0f);
  w.Set(6, 3, oxygen, 1.0f, 300.0f);
  w.Set(8, 3, oxygen, 1.0f, 300.0f);
  w.Set(7, 4, oxygen, 1.0f, 300.0f);
  // The retarget. `AddLiquid` looks left, right, down, up and takes the first non-solid,
  // and the only non-solid here is *down* — which is third in that order, so a scan that
  // ran in any other order would land somewhere else.
  w.Set(11, 2, oxygen, 1.0f, 300.0f);
  // Sealed rock, heavier than the liquid pushed at it: thinned, and the liquid destroyed.
  w.Set(15, 3, granite, 500.0f, 300.0f);
  // Sealed rock weighing exactly what is pushed at it. The remainder is zero, which is the
  // one input that reaches `AddMassAndUpdateTemperature`'s zero branch — the branch that
  // clears the temperature and the disease and leaves the mass alone. `AddLiquid` then
  // writes the vacuum element over the cell **without** clearing it, which is where it
  // differs from `AddGas`.
  w.Set(15, 10, granite, 50.0f, 300.0f);
  // A different liquid, with room around it for `DisplaceLiquid` to split it into.
  w.Set(19, 3, other, 100.0f, 300.0f);
  w.Set(18, 3, oxygen, 1.0f, 300.0f);
  w.Set(20, 3, oxygen, 1.0f, 300.0f);
  w.Set(19, 4, oxygen, 1.0f, 300.0f);
  // An attempt at the swap branch, which is the one thing in `AddLiquid` that no scenario
  // here reaches — see the note on it in `sim/cellmod.h`. The probe is kept because the
  // path it *does* take, a liquid pushed at a cell with nowhere to put it, is worth having.
  w.Set(18, 7, oxygen, 1.0f, 300.0f);
  w.Set(18, 8, water, 100.0f, 320.0f);
  return w;
}

void CellModLiquidTick(Backend* s, const Tables& t, const World& w, int tick) {
  if (tick == 2) {
    SendModifyCell(s, t, w.Cell(3, 3), kWater, 50.0f, 400.0f, 0);
    SendModifyCell(s, t, w.Cell(7, 3), kWater, 50.0f, 400.0f, 0);
    SendModifyCell(s, t, w.Cell(11, 3), kWater, 50.0f, 400.0f, 0);
    SendModifyCell(s, t, w.Cell(15, 3), kWater, 50.0f, 400.0f, 0);
    SendModifyCell(s, t, w.Cell(15, 10), kWater, 50.0f, 400.0f, 0);
    SendModifyCell(s, t, w.Cell(19, 3), kWater, 50.0f, 400.0f, 0);
  }
  // Tick 0 rather than tick 2, which is a fact about the world rather than about the
  // message: liquid standing on top of gas is exactly the arrangement the liquid mover
  // exists to undo, so by tick 2 the water has already fallen and the message finds water
  // where the world put gas. Sending on tick 0 is the earliest the harness can send, and
  // even that is not early enough — but it is a different path from tick 2 and it is the
  // one that reaches the tail where a liquid with nowhere to go thins the cell in its way.
  if (tick == 0) {
    SendModifyCell(s, t, w.Cell(18, 7), kWater, 50.0f, 400.0f, 0);
  }
}

World CellProperties(const Tables& t) {
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  World w;
  w.Init(24, 16, oxygen, 1.0f, 293.0f);

  // Five times the pressure on the left, so the partition has something to hold back. With
  // the bit ignored the two halves equalise within the run; with it honoured they cannot.
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 12; ++x) w.Set(x, y, oxygen, 5.0f, 293.0f);
  }

  // A full-height partition of gas-impermeable *gas*. In a real colony the bit rides on a
  // solid building tile, but the sim reads the byte and consults nothing else, so putting it
  // on gas isolates the property from every other difference a tile would bring with it.
  for (int y = 0; y < 16; ++y) w.SetProperties(12, y, kGasImpermeable);

  // One hole, so the run also proves the byte is read per cell rather than per column: the
  // pressure must equalise through this row and nowhere else.
  w.SetProperties(12, 7, 0);

  // A single impermeable cell inside the open high-pressure field. This is the destination
  // gate in `DisplaceGas` rather than the partition's flow gate.
  w.SetProperties(5, 8, kGasImpermeable);

  // The two bits the padded border carries, on an interior solid. Granite is going nowhere
  // either way, so this is here to show the bits do not perturb a cell that was already at
  // rest — a silent no-op is the correct result and the ablation table records it as such.
  w.Set(20, 8, granite, 1000.0f, 293.0f);
  w.SetProperties(20, 8, static_cast<uint8_t>(kSolidImpermeable | kUnbreakable));
  return w;
}

// The backwall, which no scenario had ever sent anything but `SimBackwall{}` for. It is a
// second material behind the cell — the tile you see through a dug-out room — and on our
// side it is a pure store: loaded, projected, written by `ModifyBackwallData`, read by no
// kernel. Klei publishes two event arrays for it that we have never filled in, so the
// question this world asks is whether its backwall is passive too.
//
// Insulation 0 everywhere, the `germgrow` trick: cell-to-cell conduction is off, so every
// column keeps the temperature it was seeded with and *any* temperature that moves came
// from the backwall. Granite at rest otherwise, so the flow sweeps carry nothing.
World Backwall(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(20, 12, granite, 1000.0f, 300.0f);
  for (size_t i = 0; i < w.Count(); ++i) w.insulation[i] = 0;

  for (int y = 1; y < 11; ++y) {
    // Hot backwall against a cold cell, and the reverse. 700 K either way is far more than
    // any conduction rule could hide inside the 1e-2 K compare tolerance.
    for (int x = 2; x <= 3; ++x) w.SetBackwall(x, y, granite, 200.0f, 1000.0f);
    for (int x = 6; x <= 7; ++x) {
      w.Set(x, y, granite, 1000.0f, 800.0f);
      w.SetBackwall(x, y, granite, 200.0f, 100.0f);
    }

    // A backwall held outside its own phase range: water 27 K over boiling, and water 73 K
    // under freezing. If anything transitions a backwall, this is what it looks like.
    w.SetBackwall(10, y, water, 100.0f, 400.0f);
    w.SetBackwall(12, y, water, 100.0f, 200.0f);

    // Backwall behind vacuum. If backwall mass can enter the grid at all, an empty cell is
    // where it would show, and the ledger would see it as an unexplained source.
    w.Set(14, y, vacuum, 0.0f, 0.0f);
    w.SetBackwall(14, y, granite, 200.0f, 500.0f);

    // Two gates the announcement might have and the columns above cannot separate: a
    // massless backwall held over granite's 942 K, and a Vacuum backwall carrying mass at
    // the same temperature. Whether either announces says whether the test is on the
    // element alone or on the element and its mass.
    w.SetBackwall(16, y, granite, 0.0f, 1000.0f);
    w.SetBackwall(17, y, vacuum, 200.0f, 20000.0f);
  }
  // Control: cells with no backwall at all, which is what the other 59 scenarios send.
  return w;
}

// -------------------------------------------------------------- the two element components

// The element consumer, in a world built so that each of its four configurations is alone
// with its own probe. Every room is sealed with granite and separated from the others, so a
// consumer's flood cannot reach anything but the pocket it sits in.
//
// Conduction is switched off by insulation 0 everywhere, so the only thing that can move a
// number here is a component.
World ElementConsume(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t hydrogen = static_cast<uint16_t>(t.IndexOf(kHydrogen));
  const uint16_t liquid = static_cast<uint16_t>(t.IndexOf(kWater));
  World w;
  w.Init(28, 12, granite, 1000.0f, 300.0f);
  w.insulation.assign(w.insulation.size(), 0);
  // Every gas pocket here is **one cell**, and that is not a stylistic choice. Two gas cells
  // at unequal masses are enough to reach Klei's random gas shuffle, which is not modelled
  // (see `gasroom` and `tracer`, both of which exist to detect it and neither of which is
  // scored) — a two-cell pocket swapped its two masses at tick 33 and a three-cell one at
  // tick 16. The multi-cell flood is covered by the granite consumer instead, because solid
  // cells do not shuffle.
  //
  // Pocket A: **one** oxygen cell, for `configuration 0` against its own element.
  w.Set(3, 3, oxygen, 2.0f, 320.0f);
  // Pocket B: **one** hydrogen cell, for `AllGas`, which is the configuration that asks the
  // reachable list what is in it rather than being told. One cell rather than two on
  // purpose: a pocket holding two different gases puts Klei's random gas shuffle between the
  // consumer and the result, and the shuffle is not modelled here (see `gasroom`).
  //
  // The rate empties this cell part way through the run, which is the other thing worth
  // covering: once it is vacuum, `AnyInputCellHasState` finds nothing, the consumer takes
  // nothing and — the part a rewrite gets wrong — does not advance its offset either.
  w.Set(9, 3, hydrogen, 2.0f, 360.0f);
  // Pocket C: gas again, for the invalid configuration byte, which must do nothing at all.
  w.Set(21, 3, oxygen, 2.0f, 300.0f);
  (void)liquid;
  return w;
}

// Five consumers, and the handles come back 0..4 in this order.
void ElementConsumeSetup(Backend* s, const Tables& t, const World& w) {
  SendAddConsumer(s, t, w.Cell(3, 3), kOxygen, 3, 0, 200);
  SendAddConsumer(s, t, w.Cell(9, 3), kOxygen, 3, 2, 201);
  // Handle 2 is deliberately a consumer that can never find anything: it sits in solid
  // rock asking for a liquid, so `AnyInputCellHasState` returns false on every substep and
  // the consumer neither drains nor advances its offset.
  SendAddConsumer(s, t, w.Cell(6, 8), kWater, 3, 1, 202);
  SendAddConsumer(s, t, w.Cell(21, 3), kOxygen, 3, 7, 203);
  // The fifth one drains solid granite, which is the branch `FloodRemoved` reaches only
  // when the element being removed is itself a solid: it floods through solids and refuses
  // everything else, the exact opposite of the other four.
  SendAddConsumer(s, t, w.Cell(25, 8), kGranite, 3, 0, 204);
}

void ElementConsumeTick(Backend* s, const Tables&, const World& w, int tick) {
  if (tick == 3) {
    SendConsumerData(s, 0, w.Cell(3, 3), 0.5f);
    SendConsumerData(s, 1, w.Cell(9, 3), 0.5f);
    SendConsumerData(s, 2, w.Cell(6, 8), 2.0f);
    SendConsumerData(s, 3, w.Cell(21, 3), 0.5f);
    SendConsumerData(s, 4, w.Cell(25, 8), 300.0f);
  }
  // A rate of zero, which is the state a consumer is registered in: it still runs, still
  // finds an element and still bumps its offset, and takes nothing.
  if (tick == 20) SendConsumerData(s, 0, w.Cell(3, 3), 0.0f);
  // And a removal, so the allocator's free list is exercised with something in it.
  if (tick == 30) SendRemoveConsumer(s, 3, 213);
}

// A consumer whose flood crosses a cell of its OWN element holding no mass, which is the
// arrangement that makes `do_remove`'s running mix divide 0 by 0.
//
// `do_remove` writes the running total `info->mass` BEFORE the temperature it is
// the denominator of, and computes `(mass_so_far * t_info + t_cell * take) / total`. On a cell
// of the right element holding nothing, `take` and `total` are both 0 and the quotient is a
// NaN — and Klei then loses it, because its clamp returns the bound whenever either
// operand is a NaN, so the mix comes out as the clamp bound.
// Written as `if (mix > hi)` in C++ the comparison is false for a NaN, the NaN survives, the
// next cell's `mass_so_far * t_info` becomes `0 * NaN`, and a drain that took real mass
// reports a NaN temperature to the game. The game logs exactly that:
// `GameUtil.GetFinalTemperature: t2=NaN` under `ElementConsumer.AddMass`, twice, with
// `PrimaryElement.SetTemperature` refusing the value behind it.
//
// GRANITE AND NOT A GAS, and the reason is not style. A massless cell of a GAS does not
// survive the load on either side — both sims come up with vacuum there and the drain never
// sees it — while a massless SOLID cell loads as written. Solids also do not move, so this
// avoids the second hazard as well: a massless gas cell beside a full one is exactly the
// unequal pair that reaches Klei's unmodelled random shuffle (see `econsume`).
//
// THE SECOND DIVERGENCE THIS ARM CARRIED IS ALSO FIXED. It read `tick 2 solidInfo
// klei 1, mine 0` and was recorded here as an unexplained first-frame solidity gap, on the
// grounds that it appeared with the consumer's rate left at zero and so was not part of what
// this scenario is for. Chasing it found the rule rather than an edge: Klei's solidity rule
// at the top of `CopySimDataToGame` does not involve MASS
// AT ALL — if the element index changed, it compares `IsSolid(old)` against `IsSolid(new)` and
// announces the difference. `sim/projection.h`'s `SolidFraction` tested `mass > 0.0f &&
// IsSolid(...)`, so a solid cell drained to nothing read as hollow here and as solid there, and
// the announcement the game needs to refresh `Grid.IsSolidCell` was never sent. Both halves of
// this scenario are therefore live arms now and neither is a standing exception.
World ElementConsumeEmpty(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  World w;
  w.Init(12, 8, granite, 1000.0f, 300.0f);
  w.insulation.assign(w.insulation.size(), 0);
  // Element granite, mass zero: the cell the flood reaches first and takes nothing from. Its
  // 300 K is the background's, so a mix that goes wrong cannot hide as a plausible number --
  // the neighbours it then drains are all the same temperature, and the report must be 300 K
  // exactly.
  w.Set(5, 4, granite, 0.0f, 300.0f);
  return w;
}

void ElementConsumeEmptySetup(Backend* s, const Tables& t, const World& w) {
  // Configuration 0 (this element and no other), sitting IN the empty cell so the flood
  // reaches it before anything with mass in it.
  SendAddConsumer(s, t, w.Cell(5, 4), kGranite, 3, 0, 220);
}

void ElementConsumeEmptyTick(Backend* s, const Tables&, const World& w, int tick) {
  if (tick == 1) SendConsumerData(s, 0, w.Cell(5, 4), 300.0f);
}

// The element emitter. Same shape: sealed pockets, insulation 0, one behaviour per pocket.
World ElementEmit(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t hydrogen = static_cast<uint16_t>(t.IndexOf(kHydrogen));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(28, 12, granite, 1000.0f, 300.0f);
  w.insulation.assign(w.insulation.size(), 0);
  // One cell per pocket, for the reason `econsume` gives: two gas cells at unequal masses
  // are enough to reach Klei's unmodelled gas shuffle, and an emitter makes them unequal on
  // its first substep. The displacement path needs two and lives in `eemitdisp`.
  //
  // Pocket A: vacuum, so the first emission is the substance announcement as well.
  w.Set(3, 3, vacuum, 0.0f, 0.0f);
  // Pocket B: the emitter's own gas already there, which is the path that announces nothing.
  w.Set(9, 3, oxygen, 0.5f, 300.0f);
  // Pocket C: a *different* gas with nowhere to put it — the pocket is one cell — so the
  // displacement fails and the emitter finds no cell it may write.
  w.Set(14, 3, hydrogen, 0.5f, 300.0f);
  // Pocket D: over the pressure ceiling from the start, so this emitter is blocked on its
  // very first update and reports it.
  w.Set(20, 3, oxygen, 50.0f, 300.0f);
  return w;
}

void ElementEmitSetup(Backend* s, const Tables&, const World&) {
  SendAddEmitter(s, 2.0f, 300, 310, 320);
  SendAddEmitter(s, 2.0f, 301, 311, 321);
  SendAddEmitter(s, 2.0f, 302, 312, 322);
  SendAddEmitter(s, 2.0f, 303, 313, 323);
}

void ElementEmitTick(Backend* s, const Tables& t, const World& w, int tick) {
  if (tick == 3) {
    // Interval shorter than a substep, so this one fires on every substep — and only once,
    // because the interval is a subtraction rather than a loop.
    SendModifyEmitter(s, t, 0, w.Cell(3, 3), kOxygen, 0.05f, 0.1f, 350.0f, 2.0f, 2);
    // Interval longer than a substep: fires on some ticks and not others, which is the only
    // way to see the carry.
    SendModifyEmitter(s, t, 1, w.Cell(9, 3), kOxygen, 0.5f, 0.1f, 350.0f, 2.0f, 2);
    // Into a foreign gas that cannot be displaced anywhere: the emitter runs, finds room
    // by mass, reports itself unblocked, and still emits nothing.
    SendModifyEmitter(s, t, 2, w.Cell(14, 3), kOxygen, 0.2f, 0.1f, 350.0f, 2.0f, 1);
    // Blocked from the start, and never unblocks.
    SendModifyEmitter(s, t, 3, w.Cell(20, 3), kOxygen, 0.2f, 0.1f, 350.0f, 2.0f, 1);
  }
  // A negative emit temperature, which is how the game says "use the element's default".
  if (tick == 20) {
    SendModifyEmitter(s, t, 1, w.Cell(9, 3), kOxygen, 0.5f, 0.1f, -1.0f, 4.0f, 2);
  }
  // Zero mass, which forces the blocked state back to its third value and so makes the very
  // next update fire a callback whichever way it goes.
  if (tick == 30) {
    SendModifyEmitter(s, t, 0, w.Cell(3, 3), kOxygen, 0.05f, 0.0f, 350.0f, 2.0f, 2);
  }
}

// A **solid** emitter, which is a different subsystem wearing the same name: it never puts
// anything in the grid and hands the game a `SpawnOreInfo` per reachable cell instead.
//
// Three of them: one with `maxDepth` 1 so it can only reach its own cell, and two whose
// reachable sets **overlap**, which is the only way to get two lumps of ore into one cell in
// one frame — and that is what the export's sort-and-merge is for.
World ElementEmitSolid(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  World w;
  w.Init(24, 12, granite, 1000.0f, 300.0f);
  w.insulation.assign(w.insulation.size(), 0);
  for (int x = 2; x <= 4; ++x) w.Set(x, 3, oxygen, 0.5f, 300.0f);
  for (int x = 10; x <= 14; ++x) w.Set(x, 3, oxygen, 0.5f, 300.0f);
  return w;
}

void ElementEmitSolidSetup(Backend* s, const Tables&, const World&) {
  SendAddEmitter(s, 2.0f, 400, 410, 420);
  SendAddEmitter(s, 2.0f, 401, 411, 421);
  SendAddEmitter(s, 2.0f, 402, 412, 422);
}

void ElementEmitSolidTick(Backend* s, const Tables& t, const World& w, int tick) {
  if (tick == 3) {
    SendModifyEmitter(s, t, 0, w.Cell(3, 3), kGranite, 0.4f, 5.0f, 400.0f, 2.0f, 1);
    // Two emitters two cells apart, each reaching two steps, so `(12,3)` is in both lists
    // and receives two lumps of granite on the same substep. Different temperatures, so the
    // merged entry is a mix rather than either of them.
    SendModifyEmitter(s, t, 1, w.Cell(11, 3), kGranite, 0.4f, 5.0f, 400.0f, 2.0f, 2);
    SendModifyEmitter(s, t, 2, w.Cell(13, 3), kGranite, 0.4f, 15.0f, 800.0f, 2.0f, 2);
  }
}

// The emitter's displacement path, which needs two cells and therefore cannot live in
// `eemit`: an emitter pushing gas into a pocket makes its cells unequal, and two unequal gas
// cells are enough to reach Klei's unmodelled random shuffle.
//
// So this one is deliberately short-lived. The emitter fires once, at an interval long
// enough that it never fires again, and everything the scenario is about has happened by
// tick 6. It is registered with an envelope all the same, because the displacement is what
// is being scored and it is exact.
World ElementEmitDisplace(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t hydrogen = static_cast<uint16_t>(t.IndexOf(kHydrogen));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(20, 10, granite, 1000.0f, 300.0f);
  w.insulation.assign(w.insulation.size(), 0);
  // A foreign gas with one vacuum cell beside it for it to be pushed into.
  w.Set(4, 3, hydrogen, 0.5f, 300.0f);
  w.Set(5, 3, vacuum, 0.0f, 0.0f);
  return w;
}

void ElementEmitDisplaceSetup(Backend* s, const Tables&, const World&) {
  SendAddEmitter(s, 2.0f, 500, 510, 520);
}

void ElementEmitDisplaceTick(Backend* s, const Tables& t, const World& w, int tick) {
  // One emission, then never again: the interval is longer than the whole run.
  if (tick == 3) {
    SendModifyEmitter(s, t, 0, w.Cell(4, 3), kOxygen, 1000.0f, 0.4f, 350.0f, 2.0f, 2);
  }
}

// `MassConsumption` and `MassEmission`, the two message-driven halves of the same machinery.
// Klei's consumption floods or walks a rectangle rather than taking from a single cell, and
// its emission displaces a different element out of the way rather than refusing it.
World MassMessages(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t hydrogen = static_cast<uint16_t>(t.IndexOf(kHydrogen));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(28, 14, granite, 1000.0f, 300.0f);
  w.insulation.assign(w.insulation.size(), 0);
  // A three-by-three block of oxygen for the flood to spread through, at three different
  // temperatures so the mix the callback reports is not the same as any one cell's.
  for (int y = 3; y <= 5; ++y) {
    for (int x = 2; x <= 4; ++x) {
      w.Set(x, y, oxygen, 3.0f, 300.0f + 40.0f * static_cast<float>(y - 3));
    }
  }
  // The same block again for the rectangle, which walks right and down from its corner and
  // so covers a different set of cells from the flood's diamond.
  for (int y = 3; y <= 5; ++y) {
    for (int x = 8; x <= 10; ++x) {
      w.Set(x, y, oxygen, 3.0f, 300.0f + 40.0f * static_cast<float>(y - 3));
    }
  }
  // Emission targets: a vacuum cell, a same-gas cell, a foreign-gas cell with somewhere to
  // put the displaced gas, and a foreign-gas cell that is sealed and so must refuse.
  w.Set(14, 3, vacuum, 0.0f, 0.0f);
  w.Set(16, 3, oxygen, 1.0f, 300.0f);
  w.Set(18, 3, hydrogen, 1.0f, 300.0f);
  w.Set(19, 3, vacuum, 0.0f, 0.0f);
  w.Set(22, 3, hydrogen, 1.0f, 300.0f);
  return w;
}

void MassMessagesTick(Backend* s, const Tables& t, const World& w, int tick) {
  if (tick == 2) {
    // The flood. `height == 0` picks it, and `radius` is a step count, so 2 reaches the
    // centre cell and its four neighbours and no further.
    SendMassConsumption(s, t, w.Cell(3, 4), kOxygen, 7.0f, 2, 0, 500);
    // The rectangle: three wide, two high, anchored at the top-left corner rather than
    // centred, and with no phase test at all.
    SendMassConsumption(s, t, w.Cell(8, 3), kOxygen, 7.0f, 3, 2, 501);
    // Emissions.
    SendMassEmission(s, t, w.Cell(14, 3), kOxygen, 1.0f, 400.0f, 502);
    SendMassEmission(s, t, w.Cell(16, 3), kOxygen, 1.0f, 400.0f, 503);
    SendMassEmission(s, t, w.Cell(18, 3), kOxygen, 1.0f, 400.0f, 504);
    // Sealed: the hydrogen has nowhere to go, so the emission is refused and the callback
    // reports element 0xffff and nothing else.
    SendMassEmission(s, t, w.Cell(22, 3), kOxygen, 1.0f, 400.0f, 505);
  }
  if (tick == 6) {
    // A consumption that asks for more than is there, so `remaining` never reaches zero and
    // the flood runs to its full extent.
    SendMassConsumption(s, t, w.Cell(3, 4), kOxygen, 1000.0f, 3, 0, 506);
    // And one with a callback of -1, which is the value that suppresses the report. It
    // still takes the mass.
    SendMassConsumption(s, t, w.Cell(8, 3), kOxygen, 1.0f, 2, 2, -1);
  }
}


// ------------------------------------------------------------------ radiation

// The radiation emitter, in its steady-state form. Four `Constant` emitters in a vacuum
// world so nothing else moves and the shared LCG is drawn from by radiation alone.
//
// Three of the four are about geometry rather than physics. `tickConstant` scans a box that
// is `2 * radiusX + 1` cells on **both** sides, so an emitter that is wider than it is tall
// scans rows that are not in its ellipse (and the ellipse test throws them away), and one
// that is taller than it is wide never scans the rows that are. The fourth carries a
// 90-degree cone.
//
// The granite pillar at x = 8 is what makes the ray walk visible: `RadiationAbsorptionAlongLine`
// steps every cell between the emitter and the target and multiplies a running transmission
// by each one's absorption, so the far side of the pillar sits in a shadow with a soft edge.
World Radiate(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(40, 16, vacuum, 0.0f, 0.0f);
  w.radiation = true;
  w.insulation.assign(w.insulation.size(), 0);
  for (int y = 4; y <= 11; ++y) w.Set(8, y, granite, 1000.0f, 300.0f);
  return w;
}

void RadiateSetup(Backend* s, const Tables&, const World& w) {
  // Square, whole circle, rate zero so it fires on every substep: the reference shape.
  SendAddRadiationEmitter(s, w.Cell(5, 7), 4, 4, 100.0f, 0.0f, 1.0f, 0.0f, 360.0f, 0, 700);
  // Wider than tall. Its scan box is 13 rows deep and runs off the bottom of the world,
  // which is also the only test of the grid-bounds guard inside the writer.
  SendAddRadiationEmitter(s, w.Cell(16, 7), 6, 2, 50.0f, 0.0f, 1.0f, 0.0f, 360.0f, 0, 701);
  // Taller than wide: a five-row box over a thirteen-row ellipse.
  SendAddRadiationEmitter(s, w.Cell(26, 7), 2, 6, 50.0f, 0.0f, 1.0f, 0.0f, 360.0f, 0, 702);
  // A 90-degree cone pointing straight up.
  SendAddRadiationEmitter(s, w.Cell(34, 7), 3, 3, 80.0f, 0.0f, 1.0f, 90.0f, 90.0f, 0, 703);
}

void RadiateTick(Backend* s, const Tables&, const World& w, int tick) {
  // Modify does not touch either timer or the phase, and it clamps `emitSpeed` against
  // `emitRate` where `Register` clamped it against `emitRads`.
  if (tick == 25) {
    SendModifyRadiationEmitter(s, 3, w.Cell(34, 7), 3, 3, 80.0f, 0.3f, 1.0f, 270.0f, 90.0f,
                               0);
  }
  // Removed mid-run: the rads it has already painted stay where they are.
  if (tick == 35) SendRemoveRadiationEmitter(s, 2, 712);
}

// The three pulsing shapes plus the one that does nothing.
//
// A pulse is a sweep: `emitStepTimer` is a carry, one step's worth is `emitSpeed / max(rx, ry)`
// seconds, and a substep that cannot afford a step banks its time towards the next one. At
// `speed = 1` and a radius of 4 that is 0.25 s a step against a 0.2 s substep, so the sweep
// advances on two substeps out of three and the carry is the only thing that decides which.
//
// `SimplePulse` and `Attractor` compute the sweep fraction by multiplying by a reciprocal
// where `tickPulsing` divides, so the two rasters can differ by a pixel at the same phase.
World RadPulse(const Tables& t) {
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(48, 18, vacuum, 0.0f, 0.0f);
  w.radiation = true;
  w.insulation.assign(w.insulation.size(), 0);
  return w;
}

void RadPulseSetup(Backend* s, const Tables&, const World& w) {
  SendAddRadiationEmitter(s, w.Cell(5, 8), 4, 4, 200.0f, 2.0f, 1.0f, 0.0f, 360.0f, 1, 800);
  // Averaged: the same ring, divided by the area of its own bounding box, so the intensity
  // falls off as the sweep widens instead of repeating.
  SendAddRadiationEmitter(s, w.Cell(15, 8), 4, 4, 200.0f, 2.0f, 1.0f, 0.0f, 360.0f, 2, 801);
  // Simple pulse: no attenuation at all, and it writes its own cell like any other.
  SendAddRadiationEmitter(s, w.Cell(25, 8), 4, 4, 200.0f, 2.0f, 1.0f, 0.0f, 360.0f, 3, 802);
  // `emitRads` below `emitSpeed`, which is the only way to see `Register`'s clamp: the
  // stored speed is 0.5, so a step is 0.1667 s and the sweep advances every substep. With
  // the clamp missing it would be 1.0, a step would be 0.333 s, and the first substep would
  // take no step at all.
  SendAddRadiationEmitter(s, w.Cell(35, 8), 3, 3, 0.5f, 2.0f, 1.0f, 0.0f, 360.0f, 1, 803);
  // `RadialBeams` has no case in the switch: it steps its phase and its timers and paints
  // nothing at all.
  SendAddRadiationEmitter(s, w.Cell(43, 8), 3, 3, 200.0f, 2.0f, 1.0f, 0.0f, 360.0f, 4, 804);
}

void RadPulseTick(Backend* s, const Tables&, const World& w, int tick) {
  // Mid-sweep: the phase and both timers survive, so the ring carries on from where it was
  // rather than restarting at the centre.
  if (tick == 17) {
    SendModifyRadiationEmitter(s, 0, w.Cell(5, 8), 4, 4, 120.0f, 2.0f, 1.0f, 0.0f, 180.0f, 1);
  }
}

// The attractor, which is the one emitter that *takes*: it drains up to `emitRads` out of
// every cell of its ring that holds at least 0.01, and adds back into its own cell whatever
// the **last** cell of the raster gave it — not the sum, because the writer overwrites its
// output slot instead of accumulating.
//
// The field it drains is put there by `CellRadiationModification`, which is also the only
// producer of `radiationConsumedCallbacks`.
World RadAttract(const Tables& t) {
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(32, 16, vacuum, 0.0f, 0.0f);
  w.radiation = true;
  w.insulation.assign(w.insulation.size(), 0);
  return w;
}

void RadAttractSetup(Backend* s, const Tables&, const World& w) {
  SendAddRadiationEmitter(s, w.Cell(8, 8), 4, 4, 20.0f, 0.0f, 1.0f, 0.0f, 360.0f, 5, 900);
  // A second attractor over cells that hold nothing: every pixel refuses at the 0.01 gate,
  // its output slot is never written, and it still adds that zero back into its own cell.
  SendAddRadiationEmitter(s, w.Cell(24, 8), 3, 3, 20.0f, 0.0f, 1.0f, 0.0f, 360.0f, 5, 901);
}

void RadAttractTick(Backend* s, const Tables&, const World& w, int tick) {
  // A whole block at once, and every cell of it a different amount. Several cells of one
  // ring then have something to give on the same raster, which is the only way the
  // attractor's report can be about *which* cell it took from last rather than about
  // whether it took anything at all: the writer overwrites its output slot rather than
  // adding to it, and the four-way mirror decides which write survives.
  if (tick == 2 || tick == 12 || tick == 22) {
    for (int y = 4; y <= 12; ++y) {
      for (int x = 4; x <= 12; ++x) {
        SendCellRadiation(s, w.Cell(x, y), 40.0f + 3.0f * static_cast<float>(x + 2 * y), -1);
      }
    }
  }
  // Charged one cell at a time as well, so the drain and the charge interleave.
  if (tick >= 4 && tick <= 10) {
    SendCellRadiation(s, w.Cell(6 + (tick - 4), 7), 50.0f, 950 + tick);
  }
  // One cell charged to just under the 0.01 gate, which the attractor must refuse.
  if (tick == 5) SendCellRadiation(s, w.Cell(13, 8), 0.005f, 960);
}

// The two messages that are not the emitter's: `CellRadiationModification` and
// `RadiationParamsModification`, both drained by `ProcessCellRadiationChanges`.
//
// The callback is the point. It reports **the delta that was applied**, except when the
// delta took the cell to zero or below, where it reports the rads the cell actually had —
// so a consumer asking for more than is there is told what it got. The test is `<= 0`, not
// `< 0`, so a delta that lands exactly on zero reports the old value too.
//
// The five tunables then change what a constant emitter paints: `LINGER_RATE` divides every
// figure it writes, and the other four are the absorption walk's, which is why there is a
// wall in the way and why one cell of it is a **constructed tile** — the 0x80 property bit
// is the only thing that selects `RADIATION_CONSTRUCTED_FACTOR` over the mass-weighted mix.
World RadMsg(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(28, 16, vacuum, 0.0f, 0.0f);
  w.radiation = true;
  w.insulation.assign(w.insulation.size(), 0);
  for (int y = 3; y <= 12; ++y) {
    w.Set(9, y, granite, 800.0f, 300.0f);
    // The bottom half of the wall is built rather than natural.
    if (y >= 8) w.SetProperties(9, y, 0x80);
  }
  return w;
}

void RadMsgSetup(Backend* s, const Tables&, const World& w) {
  SendAddRadiationEmitter(s, w.Cell(5, 7), 6, 6, 400.0f, 0.0f, 1.0f, 0.0f, 360.0f, 0, 1000);
}

void RadMsgTick(Backend* s, const Tables&, const World& w, int tick) {
  // A plain deposit: the callback reports the delta.
  if (tick == 2) SendCellRadiation(s, w.Cell(20, 4), 40.0f, 1100);
  // A withdrawal that fits: still the delta, and it is negative.
  if (tick == 4) SendCellRadiation(s, w.Cell(20, 4), -10.0f, 1101);
  // A withdrawal that overshoots: the cell is zeroed and the callback reports what was there.
  if (tick == 6) SendCellRadiation(s, w.Cell(20, 4), -100.0f, 1102);
  // A deposit and an exact-to-zero withdrawal, which is the `<= 0` half of the test.
  if (tick == 8) SendCellRadiation(s, w.Cell(20, 6), 25.0f, 1103);
  if (tick == 9) SendCellRadiation(s, w.Cell(20, 6), -25.0f, 1104);
  // No callback index at all: the cell moves and nothing is reported.
  if (tick == 10) SendCellRadiation(s, w.Cell(20, 8), 12.5f, -1);
  // Two in one frame at one cell, to prove the list is in drain order and not merged.
  if (tick == 12) {
    SendCellRadiation(s, w.Cell(22, 10), 5.0f, 1105);
    SendCellRadiation(s, w.Cell(22, 10), 7.0f, 1106);
  }

  // The tunables, one at a time so each one's effect is separable in the dump.
  if (tick == 15) SendRadiationParams(s, 0, 2.2f);   // LINGER_RATE
  if (tick == 20) SendRadiationParams(s, 2, 0.15f);  // BASE_WEIGHT
  if (tick == 25) SendRadiationParams(s, 3, 1.4f);   // DENSITY_WEIGHT
  if (tick == 30) SendRadiationParams(s, 4, 0.2f);   // CONSTRUCTED_FACTOR
  if (tick == 35) SendRadiationParams(s, 5, 500.0f); // MAX_MASS
  // Type 1 is missing from Klei's chain, and 9 is not a type at all: both are dropped.
  if (tick == 40) {
    SendRadiationParams(s, 1, 99.0f);
    SendRadiationParams(s, 9, 99.0f);
  }
  // `Modify`'s clamp is against `emitRate`, so this stores a speed of 0.3.
  if (tick == 45) {
    SendModifyRadiationEmitter(s, 0, w.Cell(5, 7), 6, 6, 400.0f, 0.3f, 1.0f, 0.0f, 360.0f, 0);
  }
}


// The radiation **field** with no emitter in it at all: the three sources the two sweeps
// inside `SimBase::UpdateData` add, and the decay they take away.
//
// It runs at `dt = 1.0`, which is five substeps in one frame. That is deliberate: the whole
// pass is per **substep**, not per frame, and at one substep a frame there is no way to tell
// the two apart — every earlier radiation scenario would score the same either way.
//
//   * **Radioactive elements** spray through a 5x5 stencil whose weights sum to 9.9. Element
//     54 is the strongest solid in the table at 250 rads per 1000 kg; 93 is weak but absorbs
//     0.85, so it shadows itself.
//   * **Cosmic radiation** arrives from the top of each column, attenuated by everything
//     above, and the granite roof with a gap in it is what makes the shaft visible.
//   * **Radiation sickness** germs make their own cell radioactive at a thousandth of the
//     count, which is the only reader of a disease index anywhere outside `disease.h`.
// Germs in a **zero-mass** cell. `radsource` found this by accident and had to move its
// germs into the roof to get clear of it: a seeded vacuum cell diverges on its own, with no
// radiation, no flow and no conduction anywhere near it. Both germ scenarios in the suite
// are solid granite wall-to-wall, so mass 0 was never once under a germ.
//
// The world is granite at rest with a vacuum pocket cut into it, so the only thing in it
// that can move is disease. Every cell that carries germs here has mass 0, which is the one
// input the growth rule divides the population bounds by: `min_count` and the crowding
// ceiling are both `mass * perKG`, so at mass 0 they collapse to 0 and the three-way branch
// has no interior left.
// Disease diffusion in granite **at** the threshold, which is where it diverges.
//
// Found by accident: `germcons` first seeded a round 1,000,000 germs per cell, and the world
// diverged at tick 2 with no message sent. Granite's `minDiffusionCount` is exactly 1,000,000
// and the gate is `count < min`, so a round million *passes* it — every germ scenario written
// before today sat below the threshold and never diffused in granite at all.
//
// It diverged by nine cells of `diseaseIdx` and twelve of `diseaseCount`, and the cause was
// ours: `DiseaseDiffusionScale` read the **live** grid where Klei's `GetDiffusionScale` reads
// the pre-sweep snapshot. The first of a cell's four transfers dropped it to 999,875 and our
// gate then refused the other three, so a cell diffused to one neighbour where Klei diffuses
// to four. Fixed; exact for 50 ticks since. `germgas` is the reason it took a
// second scenario to see: oxygen's threshold is 1,000 against counts of thousands, so no
// transfer ever crosses it there.
World GermThreshold(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  World w;
  w.Init(24, 16, granite, 1000.0f, 293.15f);
  // Exactly on the gate, and 1000 kg * 1000 per kg is exactly the crowding ceiling too, so
  // these cells sit on both boundaries at once.
  w.SetDisease(4, 4, kFoodPoisoning, 1000000);
  w.SetDisease(8, 4, kFoodPoisoning, 1000000);
  w.SetDisease(16, 4, kFoodPoisoning, 1000000);
  return w;
}

// `ProcessConsumeDisease` — the disease *consumer*, which is a per-frame
// **message** and not a component update at all. Granite everywhere, so nothing but the
// messages can touch a germ: diffusion cannot start (granite's `minDiffusionCount` is a
// million) and there is no fluid to move anything.
World GermConsume(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  World w;
  w.Init(24, 16, granite, 1000.0f, 293.15f);
  // **Under** granite's `minDiffusionCount`, which is exactly 1,000,000. Seeding a round
  // million puts every cell right on the threshold — the gate is `count < min`, so a million
  // passes it — and the whole world starts diffusing before the first message is sent.
  w.SetDisease(4, 4, kFoodPoisoning, 900000);
  w.SetDisease(8, 4, kFoodPoisoning, 900000);
  w.SetDisease(12, 4, kFoodPoisoning, 7);
  w.SetDisease(16, 4, kFoodPoisoning, 900000);
  return w;
}

void SendConsumeDisease(Backend* s, int game_cell, int32_t callback, float percent,
                        int32_t max_count) {
  ConsumeDiseaseMessage m{};
  m.gameCell = game_cell;
  m.callbackIdx = callback;
  m.percentToConsume = percent;
  m.maxToConsume = max_count;
  s->handle_message(static_cast<int32_t>(SimMessageHash::ConsumeDisease), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

void GermConsumeTick(Backend* s, const Tables&, const World& w, int tick) {
  if (tick == 3) {
    // A plain share, well under the cap.
    SendConsumeDisease(s, w.Cell(4, 4), 500, 0.25f, 1000000);
    // The **cap** binds: a quarter of a million requested, ten taken.
    SendConsumeDisease(s, w.Cell(8, 4), 501, 0.25f, 10);
    // Seven germs at 50%: 3.5 rounds to 4, because the amount is `(int)(count * pct + 0.5f)`
    // and not a truncation.
    SendConsumeDisease(s, w.Cell(12, 4), 502, 0.5f, 1000000);
    // All of them, which empties the cell and must clear the index with it. The callback
    // then reports 0xFF rather than the disease that was just taken.
    SendConsumeDisease(s, w.Cell(16, 4), 503, 1.0f, 1000000);
    // A clean cell: no germs, so nothing is taken and the callback still fires with 0xFF/0.
    SendConsumeDisease(s, w.Cell(20, 4), 504, 1.0f, 1000000);
    // No callback at all, which must consume and stay silent.
    SendConsumeDisease(s, w.Cell(4, 4), -1, 0.5f, 1000000);
  }
  // Repeated on an already-emptied cell.
  if (tick == 10) {
    SendConsumeDisease(s, w.Cell(16, 4), 505, 1.0f, 1000000);
  }
}

// `SimData::ResizeAndInitializeVacuumCells`. **Unimplemented, and this scenario
// is the reason.** The tail of the function is a plain clearing loop — element, mass,
// temperature, disease count, disease index and radiation, over a `width` x `height` rectangle
// at (`xOffset`, `yOffset`), in both grids — and implementing that alone is wrong. Sent these
// six int32s, Klei's world **gains 240 tonnes** (about 120 cells of 2000 kg) and 48 cells
// change element, so the part of the function before the loop does the resizing its name
// promises. Read that first; 2762 bytes, and the clearing loop is a small piece of it.
//
// The world is a warm room of gas with germs and radiation in it, so every field the message
// is supposed to clear has something in it to clear — and the cells *outside* the rectangle
// say whether it clears more than it should.
World VacuumRect(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  World w;
  w.Init(24, 16, granite, 2000.0f, 293.15f);
  for (int y = 1; y < 15; ++y) {
    for (int x = 1; x < 23; ++x) w.Set(x, y, oxygen, 2.0f, 320.0f);
  }
  // One germ pocket in each of the three zones the message divides the world into: the
  // rectangle it clears, the one-cell ring it walls that rectangle in with, and the rest of
  // the grid. Without the ring cell there is no way to tell whether the border write clears
  // the six fields or only the three it needs to place a solid.
  w.SetDisease(6, 6, kFoodPoisoning, 500);    // inside the cleared rectangle
  w.SetDisease(3, 4, kFoodPoisoning, 500);    // on the Unobtanium ring
  w.SetDisease(14, 6, kFoodPoisoning, 500);   // outside both, the control
  w.radiation = true;
  return w;
}

void VacuumRectTick(Backend* s, const Tables& t, const World& w, int tick) {
  // Radiation in the same three zones, and for the same reason.
  if (tick == 1) {
    SendCellRadiation(s, w.Cell(6, 6), 250.0f);
    SendCellRadiation(s, w.Cell(3, 4), 250.0f);
    SendCellRadiation(s, w.Cell(14, 6), 250.0f);
  }
  // A solid plug dropped into the middle of the cleared rectangle two ticks after it opens.
  // The rectangle reads back fully lit, and this is what says *why*: if the message registers
  // the rectangle as a world and the ordinary sunlight sweep lights it, the plug casts a
  // shadow down its column; if the message simply writes 255 into the texture, nothing below
  // the plug changes. One lit rectangle on its own cannot tell those apart.
  if (tick == 5) SendModifyCell(s, t, w.Cell(6, 7), kGranite, 800.0f, 300.0f);
  if (tick != 3) return;
  // gridSizeX, gridSizeY (read and unused), then width, height, xOffset, yOffset.
  const int32_t v[6] = {24, 16, 6, 4, 4, 5};
  std::vector<uint8_t> b(reinterpret_cast<const uint8_t*>(v),
                         reinterpret_cast<const uint8_t*>(v) + sizeof(v));
  s->Send(SimMessageHash::SimData_ResizeAndInitializeVacuumCells, b);
}

// Two diseases meeting. `germemit` carried a second disease beside its seeded row, and taking
// it out removed several cell divergences on its own — so the merge deserves a world where it
// is the only thing happening.
//
// `Disease::CalculateFinalDiseaseCount` is not symmetric: each side's claim is its count times
// its `strength`, and **every disease in the shipped table has strength 0**, so both claims are
// zero and the resident always wins. That makes the ratio branch dead code in practice, and
// this scenario is what says whether our reading of the live branch is right.
World GermMerge(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  World w;
  w.Init(24, 16, granite, 2000.0f, 293.15f);
  for (int y = 1; y < 15; ++y) {
    for (int x = 1; x < 23; ++x) w.Set(x, y, oxygen, 1.0f, 293.15f);
  }
  // Adjacent, both above oxygen's diffusion threshold of 1,000, so each spreads into the
  // other and the merge runs on every substep from tick 1.
  w.SetDisease(8, 6, kFoodPoisoning, 40000);
  w.SetDisease(9, 6, 1, 40000);
  // Unequal, so the ratio branch would show if it were ever live.
  w.SetDisease(14, 6, kFoodPoisoning, 100000);
  w.SetDisease(15, 6, 1, 2000);
  // Separated by one clean cell, so the two arrive in the middle from opposite sides.
  w.SetDisease(18, 6, kFoodPoisoning, 40000);
  w.SetDisease(20, 6, 2, 40000);
  return w;
}

// Disease diffusion in **gas**, with no emitter and nothing else moving.
//
// Both germ scenarios in this suite are solid granite, and granite's growth row has
// `minDiffusionCount` 1,000,000 — so in five months of germ scenarios, disease diffusion has
// only ever been tested *below its own threshold* in one element. Oxygen's row is
// `diffusionScale 0.01, minDiffusionCount 1000, minCountPerKG 250, maxCountPerKG 1000`, so a
// 1 kg cell of oxygen holding a few thousand germs diffuses on every substep and is
// over-crowded while it does it. `germemit` found this by accident; this is the same thing
// with the emitter taken away.
World GermGas(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  World w;
  w.Init(24, 16, granite, 2000.0f, 293.15f);
  for (int y = 1; y < 15; ++y) {
    for (int x = 1; x < 23; ++x) w.Set(x, y, oxygen, 1.0f, 293.15f);
  }
  // Seeded through the payload, so the germs are already there when the first snapshot is
  // taken — which is the one thing `germemit` cannot arrange.
  w.SetDisease(6, 6, kFoodPoisoning, 2000);
  // Above the crowding ceiling (1000 per kg on 1 kg) by a lot, and below it.
  w.SetDisease(12, 6, kFoodPoisoning, 500000);
  w.SetDisease(18, 6, kFoodPoisoning, 900);
  return w;
}

// The disease emitter, `DiseaseEmitter::Update` — the seventh and last entry in
// `SimData`'s component list, and the only thing in the sim that *creates* germs. Nothing in
// this suite had ever sent one, so neither the component nor the `diseaseEmittedInfos` list it
// reports through had ever been exercised.
//
// The world is a granite shell around one room of oxygen. `GetReachableCells` refuses solids,
// so the room is what bounds every emitter's reach, and the gas is heavy enough and cold
// enough that nothing else moves germs around while the emitters work.
World DiseaseEmit(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  World w;
  w.Init(24, 16, granite, 2000.0f, 293.15f);
  for (int y = 1; y < 15; ++y) {
    for (int x = 1; x < 23; ++x) w.Set(x, y, oxygen, 1.0f, 293.15f);
  }
  // Germs for the removal emitter to take away, and a second disease beside them so the
  // "only cells already carrying *this* disease" test has something to refuse.
  for (int x = 14; x <= 18; ++x) w.SetDisease(x, 8, kFoodPoisoning, 5000000);
  w.SetDisease(16, 10, 1, 5000000);
  return w;
}

void DiseaseEmitSetup(Backend* s, const Tables&, const World&) {
  for (int32_t cb = 400; cb < 405; ++cb) {
    AddDiseaseEmitterMessage m{};
    m.callbackIdx = cb;
    s->handle_message(static_cast<int32_t>(SimMessageHash::AddDiseaseEmitter), sizeof(m),
                      reinterpret_cast<const uint8_t*>(&m));
  }
}

void SendModifyDiseaseEmitter(Backend* s, int32_t handle, int game_cell, uint8_t disease_idx,
                              uint8_t max_depth, float interval, int32_t count) {
  ModifyDiseaseEmitterMessage m{};
  m.handle = handle;
  m.gameCell = game_cell;
  m.diseaseIdx = disease_idx;
  m.maxDepth = max_depth;
  m.emitInterval = interval;
  m.emitCount = count;
  s->handle_message(static_cast<int32_t>(SimMessageHash::ModifyDiseaseEmitter), sizeof(m),
                    reinterpret_cast<const uint8_t*>(&m));
}

void DiseaseEmitTick(Backend* s, const Tables&, const World& w, int tick) {
  if (tick == 3) {
    // Interval shorter than a substep: fires every substep, and only once each, because the
    // interval is subtracted rather than looped.
    SendModifyDiseaseEmitter(s, 0, w.Cell(4, 4), kFoodPoisoning, 1, 0.05f, 1000);
    // Interval longer than a substep, so the carry is visible: some ticks emit, some do not.
    SendModifyDiseaseEmitter(s, 1, w.Cell(9, 4), kFoodPoisoning, 2, 0.5f, 5000);
    // A **negative** count, which is a removal and not an emission: it may only touch cells
    // whose disease index already matches. Sat over the seeded row, with the other disease
    // two cells below it as the control.
    SendModifyDiseaseEmitter(s, 2, w.Cell(16, 8), kFoodPoisoning, 2, 0.2f, -1000000);
    // Depth 0: the flood reaches the origin and nothing else.
    SendModifyDiseaseEmitter(s, 3, w.Cell(20, 12), kFoodPoisoning, 0, 0.2f, 2000);
    // Handle 4 is left registered and unmodified — `Register` leaves `diseaseIdx` at 0xFF,
    // which disables the emitter outright, and it should report as never having fired.
  }
  // Re-modified mid-run. `DiseaseEmitter::Modify` does **not** reset `elapsedTime`, unlike
  // the element emitter's, so this one carries its clock across the change.
  if (tick == 20) {
    SendModifyDiseaseEmitter(s, 1, w.Cell(9, 4), kFoodPoisoning, 3, 0.5f, 7000);
  }
  // Removed outright, which must stop it and free the slot.
  if (tick == 30) {
    RemoveDiseaseEmitterMessage m{};
    m.handle = 0;
    m.callbackIdx = 400;
    s->handle_message(static_cast<int32_t>(SimMessageHash::RemoveDiseaseEmitter), sizeof(m),
                      reinterpret_cast<const uint8_t*>(&m));
  }
}

// ------------------------------------------------------------------- cluster messages
//
// World zones. The game sends three of these on every load and `ModifyCellWorldZone` hundreds
// of times a frame after that. A suite that never sends one runs with the zone array left
// at zero. What zones are *for* is a cluster map: several asteroids share one grid, and each
// cell carries the id of the world it belongs to.
//
// The question this scenario asks is the only one that matters before implementing them:
// does any kernel **read** the zone? Klei is the oracle. A room of gas with a pressure step
// across the middle has to move gas over `x == kZoneSplit` on every substep of every tick;
// if zones gate anything at all — flow, conduction, the shuffle — then telling Klei that
// the two halves are different worlds must change what Klei does. If Klei's output is
// unchanged by the zone messages, the zone array is inert for physics and implementing it
// is bookkeeping, not a kernel.
const int kZoneWidth = 24;
const int kZoneHeight = 16;
const int kZoneSplit = 12;

World ZoneSplit(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  World w;
  w.Init(kZoneWidth, kZoneHeight, granite, 2000.0f, 293.15f);
  // One room, wall to wall, with no divider: the split is a *zone* boundary and nothing
  // else, so anything that stops at it stopped because of the zone.
  for (int y = 1; y < kZoneHeight - 1; ++y) {
    for (int x = 1; x < kZoneWidth - 1; ++x) {
      const float mass = x < kZoneSplit ? 10.0f : 0.1f;
      const float temp = 280.0f + 2.0f * x + 2.0f * y;
      w.Set(x, y, oxygen, mass, temp);
    }
  }
  // Two probes in the right-hand world, which is the short one and so is lit from y9 down.
  // A solid plug, to ask whether the solid is itself lit and whether it stops what is below
  // it; and a vacuum column, to ask whether "not solid" is really the test or whether the
  // sim wants something with mass in it.
  w.Set(14, 6, granite, 2000.0f, 293.15f);
  w.Set(18, 6, static_cast<uint16_t>(t.IndexOf(kVacuum)), 0.0f, 0.0f);
  w.Set(18, 5, static_cast<uint16_t>(t.IndexOf(kVacuum)), 0.0f, 0.0f);
  return w;
}

// The sunlight probe, and it is deliberately **liquid-free**. The attenuation law needs
// partially-filled cells to exercise it, and a solid gives that without moving: granite's
// `lightAbsorptionFactor` is 1 and its `maxMass` is 1840, so a granite cell holding a quarter
// of that absorbs exactly a quarter of the beam and sits still while doing it. Liquid would
// test the same arithmetic and drag in the liquid-RGB gap, which diverges at tick 1 and stops
// the harness comparing anything else — see `sunliquid` below, which pays that price on
// purpose.
World SunlightProbe(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  World w = ZoneSplit(t);
  // x, kg. Granite's maxMass is 1840, so these are the quarter points of the beam: a cell
  // holding `f * 1840` kg must leave `255 * (1 - f)` below it.
  const struct { int x; float mass; } plugs[] = {
      {13, 460.0f}, {15, 920.0f}, {17, 1380.0f}, {19, 1840.0f}, {21, 2760.0f}};
  for (const auto& p : plugs) w.Set(p.x, 5, granite, p.mass, 293.15f);
  return w;
}

// The same probe with liquid in it, which is the only way to exercise a *non-1* absorption
// factor: water's is 0.25. Known-bad for two reasons and neither of them is the light. The
// liquid RGB texture is off by one in a colour channel (a tier-2 gap that predates this) and
// diverges at tick 1; and the liquid **mass** itself diverges from tick 3, in the columns
// where a partly-filled column is still settling. The sunlight tracks whatever mass its own
// sim has, exactly, which is why it agrees at tick 2 and then follows the mass apart. Read it
// with `--dump ...,S` and read the mass beside it before believing a light diff.
World SunlightLiquid(const Tables& t) {
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  World w = ZoneSplit(t);
  const struct { int x; float mass; } columns[] = {{13, 100.0f}, {16, 1000.0f}, {19, 500.0f}};
  for (const auto& c : columns) {
    for (int y = 1; y <= 5; ++y) w.Set(c.x, y, water, c.mass, 293.15f);
  }
  // A liquid whose `lightAbsorptionFactor` is **1.0** against water's 0.25, and whose
  // `maxMass` is 870 rather than 1000 — the pair that proved the coefficient is the element's
  // and not a constant. It also has to be a liquid that is *stable* at 293 K: the first
  // attempt used liquid oxygen, which boiled to gas before the first tick and read as "no
  // attenuation at all".
  const uint16_t other = static_cast<uint16_t>(t.IndexOf(-1412059381));
  for (int y = 1; y <= 5; ++y) w.Set(21, y, other, 870.0f, 293.15f);
  return w;
}

// Does sunlight pass a Glass Tile? Klei's `GlassTileConfig` sets `SimCellOccupier.setTransparent`,
// which puts `Sim.Cell.Properties.Transparent` (0x10) on the cell, and glass is a solid whose
// `lightAbsorptionFactor` is 0.1. Our `ComputeSunlight` used to read neither: a solid stopped the
// column, measured only on granite. So plugs on `sunlight`'s row in the short world (lit from y9),
// each alone in its column, and the texture below each one is the answer:
//
//   x13  glass 800 kg,     Transparent   -- the Glass Tile as the game builds it
//   x15  glass 800 kg,     no flag       -- the control: the material alone
//   x16  glass 800 kg x2,  y5 and y4     -- stacked panes: do the factors subtract or multiply
//   x17  diamond 700 kg,   Transparent   -- a second 0.1 solid, another mass
//   x19  ice 1000 kg,      Transparent   -- 0.33333, maxMass 1100; 200 K so it cannot melt
//   x20  ice 1000 kg x3,   y5, y4, y3    -- three thirds: where the exposure clamps
//   x21  granite 2000 kg,  Transparent   -- the flag on an opaque solid: does the bit alone pass
//
// The first five plugs are single panes; the two stacks show how the factors combine. Liquid-free for the same
// reason `sunlight` is. Read with `--dump 12,0,22,9,s`.
World SunlightGlass(const Tables& t) {
  const uint16_t glass = static_cast<uint16_t>(t.IndexOf(kGlass));
  const uint16_t diamond = static_cast<uint16_t>(t.IndexOf(kDiamond));
  const uint16_t ice = static_cast<uint16_t>(t.IndexOf(kIce));
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  World w = ZoneSplit(t);
  w.Set(13, 5, glass, 800.0f, 293.15f);
  w.SetProperties(13, 5, kTransparent);
  w.Set(15, 5, glass, 800.0f, 293.15f);
  for (int y = 4; y <= 5; ++y) w.Set(16, y, glass, 800.0f, 293.15f);
  w.Set(17, 5, diamond, 700.0f, 293.15f);
  w.SetProperties(17, 5, kTransparent);
  w.Set(19, 5, ice, 1000.0f, 200.0f);
  w.SetProperties(19, 5, kTransparent);
  for (int y = 3; y <= 5; ++y) w.Set(20, y, ice, 1000.0f, 200.0f);
  w.Set(21, 5, granite, 2000.0f, 293.15f);
  w.SetProperties(21, 5, kTransparent);
  return w;
}

// The three cluster messages, sent to both sims after the world and before the first tick.
void ZoneMessages(Backend* s, const World& w, bool send_offsets) {
  // `SimData::SetWorldZones` allocates `width * height` bytes, zeroes them,
  // and then reads **`width - 2` bytes per row for rows 1 .. height - 2**, into the padded
  // offset `row * width + 1`.
  //
  // NOTE: those are `SimData`'s dimensions, and `SimData` is PADDED -- `world.h` puts it
  // as `width_ = game_width + 2`. So Klei's `width - 2` is the full GAME width and its
  // `height - 2` is the full GAME height, and the payload it wants is one byte per game cell:
  // `w.width * w.height`, which is 384 bytes for these scenarios. This used to send
  // `(w.width - 2) * (w.height - 2)` = 308, having read those two subtractions as the sim's
  // border when they are the padding being undone.
  //
  // Klei does not catch the shortfall. The end-of-buffer check only reaches
  // its `DebugBreak` when a debugger is attached, so in
  // release the reader simply walks 76 bytes off the end of this vector -- silently, on every
  // `zones` / `sunlight` / `sunliquid` run, for as long as those scenarios have existed.
  // About one time in a hundred that tail sits within 24 bytes of an unmapped page, `memcpy`
  // takes an access violation, and Klei's `SimDLLUnhandledExceptionFilter` spins at 100% of a
  // core forever. That is the whole of the "diffsim hangs at zones" defect: not a deadlock,
  // not a kernel, not the handshake -- a short payload of ours and a crash reporter that never
  // returns. Measured: 3 access violations in 300 runs before the fix, 0 in 300 after.
  //
  // The old loop was also off by one in origin, so every row's zone bytes were shifted and
  // the last rows were heap garbage. "Zones are inert" was therefore measured against a
  // misaligned, partly-random map; it still holds with a correct one, but it is only now
  // actually earned.
  std::vector<uint8_t> zones;
  zones.reserve(static_cast<size_t>(w.width) * static_cast<size_t>(w.height));
  for (int y = 0; y < w.height; ++y) {
    for (int x = 0; x < w.width; ++x) {
      zones.push_back(x < kZoneSplit ? 0 : 1);
    }
  }
  s->Send(SimMessageHash::SetWorldZones, zones);

  // `DefineWorldOffsets`: an int32 count, then four int32s per entry into a
  // 16-byte `SimData::WorldOffsetData`. Two worlds side by side, which is what the zone
  // bytes above just claimed.
  std::vector<uint8_t> offsets;
  auto push_i32 = [&offsets](int32_t v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    offsets.insert(offsets.end(), p, p + 4);
  };
  push_i32(2);
  push_i32(0);                        // world 0: offset x, y
  push_i32(0);
  push_i32(kZoneSplit);               // world 0: size x, y
  push_i32(kZoneHeight);
  push_i32(kZoneSplit);               // world 1: offset x, y
  push_i32(0);
  push_i32(kZoneWidth - kZoneSplit);  // world 1: size x, y
  // Deliberately **shorter** than the grid. If the sunlight texture is "the top row of each
  // world" then this world's lit row is `offsetY + sizeY - 1` = 9, not the grid's 15, and the
  // two halves of the top row must disagree with each other.
  push_i32(10);
  if (send_offsets) s->Send(SimMessageHash::DefineWorldOffsets, offsets);

  // `ModifyCellWorldZone`, the one the game sends 468 times a frame. It is a frame message:
  // `SimFrameManager::HandleMessage` collects it into a per-frame vector rather than acting
  // on it, so this is also a test that the collection does not disturb anything.
  for (int y = 4; y <= 6; ++y) {
    CellWorldZoneModification m{};
    m.cell = w.Cell(kZoneSplit, y);
    m.zoneID = 0;  // hand three cells of world 1 back to world 0
    std::vector<uint8_t> b(reinterpret_cast<const uint8_t*>(&m),
                           reinterpret_cast<const uint8_t*>(&m) + sizeof(m));
    s->Send(SimMessageHash::ModifyCellWorldZone, b);
  }
}

// Zones and per-cell zone edits, with **no** world offsets. Klei's output is unchanged by
// these — that is the result this scenario exists to hold on to.
//
// The order is not decoration. `ModifyCellWorldZone` on its own, with no `SetWorldZones`
// before it, kills the run before the first tick (there is no zone array to modify yet), and
// offsets plus modifications with no zones **hangs** the DLL. The game always sends
// `SetWorldZones` first, and so does this.
void ZoneSetup(Backend* s, const Tables& t, const World& w) {
  (void)t;
  ZoneMessages(s, w, false);
}

// The same three messages **with** `DefineWorldOffsets`, which is what turns the sunlight
// texture on.
void SunlightSetup(Backend* s, const Tables& t, const World& w) {
  (void)t;
  ZoneMessages(s, w, true);
}

World GermVacuum(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(24, 16, granite, 1000.0f, 293.0f);
  // A room of vacuum with a solid rim, so every pairing a germ cell can make is either
  // vacuum-to-vacuum or vacuum-to-solid.
  for (int y = 4; y <= 9; ++y)
    for (int x = 3; x <= 12; ++x) w.Set(x, y, vacuum, 0.0f, 0.0f);

  // Alone in the middle of the room: four clean vacuum neighbours, so this is the
  // "one side is 0xFF" export branch with mass 0 on both halves.
  w.SetDisease(7, 6, kFoodPoisoning, 20000000);

  // Against the rim: the only neighbours with any mass are solid granite, which the phase
  // gate should refuse outright.
  w.SetDisease(3, 4, kFoodPoisoning, 20000000);

  // An unequal pair, both in vacuum: the branch that moves an eighth of the difference.
  w.SetDisease(10, 8, kFoodPoisoning, 20000000);
  w.SetDisease(11, 8, kFoodPoisoning, 4000000);

  // A tiny count in vacuum. With mass 0 the underpopulation floor is 0, so nothing is ever
  // below it -- the one cell that says which side of the branch mass 0 lands on.
  w.SetDisease(5, 9, kFoodPoisoning, 100);

  // The same tiny count in granite one cell away, as the control: this one is genuinely
  // starving (1000 kg * 0.4 = 400) and must die on the flat rate.
  w.SetDisease(5, 11, kFoodPoisoning, 100);

  // Three probes that ask what the relocation needs, each its own sealed pocket so they
  // cannot feed one another. All three carry the same 2e7 the room does, so anything that
  // separates them separates the *shape* of the pocket and nothing else.
  //
  // One: a single vacuum cell with no vacuum neighbour at all. If the germs still leave, the
  // rule is not moving them into a neighbour; if they stay, it needs somewhere to put them.
  w.Set(18, 6, vacuum, 0.0f, 0.0f);
  w.SetDisease(18, 6, kFoodPoisoning, 20000000);

  // Two: the only vacuum neighbour is to the **left**, so there is no upward exit.
  w.Set(17, 11, vacuum, 0.0f, 0.0f);
  w.Set(18, 11, vacuum, 0.0f, 0.0f);
  w.SetDisease(18, 11, kFoodPoisoning, 20000000);

  // Three: the only vacuum neighbour is **below**. Together with two, this says whether the
  // direction is gravity's or the sweep's.
  w.Set(21, 6, vacuum, 0.0f, 0.0f);
  w.Set(21, 7, vacuum, 0.0f, 0.0f);
  w.SetDisease(21, 7, kFoodPoisoning, 20000000);
  return w;
}

World RadSource(const Tables& t) {
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  World w;
  w.Init(32, 20, vacuum, 0.0f, 0.0f);
  w.radiation = true;
  w.cosmic = 800.0f;
  w.insulation.assign(w.insulation.size(), 0);
  // A roof with a two-cell gap: the columns under the gap see the sky, the rest do not.
  for (int x = 0; x < 32; ++x) {
    if (x == 14 || x == 15) continue;
    w.Set(x, 17, granite, 1000.0f, 300.0f);
  }
  // Two radioactive lumps, far enough apart that their stencils do not touch.
  w.Set(5, 6, static_cast<uint16_t>(54), 400.0f, 300.0f);
  w.Set(20, 6, static_cast<uint16_t>(93), 900.0f, 300.0f);
  // A built tile beside the strong lump, so the occlusion above it takes the constructed
  // factor rather than the mass-weighted mix.
  w.Set(5, 10, granite, 1000.0f, 300.0f);
  w.SetProperties(5, 10, 0x80);
  // A three-cell plug of the table's strongest absorber at full `RADIATION_MAX_MASS`. One
  // such cell transmits 0.15, three transmit 0.0034, and that is under the occlusion pass's
  // 0.01 cutoff — the only way to reach the branch that snaps a dim column to zero.
  for (int y = 15; y <= 17; ++y) w.Set(8, y, static_cast<uint16_t>(93), 2000.0f, 300.0f);
  // Germs of the one disease that is itself a radiation source, in the **roof** rather than
  // in the vacuum below it. Germs in a zero-mass cell diverge on their own — Klei's grow and
  // spread and ours sit still — which is a disease-in-vacuum question and not a radiation
  // one. Solid granite at rest is the shape disease is known good
  // in, so the source term is scored there.
  w.SetDisease(26, 17, kRadContaminants, 30000000);
  return w;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  const char* klei_dll = "SimDLL_orig.dll";
  const char* mine_dll = "..\\sim\\build\\SimDLL.dll";
  const char* corpus = nullptr;
  const char* scenario = "all";
  bool run_conduits = false;
  bool conduits_live = false;
  int ticks = 50;
  DumpRect dump;
  bool dump_vacuum_growth = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--klei") && i + 1 < argc) klei_dll = argv[++i];
    else if (!strcmp(argv[i], "--mine") && i + 1 < argc) mine_dll = argv[++i];
    else if (!strcmp(argv[i], "--corpus") && i + 1 < argc) corpus = argv[++i];
    else if (!strcmp(argv[i], "--scenario") && i + 1 < argc) scenario = argv[++i];
    else if (!strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--chunks")) g_show_chunks = true;
    else if (!strcmp(argv[i], "--conduits")) run_conduits = true;
    else if (!strcmp(argv[i], "--conduits-live")) { run_conduits = true; conduits_live = true; }
    else if (!strcmp(argv[i], "--no-textures")) g_skip_textures = true;
    else if (!strcmp(argv[i], "--ledger")) g_ledger = true;
    else if (!strcmp(argv[i], "--vacuum")) g_fill_vacuum = true;
    else if (!strcmp(argv[i], "--draws")) g_show_draws = true;
    else if (!strcmp(argv[i], "--nowait")) g_no_wait = true;
    else if (!strcmp(argv[i], "--firstwait") && i + 1 < argc) g_first_wait_us = atol(argv[++i]);
    else if (!strcmp(argv[i], "--tickwait") && i + 1 < argc) g_tick_wait_us = atol(argv[++i]);
    else if (!strcmp(argv[i], "--bootwait") && i + 1 < argc) g_boot_wait_us = atol(argv[++i]);
    else if (!strcmp(argv[i], "--gameside")) g_gameside = true;
    else if (!strcmp(argv[i], "--messages")) g_show_messages = true;
    else if (!strcmp(argv[i], "--gas") && i + 1 < argc) {
      g_tracer_gas = static_cast<int32_t>(strtol(argv[++i], nullptr, 10));
    }
    else if (!strcmp(argv[i], "--gasmass") && i + 1 < argc) {
      g_tracer_mass = static_cast<float>(atof(argv[++i]));
    }
    else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
      g_world_seed = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
    }
    else if (!strcmp(argv[i], "--room") && i + 1 < argc) {
      sscanf(argv[++i], "%d,%d", &g_room_w, &g_room_h);
    }
    else if (!strcmp(argv[i], "--layout") && i + 1 < argc) {
      g_layout = argv[++i];
    }
    else if (!strcmp(argv[i], "--columnmass") && i + 1 < argc) {
      g_column_gas_mass = static_cast<float>(atof(argv[++i]));
    }
    else if (!strcmp(argv[i], "--dumpexact")) {
      g_dump_exact = true;
    }
    else if (!strcmp(argv[i], "--dumpvacuumgrowth")) {
      dump_vacuum_growth = true;
    }
    else if (!strcmp(argv[i], "--columnwtemp") && i + 1 < argc) {
      g_column_water_temp = static_cast<float>(atof(argv[++i]));
    }
    else if (!strcmp(argv[i], "--columnflat")) {
      g_column_flat = true;
    }
    else if (!strcmp(argv[i], "--columnwater") && i + 1 < argc) {
      g_column_water_mass = static_cast<float>(atof(argv[++i]));
    }
    else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
      // --dump x0,y0,x1,y1[,t|m|e]
      char f = 't';
      if (sscanf(argv[++i], "%d,%d,%d,%d,%c", &dump.x0, &dump.y0, &dump.x1, &dump.y1, &f) >= 4) {
        dump.on = true;
        dump.field = f;
      }
    }
  }
  if (!corpus) {
    printf("usage: diffsim.exe --corpus <corpus.bin> [--klei <dll>] [--mine <dll>]\n"
           "       [--scenario equilibrium|sunlit|falling|crossload|textures|all]"
           " [--ticks N]\n"
           "       [--draws]  per-tick random-stream advance in our sim\n"
           "       [--messages]  per-tick substanceChangeInfo and unstableCellInfo lists\n"
           "       [--gameside]  send headless = 0, so unstable solids and falling liquid are\n"
           "                     handed to the game; `hang` always runs this way\n");
    return 2;
  }

  Tables tables;
  if (!LoadTables(corpus, &tables)) return 1;
  printf("element table: %d elements\n", tables.count);

  if (getenv("ONI_DUMP_VACUUM_GROWTH") || dump_vacuum_growth) {
    // Raw parse, matching sim/world.h's `DiseaseTable::Load` byte layout exactly (that class
    // isn't linked into this driver): 0 int32 diseaseCount | 4 int32 elementCount | per
    // disease: KleiString name, int32 idHash, 76-byte fixed part, elementCount * 29-byte
    // growth records.
    const std::vector<uint8_t>& d = tables.diseases;
    const int32_t vac = tables.IndexOf(kVacuum);
    if (d.size() < 8 || vac < 0) {
      printf("dump failed: diseases.size=%zu vac=%d\n", d.size(), vac);
      return 0;
    }
    int32_t disease_count = 0, element_count = 0;
    memcpy(&disease_count, d.data(), 4);
    memcpy(&element_count, d.data() + 4, 4);
    printf("disease_count=%d element_count=%d vacuum_idx=%d\n", disease_count, element_count,
           vac);
    size_t off = 8;
    for (int32_t i = 0; i < disease_count; ++i) {
      int32_t name_len = 0;
      memcpy(&name_len, d.data() + off, 4);
      off += 4;
      std::string name(reinterpret_cast<const char*>(d.data() + off),
                       static_cast<size_t>(name_len));
      off += static_cast<size_t>(name_len);
      int32_t hash = 0;
      memcpy(&hash, d.data() + off, 4);
      const size_t fixed = 76;   // id_hash+strength+4*4+4*4+4*4+4*4+radiation = 4+4+64+4=76
      const size_t growth_stride = 29;
      if (i == kFoodPoisoning && static_cast<int32_t>(vac) < element_count) {
        size_t p = off + fixed + static_cast<size_t>(vac) * growth_stride;
        auto f32 = [&](size_t k) { float v; memcpy(&v, d.data() + p + k * 4, 4); return v; };
        int32_t minDiffCount; memcpy(&minDiffCount, d.data() + p + 24, 4);
        uint8_t minDiffTicks = d[p + 28];
        printf("disease[%d]=%s hash=%d growth[vacuum=%d]: underPopDeathRate=%g "
               "popHalfLife=%g overPopHalfLife=%g diffusionScale=%g minCountPerKG=%g "
               "maxCountPerKG=%g minDiffusionCount=%d minDiffusionInfestTicks=%u\n",
               i, name.c_str(), hash, vac, f32(0), f32(1), f32(2), f32(3), f32(4), f32(5),
               minDiffCount, minDiffTicks);
      }
      off += fixed + static_cast<size_t>(element_count) * growth_stride;
    }
    return 0;
  }

  Backend klei, mine;
  if (!klei.Bind(klei_dll, "klei") || !mine.Bind(mine_dll, "mine")) return 1;

  // The conduit harness drives a different set of exports and owns its own boot, so it runs
  // instead of the scenario table rather than alongside it.
  if (run_conduits) return RunConduits(&klei, &mine, tables, conduits_live) == 0 ? 0 : 1;

  // Exact equality cannot be required of anything with physics in it, because Klei is not
  // deterministic — and how far it wanders depends on what is in the world, so a single
  // global tolerance is useless. Each scenario carries the spread measured by running
  // Klei against a byte-identical copy of itself three times over 60 ticks, and passes if
  // it stays inside that. `envelope_k` of 0 means the scenario is not yet expected to
  // pass; the number it reports is the size of the remaining gap.
  int failures = 0;
  struct Entry {
    const char* name;
    World (*build)(const Tables&);
    double envelope_k;   // 0 = known gap, report but do not fail
    const char* note;
    // Messages to send both sims after Boot and before the first tick. Buildings are the
    // only thing that needs it so far, because they are the only sim state that does not
    // arrive in the world payload.
    BuildingSetup setup = nullptr;
    // Null means one region covering the whole world, which is what every scenario sent
    // before the region probes existed and what a single-asteroid game sends.
    RegionFn regions = nullptr;
    // 0.2 s is one substep, which is what every other scenario runs. A scenario that wants
    // several substeps inside one frame says so here.
    float dt = 0.2f;
    // Messages to send before each tick. See `TickHook`.
    TickHook tick_hook = nullptr;
  };
  const Entry entries[] = {
      {"equilibrium", &Equilibrium, 0.01, "nothing to simulate; must be exact"},
      {"heatblock", &HeatBlock, 0.01, "conduction only"},
      {"insulated", &Insulated, 0.01, "conduction through insulated cells; the min branch"},
      {"gaspockets", &GasPockets, 0.5, "gas flow; Klei's own spread here reaches 0.42 K"},
      {"gasmix", &GasMix, 0.5, "two gases in threes; the displacement sweep alone"},
      // These three do not measure our kernel: they measure Klei's random gas shuffle,
      // which is real, seeded and not modelled here. They are detectors, so they are
      // reported and never scored — a pass would only mean the shuffle happened to be
      // quiet.
      {"gasroom", &GasRoom, 0.0, "detector: gas room, shows the shuffle as displaced heat"},
      {"tracer", &Tracer, 0.0, "detector: a room where nothing may move; every swap shows"},
      {"tracerslab", &TracerSlab, 0.0, "detector: two stacked pockets; is the dead zone pocket-relative"},
      {"tracermix", &TracerMix, 0.0, "detector: two gases; the shuffle carries elements too"},
      // All four liquid probes are essentially exact with --vacuum over 50 ticks — worst
      // 0.0002 kg. **Run them with --vacuum.** In gas they carry the random shuffle on top,
      // which is not a liquid law, and debugging liquid through them is debugging the wrong
      // thing. They are still worth running in gas for one reason: they are the only probes
      // that exercise `DisplaceGas`, and its early ticks are exact, so a divergence in the
      // first few ticks of `pour` is a real defect and not the shuffle.
      // Not a physics probe: a draw-accounting instrument. See `Column`.
      {"column", &Column, 0.0, "detector: one cell wide, spelled by --layout; use --draws"},
      {"drop", &LiquidDrop, 0.0, "swap alone; --vacuum: 0.000015 kg over 50 ticks"},
      {"pair", &LiquidPair, 0.0, "alternating columns; --vacuum: 0.000061 kg over 50 ticks"},
      {"pool", &LiquidPool, 0.0, "pressure + spread; --vacuum: 0.000244 kg over 50 ticks"},
      {"hang", &LiquidHang, 0.01, "liquid that can only fall: SpawnFallingLiquid, non-headless"},
      {"melt", &PartialMelt, 0.01, "hot gas melting 5 kg off cold ice: DoPartialMelt, headless"},
      {"meltfall", &PartialMeltFalling, 0.01,
       "the same, non-headless: each melt a falling-liquid record"},
      {"meltcold", &PartialMeltCold, 0.01,
       "melt with 250 K ice: meltwater refreezes as ore (small freeze)"},
      {"freeze", &SmallFreeze, 0.01, "small freezes to ore, large ones to a solid cell"},
      {"rain", &Rain, 0.01, "condensing steam, non-headless: each a falling-liquid record"},
      {"pbreak", &PressureBreak, 0.01,
       "over-full liquid: DoPressureBreak on thin walls, the over-full DisplaceLiquid",
       &PressureBreakSetup},
      {"tilemelt", &TileMelt, 0.01,
       "tiles marked NotifyOnMelt melting, freezing and boiling: the cellMeltedInfo report"},
      {"germflow", &GermFlow, 0.01,
       "germs riding on liquid: the mover's share in all four directions"},
      {"germliq", &GermLiquids, 0.01,
       "germs through the liquid displacement slice and merge, water beside a heavier liquid"},
      {"germdisp", &GermGasDisplace, 0.5,
       "germs in gas displacement and in a gas shoved aside"},
      {"germsubl", &GermSublimate, 0.01, "germs through sublimation: merge, write, displace"},
      {"germoffgas", &GermOffGas, 0.01, "germs through the liquid off-gas path"},
      {"germore", &GermOre, 0.01, "a transition ore's germ share, and its 1 g gate"},
      {"liqvgate", &LiquidVerticalGate, 0.01,
       "liquid displacement upward: the live maxMass gate"},
      {"pour", &LiquidPour, 0.0, "gas displacement; --vacuum: 0.000122 kg over 50 ticks"},
      // Exact in an atmosphere, 0.000000 kg and 0.000000 K over 50 ticks, so unlike the
      // other liquid probes it is scored rather than reported.
      {"squeeze", &LiquidSqueeze, 0.01, "the up direction alone: liquid rising into gas"},
      {"liquid", &LiquidWorld, 0.0, "liquid in gas: the shuffle, not a liquid law"},
      {"boiling", &Boiling, 0.5,
       "state changes, no flow; Klei's own spread here reaches 0.20 K"},
      {"sunlit", &Sunlit, 0.01, "vacuum shaft; massless cells scored separately"},
      // Both diverge, and both are new information rather than a regression: these two
      // kernels had never been executed by any scenario before this one existed. See the
      // comment on `Sublimate` for what they showed.
      {"sublimate", &Sublimate, 0.01, "the crowded sublimation world"},
      {"subldisp", &SublimateDisplace, 0.01,
       "one OxyRock tile in contaminated oxygen"},
      {"sublvac", &SublimateVacuum, 0.01,
       "the same tile in vacuum, one movable gas cell"},
      // The four that found the displacement sweep's bounds. Each puts a three-cell
      // displacement where exactly one of the four clamps can refuse it, so a wrong bound
      // shows up as one cell holding the wrong element on tick 1 instead of as a kilogram
      // fifty ticks later.
      {"tracerow", &TraceRow, 0.01, "`sublvac`'s tick 6 as a one-tick world"},
      {"traceflow", &TraceFlow, 0.01,
       "the same row with the 0.12/0.125 coincidence removed"},
      {"tracetall", &TraceTall, 0.01,
       "the same row at `height - 3` of a 16-high world"},
      {"tracewall", &TraceWall, 0.01,
       "the same displacement on the first and last interior columns"},
      {"offgas", &OffGas, 0.01, "the liquid off-gas path"},
      // Regions and substeps. `tworooms` is the baseline — the same world with the single
      // full-extent region every other scenario sends — and the other four change one
      // thing each against it. They are detectors until they have been run once; see the
      // note above `TwoRooms`.
      {"tworooms", &TwoRooms, 0.5, "baseline: two sealed rooms, one region, one substep"},
      {"regionone", &TwoRooms, 0.5,
       "control: the same world with the region spelled out, not defaulted", nullptr,
       &RegionsOne},
      {"regionsplit", &TwoRooms, 0.5,
       "two disjoint regions, one per room: region-outer vs sweep-inner", nullptr,
       &RegionsSplit},
      {"regionadj", &TwoRooms, 0.0,
       "detector: two touching regions; the seam has one end in each", nullptr,
       &RegionsAdjacent},
      {"regionlap", &TwoRooms, 0.0,
       "detector: two overlapping regions; Klei runs the frame body twice over the overlap",
       nullptr, &RegionsOverlap},
      {"regioninset", &TwoRooms, 0.5,
       "control: one region, inset by one cell; is the region edge the finding", nullptr,
       &RegionsInset},
      {"regioninsetx", &TwoRooms, 0.5, "control: min_x = 1 only", nullptr, &RegionsInsetX},
      {"regioninsety", &TwoRooms, 0.5, "control: min_y = 1 only", nullptr, &RegionsInsetY},
      {"edgecol", &EdgeColumn, 0.01, "minimal: two gas columns, one region"},
      {"edgecolx", &EdgeColumn, 0.01, "minimal: the same, min_x = 1", nullptr,
       &RegionsInsetX},
      {"regiontrimx", &TwoRooms, 0.5, "control: max_x pulled in by one", nullptr,
       &RegionsTrimX},
      {"regiontrimy", &TwoRooms, 0.5, "control: max_y pulled in by one", nullptr,
       &RegionsTrimY},
      {"rampinsetx", &TwoRoomsSolid, 0.01, "the solid ramp, min_x = 1", nullptr,
       &RegionsInsetX},
      {"ramptrimx", &TwoRoomsSolid, 0.01, "the solid ramp, max_x pulled in", nullptr,
       &RegionsTrimX},
      {"ramptrimy", &TwoRoomsSolid, 0.01, "the solid ramp, max_y pulled in", nullptr,
       &RegionsTrimY},
      // The same min_x = 1 inset on two worlds that each run one kernel, to say which
      // kernel disagrees at the low edge. `heatblock` is conduction with nothing that can
      // move; `gaspockets` is the pressure sweep with no temperature gradient to speak of.
      {"edgeheat", &HeatBlock, 0.01, "min_x = 1 on the conduction world", nullptr,
       &RegionsInsetX},
      {"edgeramp", &TwoRoomsSolid, 0.01,
       "min_x = 1 on the same ramp, all solid: conduction and nothing else", nullptr,
       &RegionsInsetX},
      {"edgegas", &GasPockets, 0.5, "min_x = 1 on the gas-flow world", nullptr,
       &RegionsInsetX},
      {"edgetracer", &Tracer, 0.0, "detector: min_x = 1 on the still room", nullptr,
       &RegionsInsetX},
      {"regionleft", &TwoRooms, 0.5,
       "control: one region over one room; the frozen room must not move", nullptr,
       &RegionsLeft},
      {"substeps", &TwoRooms, 0.5,
       "one region, dt 1.0: five substeps in a frame, which nothing else here has sent",
       nullptr, nullptr, 1.0f},
      // Reported bug: a paused game (dt = 0.0 every frame) still moves gas in ours, and
      // moves it fast, because a render frame keeps calling PrepareGameData while paused
      // with no speed throttling between calls. Klei's `Sim::Main` takes the
      // `UpdateComponentsDataListOnly` branch whenever `dt <= 0` and runs no substep at
      // all; nothing in this suite had ever sent dt = 0 before, so nothing caught this.
      {"paused", &TwoRooms, 0.01,
       "one region, dt 0.0 every tick: the paused-game path must run zero substeps",
       nullptr, nullptr, 0.0f},
      // Germs. Both worlds are solid rock at rest, so these two are the only scenarios in
      // the suite whose entire content is disease; anything they report is the disease
      // kernels and nothing else, and both must be exact.
      {"germdiff", &GermDiffusion, 0.01,
       "germ diffusion, the infestation gate and the phase gate"},
      {"germgrow", &GermGrowth, 0.01,
       "germ growth and decay: five temperature bands by three population bands"},
      // Cluster messages. The world is one room split by a zone boundary and nothing else,
      // so if Klei gates any kernel on the zone this scenario cannot stay exact.
      {"zones", &ZoneSplit, 0.01,
       "world zones and per-cell zone modifications: inert, and must stay inert", &ZoneSetup},
      {"sunlight", &SunlightProbe, 0.01,
       "world offsets, the per-world sunlight texture and its absorption law", &SunlightSetup},
      {"sunliquid", &SunlightLiquid, 0.01,
       "sunlight through liquid: the gradient, the transition colour and the sideways displacement",
       &SunlightSetup},
      {"sunglass", &SunlightGlass, 0.01,
       "sunlight at a transparent solid: glass, diamond and ice, with and without the Transparent bit",
       &SunlightSetup},
      {"germthresh", &GermThreshold, 0.01,
       "diffusion in granite at exactly minDiffusionCount, which no scenario had reached"},
      {"germcons", &GermConsume, 0.01,
       "the disease consumer: a per-frame message, its rounding, its cap and its callback",
       nullptr, nullptr, 0.2f, &GermConsumeTick},
      {"vacrect", &VacuumRect, 0.01,
       "ResizeAndInitializeVacuumCells: a rectangle cleared to vacuum in both grids", nullptr,
       nullptr, 0.2f, &VacuumRectTick},
      {"germmerge", &GermMerge, 0.01,
       "two diseases meeting: the asymmetric merge, where every strength in the table is 0"},
      {"germgas", &GermGas, 0.01,
       "disease diffusion in gas, where the diffusion threshold is actually reachable"},
      {"germemit", &DiseaseEmit, 0.01,
       "the disease emitter: intervals, depth, removal, and the report list",
       &DiseaseEmitSetup, nullptr, 0.2f, &DiseaseEmitTick},
      {"germvac", &GermVacuum, 0.01,
       "germs in a zero-mass cell: the population bounds collapse when mass is 0"},
      // The `properties` byte. Every other scenario sends zero for it, which left the ten
      // branches that read it unreachable; this one is the only coverage they have.
      {"cellprops", &CellProperties, 0.01,
       "the cell properties bitfield: a gas-impermeable partition with one hole in it"},
      // `ModifyCellEnergy`: 5,377 calls in the live census, third busiest message overall. Insulation 0 in the first one, so each probe cell
      // is alone with the handler.
      {"cellenergy", &CellEnergy, 0.01,
       "ModifyCellEnergy: the ceiling, both gates, the 10000 K clamp and two per frame",
       nullptr, nullptr, 0.2f, &CellEnergyTick},
      {"cellenergyphase", &CellEnergyPhase, 0.01,
       "the same message boiling, melting and cracking a cell inside the drain",
       nullptr, nullptr, 0.2f, &CellEnergyPhaseTick},
      // The two byte-array writes at the top of Klei's frame.
      {"setinsul", &SetInsulation, 0.01,
       "SetInsulationValue: truncation, no clamp, and strength stored unscaled",
       nullptr, nullptr, 0.2f, &SetInsulationTick},
      {"msgorder", &MessageOrder, 0.01,
       "two message categories at one cell in one frame: Klei's drain is not arrival order",
       nullptr, nullptr, 0.2f, &MessageOrderTick},
    {"modifycell", &ModifyCellPaths, 0.01,
     "ModifyCell: the three replaceType paths, both clamps, removal and the addSubType gate",
     nullptr, nullptr, 0.2f, &ModifyCellPathsTick},
    {"cellmodgas", &CellModGas, 0.01,
     "AddGas: displacement, the three-neighbour scan, and the tail that destroys mass",
     nullptr, nullptr, 0.2f, &CellModGasTick},
    {"cellmodsolid", &CellModSolid, 0.01,
     "AddSolid: vertical displacement, the maxMass top-up, and OnlyIfSameElement",
     nullptr, nullptr, 0.2f, &CellModSolidTick},
    {"cellmodliq", &CellModLiquid, 0.01,
     "AddLiquid: the solid retarget, DisplaceLiquid and the tail that destroys mass",
     nullptr, nullptr, 0.2f, &CellModLiquidTick},
    // The sim's first two components, neither of which any scenario had ever registered.
    {"econsume", &ElementConsume, 0.01,
     "ElementConsumer: the four configurations, the flood, and a solid drain",
     &ElementConsumeSetup, nullptr, 0.2f, &ElementConsumeTick},
    {"econsumeempty", &ElementConsumeEmpty, 0.01,
     "ElementConsumer draining across a zero-mass cell of its own element: the 0/0 Klei's "
     "clamp scrubs and a C++ `if` does not",
     &ElementConsumeEmptySetup, nullptr, 0.2f, &ElementConsumeEmptyTick},
    {"eemit", &ElementEmit, 0.01,
     "ElementEmitter: the interval carry, the ceiling, the default temperature, callbacks",
     &ElementEmitSetup, nullptr, 0.2f, &ElementEmitTick},
    {"eemitdisp", &ElementEmitDisplace, 0.01,
     "an emitter pushing a foreign gas out of the way before it fills the cell",
     &ElementEmitDisplaceSetup, nullptr, 0.2f, &ElementEmitDisplaceTick},
    {"eemitsolid", &ElementEmitSolid, 0.01,
     "a solid emitter, which spawns ore in every reachable cell and fills none of them",
     &ElementEmitSolidSetup, nullptr, 0.2f, &ElementEmitSolidTick},
    {"massmsg", &MassMessages, 0.01,
     "MassConsumption's flood and rectangle, and MassEmission's displacement and refusal",
     nullptr, nullptr, 0.2f, &MassMessagesTick},
    // The radiation emitter, third in the component list, and the first scenarios in the
    // suite to set the world header's radiation byte at all.
    {"radiate", &Radiate, 0.01,
     "RadiationEmitter Constant: the ellipse, the square-in-radiusX box, cones, absorption",
     &RadiateSetup, nullptr, 0.2f, &RadiateTick},
    {"radpulse", &RadPulse, 0.01,
     "the pulsing sweeps: the step carry, the averaged divisor, and Register's speed clamp",
     &RadPulseSetup, nullptr, 0.2f, &RadPulseTick},
    {"radattract", &RadAttract, 0.01,
     "the attractor, which drains rads and reports only the last cell it took from",
     &RadAttractSetup, nullptr, 0.2f, &RadAttractTick},
    {"radmsg", &RadMsg, 0.01,
     "CellRadiationModification's callback and the five RadiationParamsModification types",
     &RadMsgSetup, nullptr, 0.2f, &RadMsgTick},
    {"radsource", &RadSource, 0.01,
     "the radiation field: radioactive elements, cosmic occlusion, sick germs, and the decay",
     nullptr, nullptr, 1.0f},
// The backwall payload, sent for the first time here. Also the only scenario that
  // exercises the three backwall arrays in `GameDataUpdate`, which nothing compared before.
  {"backwall", &Backwall, 0.01,
   "the backwall: hot, cold, out-of-phase and behind vacuum, with conduction switched off"},
  {"toprow", &TopRow, 0.01,
       "a pressure gradient on row height-1: is the gas sweep's extra row real"},
      {"sand", &SandFall, 0.01, "unstable solids over vacuum: the only draws are theirs"},
      {"sandgas", &SandGas, 0.01,
       "the same sand in oxygen: unstable draws against the shuffle"},
      {"falling", &Falling, 0.0,
       "liquid settling in gas: the shuffle; Klei's own spread is 6.2 K"},
      // Buildings. Nothing in these worlds can flow, so every number in them is either
      // conduction (already exact) or the building components, which makes a divergence
      // unambiguous.
      // The same box the building probes stand in, with no buildings in it. Klei reports a
      // substanceChangeInfo for every massless cell on tick 2 and we report none; that is a
      // projection difference about vacuum, not about buildings, and this is here so the
      // two are never confused again.
      {"b2bzerohc", &BuildingVoid, 0.01,
       "a contact group whose owner has no heat capacity: 1/0 times a zero proposal, which "
       "Klei's clamp scrubs",
       &BuildingZeroHeatCapacity},
      {"voidbox", &BuildingVoid, 0.01, "a vacuum pocket and nothing else; no buildings"},
      {"building", &Equilibrium, 0.01, "a hot machine in cold rock; cell exchange only",
       &BuildingHot},
      {"buildingrun", &Equilibrium, 0.01,
       "a running machine: operating heat, and the overheat event", &BuildingRunning},
      // Does a RUNNING building inside two overlapping regions have its operating heat applied
      // TWICE? `SimData::UpdateComponents` sits inside `UpdateData`'s region loop, so the
      // building kernels run once per region, and every other scenario in this suite runs one
      // region while a real colony runs many. `regionlap`'s two rects overlap on columns 12..20
      // and `BuildingRunning` plants its machine at x=7..9, outside the overlap; this pairs the
      // same overlap with a building placed INSIDE it, so the pair is a controlled comparison.
      //
      // Whatever it shows is KLEI's answer, checked against Klei's own DLL — which is the only
      // reason a vanilla reference carries a scenario like this at all.
      {"buildinglap", &Equilibrium, 0.01,
       "detector: a running machine inside two overlapping regions; is its operating heat "
       "applied once per region",
       &BuildingRunningInOverlap, &RegionsOverlap},
      {"buildingcool", &Equilibrium, 0.01,
       "a machine cooling through its overheat temperature; the clearing event",
       &BuildingCooling},
      {"buildingvac", &BuildingBox, 0.01,
       "a machine over vacuum with one tile under it; per-cell heat capacity",
       &BuildingVacuum},
      {"building2", &BuildingVoid, 0.01,
       "two machines in contact over vacuum; building-to-building only",
       &BuildingContact},
      // Element chunks — matter the game holds outside the grid. Same discipline as the
      // building probes: nothing in these worlds can flow, so every number in them is
      // conduction (already exact) or the chunk component.
      {"chunkhot", &Equilibrium, 0.01,
       "hot chunks in cold rock: cell exchange and ground exchange, three scale ratios",
       &ChunkHot},
      {"chunkvac", &BuildingBox, 0.01,
       "chunks over vacuum: one touching nothing, one on a tile, one inside it",
       &ChunkVacuum},
      {"chunkadj", &Equilibrium, 0.01,
       "the temperature adjuster, and switching it back off mid-run", &ChunkAdjuster,
       nullptr, 0.2f, &ChunkAdjusterTick},
      {"chunkmelt", &Equilibrium, 0.01,
       "the melt announcement above and below the range, with an in-range control",
       &ChunkMelt},
      {"chunkenergy", &Equilibrium, 0.01,
       "ModifyElementChunkEnergy, including a withdrawal that floors at 0 K", &ChunkEnergy,
       nullptr, 0.2f, &ChunkEnergyTick},
      {"chunkchurn", &Equilibrium, 0.01,
       "add/move/set/remove and a re-registered slot; the handle allocator", &ChunkChurn,
       nullptr, 0.2f, &ChunkChurnTick},
      {"chunkins", &ChunkInsulation, 0.01,
       "the conductivity min: cell wins insulated and bare, chunk wins in copper",
       &ChunkInsulated},
  };

  // Scenarios that reproduce a *known* open divergence. They stay in the table so the repro
  // is one command away, but `--scenario all` names them and moves on rather than running
  // them: the suite's job is to catch a regression, and a scenario that is red on purpose
  // buries a new red one. Naming them here is the whole point -- a skip nobody prints is a
  // skip nobody remembers.
  //
  // Empty: no scenario is known bad.
  const char* const known_bad[] = {""};

  for (const Entry& e : entries) {
    if (strcmp(scenario, "all") && strcmp(scenario, e.name)) continue;
    if (!strcmp(scenario, "all")) {
      bool skip = false;
      for (const char* n : known_bad) if (!strcmp(n, e.name)) skip = true;
      if (skip) {
        printf("\n=== %s === SKIPPED: known open divergence, run it by name\n", e.name);
        continue;
      }
    }
    printf("\n=== %s (%d ticks) === %s\n", e.name, ticks, e.note);
    const World w = e.build(tables);
    g_grid_width = w.width;
    const std::vector<uint8_t> payload = WorldPayload(w);
    const GameDataUpdate* a = Boot(&klei, tables, payload);
    const GameDataUpdate* b = Boot(&mine, tables, payload);
    if (!a || !b) {
      printf("  boot failed (klei %s, mine %s)\n", a ? "ok" : "null", b ? "ok" : "null");
      ++failures;
      continue;
    }
    if (e.setup) {
      e.setup(&klei, tables, w);
      e.setup(&mine, tables, w);
    }
    g_bw_klei = FrameList{};
    g_bw_mine = FrameList{};
    g_ec_klei = FrameList{};
    g_ec_mine = FrameList{};
    g_prev_chunk_infos = -1;
    // Tick 0 is the update `SIM_Initialize` hands back before any frame has run, and it is
    // the only place the two sims can disagree about a field neither of them has touched.
    Dump(dump, a, b, w.width, 0);
    bool clean = CompareUpdates(a, b, w.Count(), 0);
    int first_divergence = clean ? -1 : 0;
    uint32_t mine_rng = mine.RandomState();
    int mine_total = 0;
    size_t flow_wrong = 0;
    double flow_worst = 0.0;
    int flow_worst_tick = 0;
    const std::vector<Rect> regions = e.regions ? e.regions(w) : std::vector<Rect>();
    // The anchor is taken after `setup`, so a scenario that plants a building or sends a
    // message before tick 1 has that already accounted for rather than showing up as a
    // first-tick jump.
    double led[kLedgerCount] = {0};
    const bool have_ledger = g_ledger && mine.Ledger(led, kLedgerCount) >= kLedgerCount;
    const double ledger_anchor = have_ledger ? LedgerAnchor(led) : 0.0;
    // Transfers are float arithmetic — `d.mass = d.mass + moved` rounds, so the pair does
    // not conserve to the last bit — and the ledger is exact doubles. So the criterion is
    // rounding scale, not zero, and it is stated relative to the mass actually present.
    const double ledger_tol = have_ledger ? 1e-6 * (led[0] > 1.0 ? led[0] : 1.0) : 0.0;
    int ledger_first_tick = -1;
    for (int i = 1; i <= ticks; ++i) {
      std::vector<uint8_t> visible(w.Count(), 1);
      // Both sims get the same messages in the same order before either of them ticks, so
      // neither can observe a world the other has already stepped.
      if (e.tick_hook) {
        e.tick_hook(&klei, tables, w, i);
        e.tick_hook(&mine, tables, w, i);
      }
      a = Tick(&klei, w, &visible, e.dt, &regions);
      b = Tick(&mine, w, &visible, e.dt, &regions);
      if (!a || !b) {
        printf("  tick %d returned null (klei %s, mine %s)\n", i, a ? "ok" : "null",
               b ? "ok" : "null");
        ++failures;
        break;
      }
      // Report only the first diverging tick; after that the two worlds are different
      // and every later tick is noise.
      if (first_divergence < 0 && !CompareUpdates(a, b, w.Count(), i)) first_divergence = i;
      // The flow texture is tracked past the first divergence, and it is the only field
      // that is. Every liquid scenario parts company on tick 1 over `tex.liquidData`, a
      // colour this project does not claim to recover, and the rule above then hides
      // everything after it — so the flow texture, which is a physics result rather than a
      // palette, went unreported for its whole life. It is summarised at the end rather
      // than reported per tick because after a real divergence a per-tick line is noise.
      {
        const auto* fa = static_cast<const float*>(a->propertyTextureFlow);
        const auto* fb = static_cast<const float*>(b->propertyTextureFlow);
        if (fa && fb) {
          for (size_t c = 0; c < w.Count() * 2; ++c) {
            const double d = std::fabs(static_cast<double>(fa[c]) - fb[c]);
            if (d <= 0.0) continue;
            ++flow_wrong;
            if (d > flow_worst) {
              flow_worst = d;
              flow_worst_tick = i;
            }
          }
        }
      }
      if (have_ledger && ledger_first_tick < 0) {
        double now[kLedgerCount] = {0};
        mine.Ledger(now, kLedgerCount);
        if (std::fabs(LedgerAnchor(now) - ledger_anchor) > ledger_tol) {
          ledger_first_tick = i;
        }
      }
      // Outside `CompareUpdates` on purpose: that stops being called once a scenario has
      // diverged, and a per-frame event list scored from three ticks of data is not scored
      // at all. 
      RecordFrameList(&g_ec_klei,
                      reinterpret_cast<const int32_t*>(a->elementChunkMeltedInfos),
                      a->numElementChunkMeltedInfos, a->numFramesProcessed);
      RecordFrameList(&g_ec_mine,
                      reinterpret_cast<const int32_t*>(b->elementChunkMeltedInfos),
                      b->numElementChunkMeltedInfos, b->numFramesProcessed);
      if (g_show_chunks) DumpChunks(a, b, i);
      if (g_show_messages) DumpSubstance(a, b, i);
      if (g_show_messages) DumpUnstable(a, b, i);
      if (g_show_messages) DumpFallingLiquid(a, b, i);
      if (g_show_messages) DumpWorldDamage(a, b, i);
      if (g_show_messages) DumpCellMelted(a, b, i);
      Dump(dump, a, b, w.width, i);
      if (g_show_draws) {
        const uint32_t ma = mine.RandomState();
        const int md = StepsBetween(mine_rng, ma);
        mine_total += md;
        printf("  tick %-4d draws mine %6d (tot %8d) rng %08x\n", i, md, mine_total, ma);
        mine_rng = ma;
      }
    }
    // A NaN on either side is a divergence, not a zero. `fabs(x - NaN)` is a NaN and every
    // `if (d > max)` below is false against one, so unscored, a scenario in which one
    // sim published `nan` and the other a real temperature would score `worst temperature
    // delta 0.000000` and pass. Scoring it as an
    // infinity puts it past every envelope instead, and the printed operands still show which
    // side it was.
    auto scored = [](double d) { return d == d ? d : std::numeric_limits<double>::infinity(); };
    // Exact equality is not a usable criterion for anything with physics in it: Klei's
    // own DLL is not deterministic. Two byte-identical copies of it, given the same world
    // and the same message stream, diverge from each other at a different tick on every
    // run (measured: tick 22 on one run, 112 on the next, on this very scenario). The sim
    // runs on a worker thread and the float accumulation order evidently varies with
    // scheduling.
    //
    // So report the *envelope* — how far apart the two worlds are at the end — and read
    // it against the same measurement taken with Klei on both sides. A replacement that
    // stays inside Klei's own run-to-run spread is as close as this can measure.
    // Massless cells are scored separately. Their temperature is a don't-care for the
    // game — there is nothing there to be hot — and Klei's own answer for them is not
    // reproducible run to run, so letting them into the headline number would bury a
    // kernel that is right about everything that has mass under a 300 K difference in
    // cells that hold nothing.
    double max_delta = 0, sum_sq = 0, max_mass = 0, klei_mass = 0, mine_mass = 0;
    double max_empty_delta = 0;
    size_t counted = 0, elem_wrong = 0, empty_wrong = 0;
    if (a && b) {
      for (size_t i = 0; i < w.Count(); ++i) {
        const double d = scored(std::fabs(static_cast<double>(a->temperature[i]) -
                                          b->temperature[i]));
        if (a->mass[i] <= 0.0f && b->mass[i] <= 0.0f) {
          if (d > 1e-3) ++empty_wrong;
          if (d > max_empty_delta) max_empty_delta = d;
          continue;
        }
        if (d > max_delta) max_delta = d;
        sum_sq += d * d;
        ++counted;
        // Flow moves mass, so temperature alone stops being a sufficient summary: a
        // kernel that puts the water in the wrong cell can still have every temperature
        // right. Mass and element are the fields that matter now.
        const double m = scored(std::fabs(static_cast<double>(a->mass[i]) - b->mass[i]));
        if (m > max_mass) max_mass = m;
        if (a->elementIdx[i] != b->elementIdx[i]) ++elem_wrong;
        klei_mass += a->mass[i];
        mine_mass += b->mass[i];
      }
    }
    // Klei's own cell validator, run over BOTH published worlds. `driver/src/simcheck.h` is
    // the transcription of `SimDebugView.GetSimCheckErrorMapColour`.
    //
    // THIS IS NOT A SECOND COPY OF THE DELTA CHECK. Everything above is differential: it
    // reports how far apart the two sims are, and says nothing at all about a state both of
    // them reach. Klei's DLL can and does publish cells this check calls faulty -- a vacuum
    // cell holding a temperature is one -- and a divergence-free scenario says so without
    // noticing. The two lines below are absolute, one per sim, which makes the interesting
    // case visible: `mine` red where `klei` is not. That is a fault we introduced, in a
    // scenario whose deltas may still be inside the envelope.
    simcheck::Counts klei_check, mine_check;
    if (a && b) {
      for (size_t i = 0; i < w.Count(); ++i) {
        if (a->elementIdx[i] < tables.count) {
          klei_check.Add(*tables.At(a->elementIdx[i]), a->mass[i], a->temperature[i]);
        }
        if (b->elementIdx[i] < tables.count) {
          mine_check.Add(*tables.At(b->elementIdx[i]), b->mass[i], b->temperature[i]);
        }
      }
      printf("%s\n", klei_check.Describe("simcheck klei").c_str());
      printf("%s\n", mine_check.Describe("simcheck mine").c_str());
      if (mine_check.n[simcheck::kRed] > klei_check.n[simcheck::kRed]) {
        printf("  SIMCHECK FAIL: %llu cells are red for us and not for Klei -- a NaN, a cell"
               " over 10 t or 10 000 K, or one under 10 K.\n",
               static_cast<unsigned long long>(mine_check.n[simcheck::kRed] -
                                               klei_check.n[simcheck::kRed]));
        ++failures;
      }
    }

    // Germs, scored on their own. The headline below is a temperature envelope, and a
    // divergence in germs alone moves no temperature, so without this a scenario whose germs
    // were wrong read "within envelope" (only the per-tick
    // `identical` line and the goldens digest could see it). Germ counts are integers and
    // Klei against a copy of itself is exact, so the bar is zero: any cell whose germ type
    // or count differs on the last tick fails the scenario. Printed only when either sim
    // holds a germ, so a germ-free scenario's transcript is unchanged.
    size_t germ_wrong = 0, germ_cells = 0;
    int64_t germ_worst = 0;
    int germ_first = -1;
    if (a && b) {
      for (size_t i = 0; i < w.Count(); ++i) {
        const bool ka = a->diseaseIdx[i] != 0xFF || a->diseaseCount[i] != 0;
        const bool kb = b->diseaseIdx[i] != 0xFF || b->diseaseCount[i] != 0;
        if (ka || kb) ++germ_cells;
        if (a->diseaseIdx[i] == b->diseaseIdx[i] && a->diseaseCount[i] == b->diseaseCount[i]) {
          continue;
        }
        if (germ_first < 0) germ_first = static_cast<int>(i);
        ++germ_wrong;
        const int64_t d = std::llabs(static_cast<int64_t>(a->diseaseCount[i]) -
                                     static_cast<int64_t>(b->diseaseCount[i]));
        if (d > germ_worst) germ_worst = d;
      }
      if (germ_cells > 0) {
        printf("  after %d ticks: germs in %zu cells, %zu differ", ticks, germ_cells,
               germ_wrong);
        if (germ_wrong > 0) {
          printf(" (first at %d, worst count |delta| %lld)", germ_first,
                 static_cast<long long>(germ_worst));
        }
        printf("\n");
      }
    }

    // A building's temperature is not in any cell, so the cell sweep above cannot see it.
    // It goes into the same headline number rather than being reported beside it: a
    // building scenario whose rock is right and whose machine is 40 K out has failed.
    double max_building = 0;
    if (a && b && a->numBuildingTemperatures == b->numBuildingTemperatures) {
      for (int i = 0; i < a->numBuildingTemperatures; ++i) {
        const double d =
            scored(std::fabs(static_cast<double>(a->buildingTemperatures[i].temperature) -
                             b->buildingTemperatures[i].temperature));
        if (d > max_building) max_building = d;
      }
      if (a->numBuildingTemperatures > 0) {
        printf("  after %d ticks: %d buildings, worst temperature delta %.6f K"
               " (klei %.5f, mine %.5f)\n", ticks, a->numBuildingTemperatures,
               max_building, a->buildingTemperatures[0].temperature,
               b->buildingTemperatures[0].temperature);
        // Say what the event lists held on the last tick. Two zeroes agreeing is not
        // evidence that the events work, and a scenario meant to fire one has to show it.
        printf("  last tick events: overheat klei %d/mine %d, cleared klei %d/mine %d,"
               " melted klei %d/mine %d\n",
               a->numBuildingOverheatInfos, b->numBuildingOverheatInfos,
               a->numBuildingNoLongerOverheatedInfos,
               b->numBuildingNoLongerOverheatedInfos, a->numBuildingMeltedInfos,
               b->numBuildingMeltedInfos);
      }
      if (max_building > max_delta) max_delta = max_building;
    }
    // Reported here rather than per tick, and unconditionally rather than only before the
    // first divergence. See the tracker in the tick loop.
    if (flow_wrong) {
      printf("  flow texture: %zu cell-components differ over %d ticks, worst |delta| %.6g"
             " at tick %d\n", flow_wrong, ticks, flow_worst, flow_worst_tick);
    }
    // Read before the shutdowns, because the world goes with them.
    if (have_ledger) {
      double now[kLedgerCount] = {0};
      mine.Ledger(now, kLedgerCount);
      const double drift = LedgerAnchor(now) - ledger_anchor;
      printf("  ledger: grid %.6f kg", now[0]);
      // Only the buckets that moved. A scenario's ledger line naming a bucket it has no
      // business naming is a finding on its own, and a line of ten zeroes hides that.
      for (int f = 1; f < kLedgerCount; ++f) {
        if (now[f] != 0.0) printf(", %s %+.6g", kLedgerFields[f], now[f]);
      }
      printf("\n  ledger: drift %+.9g kg over %d ticks%s\n", drift, ticks,
             std::fabs(drift) > ledger_tol ? "  <-- UNEXPLAINED" : "");
      if (ledger_first_tick > 0) {
        printf("  ledger: drift first exceeded %.6g kg at tick %d\n", ledger_tol,
               ledger_first_tick);
      }
    }
    klei.shutdown();
    mine.shutdown();
    if (first_divergence < 0) printf("  identical for all %d ticks\n", ticks);
    else printf("  first divergence at tick %d\n", first_divergence);
    printf("  after %d ticks: max temperature delta %.6f K, rms %.6f K"
           " (over %zu cells holding mass)\n", ticks, max_delta,
           counted ? std::sqrt(sum_sq / counted) : 0.0, counted);
    if (empty_wrong) {
      printf("  after %d ticks: %zu massless cells disagree on temperature, worst %.3f K"
             " (Klei's answer here is not reproducible)\n", ticks, empty_wrong,
             max_empty_delta);
    }
    printf("  after %d ticks: max mass delta %.6f kg, %zu/%zu cells hold a different"
           " element\n", ticks, max_mass, elem_wrong, counted);
    printf("  total mass: klei %.4f kg, mine %.4f kg (%+.6f)\n", klei_mass, mine_mass,
           mine_mass - klei_mass);
    // Scored on one frame's list, in order: the game reads these one cell at a time, so a
    // matching count with the cells in a different order is still wrong.
    if (g_bw_klei.per_frame != g_bw_mine.per_frame || g_bw_klei.varied || g_bw_mine.varied) {
      printf("  backwallShouldTransition: klei %zu per frame (%zu over the run), mine %zu"
             " per frame (%zu)\n",
             g_bw_klei.per_frame.size(), g_bw_klei.total, g_bw_mine.per_frame.size(),
             g_bw_mine.total);
      if (g_bw_klei.varied || g_bw_mine.varied) {
        printf("      the list is not the same every frame (klei %s, mine %s)\n",
               g_bw_klei.varied ? "varied" : "steady",
               g_bw_mine.varied ? "varied" : "steady");
      }
      const size_t n = g_bw_klei.per_frame.size() < g_bw_mine.per_frame.size()
                           ? g_bw_klei.per_frame.size()
                           : g_bw_mine.per_frame.size();
      for (size_t i = 0; i < n; ++i) {
        if (g_bw_klei.per_frame[i] == g_bw_mine.per_frame[i]) continue;
        printf("      first differing entry %zu: klei cell %d, mine %d\n", i,
               g_bw_klei.per_frame[i], g_bw_mine.per_frame[i]);
        break;
      }
      printf("  FAILED: the backwall transition announcements do not match\n");
      ++failures;
    } else if (!g_bw_klei.per_frame.empty()) {
      printf("  backwallShouldTransition: %zu cells announced every frame, identical"
             " (klei %zu over the run, mine %zu)\n",
             g_bw_klei.per_frame.size(), g_bw_klei.total, g_bw_mine.total);
    }
    // The chunk melt announcements, scored the same way and for the same reason. Entries are
    // handles rather than cells, so the order is the component's iteration order — which the
    // compacted vector rearranges on every removal, and which is therefore part of what a
    // matching list is asserting.
    if (g_ec_klei.per_frame != g_ec_mine.per_frame || g_ec_klei.varied || g_ec_mine.varied) {
      printf("  elementChunkMelted: klei %zu per frame (%zu over the run), mine %zu per"
             " frame (%zu)\n",
             g_ec_klei.per_frame.size(), g_ec_klei.total, g_ec_mine.per_frame.size(),
             g_ec_mine.total);
      if (g_ec_klei.varied || g_ec_mine.varied) {
        printf("      the list is not the same every frame (klei %s, mine %s)\n",
               g_ec_klei.varied ? "varied" : "steady",
               g_ec_mine.varied ? "varied" : "steady");
      }
      const size_t n = g_ec_klei.per_frame.size() < g_ec_mine.per_frame.size()
                           ? g_ec_klei.per_frame.size()
                           : g_ec_mine.per_frame.size();
      for (size_t i = 0; i < n; ++i) {
        if (g_ec_klei.per_frame[i] == g_ec_mine.per_frame[i]) continue;
        printf("      first differing entry %zu: klei handle %d, mine %d\n", i,
               g_ec_klei.per_frame[i], g_ec_mine.per_frame[i]);
        break;
      }
      printf("  FAILED: the element chunk melt announcements do not match\n");
      ++failures;
    } else if (!g_ec_klei.per_frame.empty()) {
      printf("  elementChunkMelted: %zu handles announced every frame, identical"
             " (klei %zu over the run, mine %zu)\n",
             g_ec_klei.per_frame.size(), g_ec_klei.total, g_ec_mine.total);
    }
    if (e.envelope_k <= 0.0) {
      printf("  KNOWN GAP, not scored: %s\n", e.note);
    } else if (germ_wrong > 0) {
      printf("  FAILED: %zu cells hold different germs on the last tick\n", germ_wrong);
      ++failures;
    } else if (max_delta > e.envelope_k) {
      printf("  FAILED: %.6f K exceeds the %.4f K envelope\n", max_delta, e.envelope_k);
      ++failures;
    } else {
      printf("  within envelope (%.4f K, measured from Klei against a copy of itself)\n",
             e.envelope_k);
    }
  }

  if (!strcmp(scenario, "heatprobe")) {
    // Print the copper block from both sims, tick by tick, so the *pattern* of
    // propagation is visible rather than a single worst-case number. Which cells move on
    // which frame is what distinguishes one sweep scheme from another.
    printf("\n=== heatprobe: copper block rows, klei vs mine ===\n");
    const World w = HeatBlock(tables);
    const std::vector<uint8_t> payload = WorldPayload(w);
    const GameDataUpdate* a = Boot(&klei, tables, payload);
    const GameDataUpdate* b = Boot(&mine, tables, payload);
    std::vector<uint8_t> vis(w.Count(), 1);
    for (int i = 0; i <= 5; ++i) {
      printf("  tick %d\n", i);
      for (int y = 2; y <= 5; ++y) {
        printf("    y=%d klei ", y);
        for (int x = 2; x <= 7; ++x) printf("%10.5f ", a->temperature[w.Cell(x, y)]);
        printf("\n    y=%d mine ", y);
        for (int x = 2; x <= 7; ++x) printf("%10.5f ", b->temperature[w.Cell(x, y)]);
        printf("\n");
      }
      a = Tick(&klei, w, &vis);
      b = Tick(&mine, w, &vis);
      if (!a || !b) break;
    }
    // Both sims spawn a worker thread; without this the process never exits.
    klei.shutdown();
    mine.shutdown();
    return 0;
  }

  // Side-by-side grid dump. A worst-case delta says a kernel is wrong; only seeing the two
  // grids next to each other says *how*.
  if (!strncmp(scenario, "dump", 4)) {
    int x0 = 7, y0 = 11, x1 = 12, y1 = 17, dticks = 6;
    if (const char* eq = strchr(scenario, ':')) {
      sscanf(eq + 1, "%d,%d,%d,%d,%d", &x0, &y0, &x1, &y1, &dticks);
    }
    const World w = LiquidWorld(tables);
    g_grid_width = w.width;
    const std::vector<uint8_t> payload = WorldPayload(w);
    const GameDataUpdate* a = Boot(&klei, tables, payload);
    const GameDataUpdate* b = Boot(&mine, tables, payload);
    for (int i = 0; i <= dticks && a && b; ++i) {
      printf("--- tick %d  (klei above, mine below; elem/mass)\n", i);
      for (int y = y1; y >= y0; --y) {
        printf("  y=%-3d klei ", y);
        for (int x = x0; x <= x1; ++x) {
          const int c = w.Cell(x, y);
          printf("%4u:%-9.3f", a->elementIdx[c], a->mass[c]);
        }
        printf("\n        mine ");
        for (int x = x0; x <= x1; ++x) {
          const int c = w.Cell(x, y);
          printf("%4u:%-9.3f", b->elementIdx[c], b->mass[c]);
        }
        printf("\n");
      }
      std::vector<uint8_t> vis(w.Count(), 1);
      a = Tick(&klei, w, &vis);
      b = Tick(&mine, w, &vis);
    }
    klei.shutdown();
    mine.shutdown();
    return 0;
  }

  // Dump element table entries by index. The shim's field notebook records elements by
  // index and nothing else, so reading it at all needs this.
  if (!strncmp(scenario, "elements", 8)) {
    const char* list = strchr(scenario, ':');
    std::vector<int> want;
    if (list) {
      for (const char* p = list + 1; *p;) {
        want.push_back(atoi(p));
        while (*p && *p != ',') ++p;
        if (*p) ++p;
      }
    }
    // No indices given: list every element the radiation field can read, which is the only
    // way to find a radioactive one in a table that carries no names.
    if (want.empty()) {
      for (int i = 0; i < tables.count; ++i) {
        const Element* e = tables.At(i);
        if (!(e->radiationPer1000Mass > 0.0f)) continue;
        printf("radioactive idx %3d  hash %11d  state %u  radPer1000 %10.4f"
               "  radAbsorption %7.4f\n",
               i, e->id, e->state, e->radiationPer1000Mass, e->radiationAbsorptionFactor);
      }
      return 0;
    }
    for (int idx : want) {
      if (idx < 0 || idx >= tables.count) continue;
      const Element* e = tables.At(idx);
      printf("idx %3d  hash %11d  state %u (phase %u)  shc %8.4f  k %9.4f  molar %8.3f\n",
             idx, e->id, e->state, e->state & 3, e->specificHeatCapacity,
             e->thermalConductivity, e->molarMass);
      printf("         maxMass %9.2f  flow %6.3f  visc %8.3f  minH %7.3f  minV %7.3f\n",
             e->maxMass, e->flow, e->viscosity, e->minHorizontalFlow, e->minVerticalFlow);
      printf("         lowTemp %8.2f  highTemp %10.2f  lightAbsorption %8.4f"
             "  radAbsorption %7.4f\n",
             e->lowTemp, e->highTemp, e->lightAbsorptionFactor,
             e->radiationAbsorptionFactor);
      printf("         SAM solid/liquid/gas %.1f/%.1f/%.1f  colour 0x%08x  gradient[0]"
             " 0x%08x  numGradient %u\n",
             e->solidSurfaceAreaMultiplier, e->liquidSurfaceAreaMultiplier,
             e->gasSurfaceAreaMultiplier, e->colour, e->gradientColours[0],
             e->numberOfGradientColors);
      printf("         materialProperties 0x%08x  lowTransIdx %u  highTransIdx %u\n",
             e->materialProperties, e->lowTempTransitionIdx, e->highTempTransitionIdx);
      for (int k = 0; k < 6; ++k) {
        const uint32_t c = e->gradientColours[k];
        printf("         gradient[%d] 0x%08x -> bytes %3u,%3u,%3u,%3u\n", k, c,
               c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, (c >> 24) & 0xFF);
      }
    }
    klei.shutdown();
    mine.shutdown();
    return 0;
  }

  // The liquid texture's alpha, which has been unknown since the property textures went in
  // and was hardcoded to 193 on four data points.
  //
  // A live game's field notebook showed magma at 0.74 of maxMass reading 255 while an
  // offline water column at 0.73 read 221 — so it is not a plain function of fill. The
  // magma cells all had more magma above them and the water cell was the surface of its
  // column, which is the hypothesis this tests: a row of liquid at many different masses,
  // gas above every one of them, read before any physics runs.
  if (!strcmp(scenario, "liquidalpha")) {
    const uint16_t granite = static_cast<uint16_t>(tables.IndexOf(kGranite));
    const uint16_t water = static_cast<uint16_t>(tables.IndexOf(kWater));
    const uint16_t oxygen = static_cast<uint16_t>(tables.IndexOf(kOxygen));
    const Element* wp = tables.At(tables.IndexOf(kWater));
    World w;
    w.Init(24, 8, granite, 2000.0f, 293.15f);
    const float frac[20] = {0.001f, 0.005f, 0.01f, 0.02f, 0.05f, 0.10f, 0.15f, 0.20f,
                            0.25f,  0.30f,  0.40f, 0.50f, 0.60f, 0.70f, 0.75f, 0.80f,
                            0.90f,  0.95f,  0.99f, 1.00f};
    for (int i = 0; i < 20; ++i) {
      w.Set(2 + i, 2, water, frac[i] * wp->maxMass, 300.0f);
      w.Set(2 + i, 3, oxygen, 1.0f, 300.0f);   // gas above: every cell is a surface
    }
    // A control column with liquid above it, to confirm the "covered" case reads 255.
    w.Set(2, 3, water, 500.0f, 300.0f);
    const GameDataUpdate* g = Boot(&klei, tables, WorldPayload(w));
    std::vector<uint8_t> vis(w.Count(), 1);
    g = Tick(&klei, w, &vis);
    const auto* liq = static_cast<const uint8_t*>(g->propertyTextureLiquid);
    printf("water gradient stops: ");
    for (int k = 0; k < wp->numberOfGradientColors; ++k) {
      const uint32_t c = wp->gradientColours[k];
      printf("[%d] rgb %u,%u,%u a %u   ", k, c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF,
             (c >> 24) & 0xFF);
    }
    printf("\n  %-8s %-10s %-6s %-18s %s\n", "frac", "mass", "liq.a", "liq.rgb", "above");
    for (int i = 0; i < 20; ++i) {
      const int c = w.Cell(2 + i, 2);
      const int up = w.Cell(2 + i, 3);
      printf("  %-8.3f %-10.2f %-6u %3u,%3u,%3u          %s\n", frac[i], g->mass[c],
             liq[c * 4 + 3], liq[c * 4], liq[c * 4 + 1], liq[c * 4 + 2],
             g->elementIdx[up] == water ? "liquid" : "gas");
    }
    klei.shutdown();
    mine.shutdown();
    return 0;
  }

  if (!strcmp(scenario, "flow")) {
    printf("\n=== flow parameters (element table) ===\n");
    ProbeFlowParams(tables);
    printf("\n=== flow (Klei) ===\n");
    ProbeFlow(tables, klei_dll);
    printf("\n=== flow constants (Klei) ===\n");
    ProbeFlow2(tables, klei_dll);
    printf("\n=== flow: substeps, pressure, gases (Klei) ===\n");
    ProbeFlow3(tables, klei_dll);
    printf("\n=== flow: displacement and mixed elements (Klei) ===\n");
    ProbeFlow4(tables, klei_dll);
    printf("\n=== flow: vertical ordering (Klei) ===\n");
    ProbeFlow5(tables, klei_dll);
    klei.shutdown();
    mine.shutdown();
    return 0;
  }

  // The same probes, but pointed at the replacement. Reading the two outputs side by side
  // is how a transition kernel is checked: the scored scenarios below say *whether* it
  // diverges, and this says which number is wrong.
  if (!strcmp(scenario, "states-mine")) {
    printf("\n=== state changes (mine) ===\n");
    ProbeStates(tables, mine_dll);
    printf("\n=== transition boundary sweep (mine) ===\n");
    ProbeStates2(tables, mine_dll);
    printf("\n=== transition ordering, chaining, products (mine) ===\n");
    ProbeStates3(tables, mine_dll);
    printf("\n=== transition ordering (clean) and event rules (mine) ===\n");
    ProbeStates4(tables, mine_dll);
    printf("\n=== transition vs flow, sublimation (mine) ===\n");
    ProbeStates5(tables, mine_dll);
    klei.shutdown();
    mine.shutdown();
    return 0;
  }

  if (!strcmp(scenario, "sunint")) {
    printf("\n=== currentSunlightIntensity (Klei) ===\n");
    ProbeSunlightIntensity(tables, klei_dll);
    klei.shutdown();
    mine.shutdown();
    return 0;
  }

  if (!strcmp(scenario, "bounds")) {
    printf("\n=== NewGameFrame maxX / maxY (Klei) ===\n");
    ProbeFrameBounds(tables, klei_dll);
    printf("\n=== NewGameFrame maxX / maxY (mine) ===\n");
    ProbeFrameBounds(tables, mine_dll);
    klei.shutdown();
    mine.shutdown();
    return 0;
  }

  if (!strcmp(scenario, "states")) {
    printf("\n=== transition parameters (element table) ===\n");
    ProbeStateParams(tables);
    printf("\n=== state changes (Klei) ===\n");
    ProbeStates(tables, klei_dll);
    printf("\n=== transition boundary sweep (Klei) ===\n");
    ProbeStates2(tables, klei_dll);
    printf("\n=== transition ordering, chaining, products (Klei) ===\n");
    ProbeStates3(tables, klei_dll);
    printf("\n=== transition ordering (clean) and event rules (Klei) ===\n");
    ProbeStates4(tables, klei_dll);
    printf("\n=== transition vs flow, sublimation (Klei) ===\n");
    ProbeStates5(tables, klei_dll);
    printf("\n=== solid product threshold, sublimation placement (Klei) ===\n");
    ProbeStates6(tables, klei_dll);
    printf("\n=== ore-vs-cell threshold, sublimation placement (Klei) ===\n");
    ProbeStates7(tables, klei_dll);
    klei.shutdown();
    mine.shutdown();
    return 0;
  }

  if (!strcmp(scenario, "conduction")) {
    printf("\n=== conduction (Klei) ===\n");
    ProbeConduction(&klei, tables, ticks);
    printf("\n=== conduction pairs (Klei) ===\n");
    ProbeConductionPairs(nullptr, tables, klei_dll);
    printf("\n=== fluid conduction (Klei) ===\n");
    ProbeFluidConduction(tables, klei_dll);
    return 0;
  }

  if (!strcmp(scenario, "textures-sun")) {
    printf("\n=== sunlight column (Klei) ===\n");
    const World w = Sunlit(tables);
    const GameDataUpdate* gd = Boot(&klei, tables, WorldPayload(w));
    std::vector<uint8_t> visible(w.Count(), 1);
    for (int i = 0; i < 20; ++i) gd = Tick(&klei, w, &visible);
    const auto* sun = static_cast<const uint8_t*>(gd->propertyTextureExposedToSunlight);
    printf("  shaft is x=10..13, vacuum from y=6 up; granite elsewhere\n");
    {
      // What does Klei do to a massless cell on its first simulated frame, and are the
      // textures even populated before one has run?
      Backend probe;
      probe.Bind(klei_dll, "probe");
      const GameDataUpdate* s0 = Boot(&probe, tables, WorldPayload(w));
      const int c = w.Cell(11, 10);
      const auto* liq0 = static_cast<const uint8_t*>(s0->propertyTextureLiquid);
      printf("  at Start: shaft elem %u temp %.2f mass %.2f; liquid tex byte[0] %u,"
             " substanceChangeInfo %d\n",
             s0->elementIdx[c], s0->temperature[c], s0->mass[c], liq0[0],
             s0->numSubstanceChangeInfo);
      std::vector<uint8_t> vis(w.Count(), 1);
      const GameDataUpdate* s1 = Tick(&probe, w, &vis);
      printf("  after 1 tick: shaft elem %u temp %.2f mass %.2f; substanceChangeInfo %d\n",
             s1->elementIdx[c], s1->temperature[c], s1->mass[c],
             s1->numSubstanceChangeInfo);
      if (s1->numSubstanceChangeInfo > 0) {
        printf("  first change: cell %d, %u -> %u\n", s1->substanceChangeInfo[0].cellIdx,
               s1->substanceChangeInfo[0].oldElemIdx,
               s1->substanceChangeInfo[0].newElemIdx);
      }
      probe.shutdown();
    }
    for (int y = w.height - 1; y >= 0; --y) {
      printf("    y=%2d  x=9 %3u | x=11 %3u | x=12 %3u | x=15 %3u   (x=11 elem %u"
             " mass %.2f)\n",
             y, sun[w.Cell(9, y)], sun[w.Cell(11, y)], sun[w.Cell(12, y)],
             sun[w.Cell(15, y)], gd->elementIdx[w.Cell(11, y)], gd->mass[w.Cell(11, y)]);
    }
    klei.shutdown();
    return 0;
  }

  if (!strcmp(scenario, "textures") || !strcmp(scenario, "textures-eq")) {
    printf("\n=== property textures (Klei) ===\n");
    ProbeTextures(&klei, tables,
                  !strcmp(scenario, "textures-eq") ? Equilibrium(tables)
                                                   : Falling(tables));
    printf("klei frames: %ld over %ld ticks, %ld spins\n", g_frames, g_ticks, g_spins);
  printf("\n%s\n", failures ? "FAILURES" : "done");
    return failures ? 1 : 0;
  }

  if (!strcmp(scenario, "all") || !strcmp(scenario, "crossload")) {
    printf("\n=== crossload ===\n");
    CrossLoad(&klei, &mine, tables, Equilibrium(tables));
  }

  printf("klei frames: %ld over %ld ticks, %ld spins\n", g_frames, g_ticks, g_spins);
  printf("\n%s\n", failures ? "FAILURES" : "done");
  return failures ? 1 : 0;
}
