// Offline driver for Klei's SimDLL.
//
// Boots the real native simulation with no game process: builds a minimal element
// table, allocates a small grid, seeds it, and ticks it. Prints the resulting cell
// state read straight out of GameDataUpdate.
//
// Point of this: the shim can only observe traffic the game happens to produce. This
// drives the sim directly, so message layouts can be exercised on demand, physics
// behaviour can be probed, and a replacement sim can be diffed against the original
// tick-for-tick — all without launching Oxygen Not Included.
//
// What it cannot do: prove a payload matches what the *game* sends. The sim will
// happily accept a struct whose fields are in the wrong order. Only the shim, running
// against the real game, is authoritative for that. The two tools are complementary.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../abi/sim_abi.h"

using namespace oni_sim;

namespace {

// ---------------------------------------------------------------- native binding

HMODULE g_sim = nullptr;

using SIM_Initialize_t = void (*)(int (*)(int, void*));
using SIM_Shutdown_t = void (*)();
using SIM_HandleMessage_t = void* (*)(int, int, const uint8_t*);

SIM_Initialize_t sim_initialize;
SIM_Shutdown_t sim_shutdown;
SIM_HandleMessage_t sim_handle_message;

int GameMessageHandler(int message_id, void* data) {
  // 0 = ExceptionHandler, 1 = ReportMessage (Sim.GameHandledMessages). Both carry a
  // struct whose first field is a char* message, so print it for diagnostics.
  const char* text = data ? *reinterpret_cast<const char* const*>(data) : nullptr;
  printf("  [sim->game] id=%d %s\n", message_id, text ? text : "(no text)");
  return 0;
}

bool Bind(const char* dll) {
  g_sim = LoadLibraryA(dll);
  if (!g_sim) {
    printf("LoadLibrary failed for %s (err %lu)\n", dll, GetLastError());
    return false;
  }
  sim_initialize = (SIM_Initialize_t)GetProcAddress(g_sim, "SIM_Initialize");
  sim_shutdown = (SIM_Shutdown_t)GetProcAddress(g_sim, "SIM_Shutdown");
  sim_handle_message = (SIM_HandleMessage_t)GetProcAddress(g_sim, "SIM_HandleMessage");
  return sim_initialize && sim_shutdown && sim_handle_message;
}

// ---------------------------------------------------------------- payload writer

// Mirrors System.IO.BinaryWriter: little-endian, no padding, bool as one byte,
// strings as an int32 UTF-8 byte count followed by the bytes ("Klei string").
struct Writer {
  std::vector<uint8_t> bytes;

  template <typename T>
  void Put(const T& v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    bytes.insert(bytes.end(), p, p + sizeof(T));
  }
  void PutBool(bool v) { bytes.push_back(v ? 1 : 0); }
  void PutString(const std::string& s) {
    Put<int32_t>(static_cast<int32_t>(s.size()));
    bytes.insert(bytes.end(), s.begin(), s.end());
  }
  void PutRaw(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    bytes.insert(bytes.end(), b, b + n);
  }
};

void* Send(SimMessageHash hash, const Writer& w) {
  return sim_handle_message(static_cast<int32_t>(hash),
                            static_cast<int>(w.bytes.size()),
                            w.bytes.empty() ? nullptr : w.bytes.data());
}

void* SendEmpty(SimMessageHash hash) {
  return sim_handle_message(static_cast<int32_t>(hash), 0, nullptr);
}

// ---------------------------------------------------------------- element table

// SimHashes values, from the game's `SimHashes` enum.
enum : int32_t {
  kVacuum = 758759285,
  kOxygen = -1528777920,
  kWater = 1836671383,
  kSteam = -899515856,
  kIce = 873952427,
  kGranite = -105943486,
};

// Element.state packs phase in the low 2 bits (0 vacuum, 1 gas, 2 liquid, 3 solid).
enum : uint8_t { kStateVacuum = 0, kStateGas = 1, kStateLiquid = 2, kStateSolid = 3 };

struct ElementDef {
  int32_t id;
  const char* name;
  uint8_t state;
  float specificHeatCapacity;
  float thermalConductivity;
  float molarMass;
  float maxMass;
  float lowTemp;
  float highTemp;
  uint16_t lowTempTransitionIdx;
  uint16_t highTempTransitionIdx;
};

// Index order is the element table order; elementIdx in every other message refers to
// these positions, so the transition indices below are positional, not hashes.
constexpr uint16_t kNone = 0xFFFF;
const ElementDef kElements[] = {
    // id        name        state          shc     tc     molar  maxMass low     high
    {kVacuum,  "Vacuum",  kStateVacuum,  0.0f,   0.0f,  0.0f,   0.0f,   0.0f,   0.0f,        kNone, kNone},
    {kOxygen,  "Oxygen",  kStateGas,     1.005f, 0.024f, 32.0f, 1.0f,   -1.0f,  9000.0f,     kNone, kNone},
    {kWater,   "Water",   kStateLiquid,  4.179f, 0.609f, 18.0f, 1000.0f, 273.15f, 372.75f,   4,     3},
    {kSteam,   "Steam",   kStateGas,     4.179f, 0.184f, 18.0f, 1.0f,   372.75f, 9000.0f,    2,     kNone},
    {kIce,     "Ice",     kStateSolid,   2.05f,  2.18f,  18.0f, 1000.0f, 0.5f,   273.15f,    kNone, 2},
    {kGranite, "Granite", kStateSolid,   0.79f,  3.39f,  100.0f, 1000.0f, 0.5f,  1210.0f,    kNone, kNone},
};
constexpr int kNumElements = sizeof(kElements) / sizeof(kElements[0]);

// Positional indices, matching the table above.
enum : uint16_t { kIdxVacuum = 0, kIdxOxygen = 1, kIdxWater = 2, kIdxSteam = 3, kIdxIce = 4, kIdxGranite = 5 };

// Live indices into whichever element table was uploaded. Default to the synthetic
// table's positions; ResolveIndices overwrites them when a real table is used.
uint16_t g_idxVacuum = 0, g_idxOxygen = 1, g_idxWater = 2, g_idxGranite = 5;

// Pull the Elements_CreateTable and Disease_CreateTable payloads out of a shim corpus.
// Record format: int32 seq | int32 id | int32 length | int32 count | length*count bytes.
bool LoadTablesFromCorpus(const char* path, std::vector<uint8_t>* elements,
                          std::vector<uint8_t>* diseases) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  for (;;) {
    int32_t h[4];
    if (fread(h, sizeof(h), 1, f) != 1) break;
    const size_t bytes = static_cast<size_t>(h[2]) * (h[3] > 0 ? h[3] : 1);
    std::vector<uint8_t> payload(bytes);
    if (bytes && fread(payload.data(), 1, bytes, f) != bytes) break;
    if (h[1] == static_cast<int32_t>(SimMessageHash::Elements_CreateTable) &&
        elements->empty()) {
      *elements = std::move(payload);
    } else if (h[1] == static_cast<int32_t>(SimMessageHash::Disease_CreateTable) &&
               diseases->empty()) {
      *diseases = std::move(payload);
    }
    if (!elements->empty() && !diseases->empty()) break;
  }
  fclose(f);
  return !elements->empty() && !diseases->empty();
}

// The table is int32 count followed by count packed Elements; each Element opens with
// its SimHashes id, so positions can be recovered by scanning for the hashes we need.
bool ResolveIndices(const std::vector<uint8_t>& table) {
  if (table.size() < 4) return false;
  int32_t count = 0;
  memcpy(&count, table.data(), 4);
  if (count <= 0 || 4 + static_cast<size_t>(count) * sizeof(Element) > table.size()) {
    return false;
  }
  auto find = [&](int32_t hash, uint16_t* out) {
    for (int32_t i = 0; i < count; ++i) {
      int32_t id = 0;
      memcpy(&id, table.data() + 4 + static_cast<size_t>(i) * sizeof(Element), 4);
      if (id == hash) {
        *out = static_cast<uint16_t>(i);
        return true;
      }
    }
    return false;
  };
  const bool ok = find(kVacuum, &g_idxVacuum) && find(kOxygen, &g_idxOxygen) &&
                  find(kWater, &g_idxWater) && find(kGranite, &g_idxGranite);
  printf("  resolved indices: vacuum=%u oxygen=%u water=%u granite=%u (of %d)\n",
         g_idxVacuum, g_idxOxygen, g_idxWater, g_idxGranite, count);
  return ok;
}

Writer BuildElementTable() {
  Writer w;
  w.Put<int32_t>(kNumElements);
  for (int i = 0; i < kNumElements; ++i) {
    const ElementDef& d = kElements[i];
    Element e{};
    e.id = d.id;
    e.lowTempTransitionIdx = d.lowTempTransitionIdx;
    e.highTempTransitionIdx = d.highTempTransitionIdx;
    e.elementsTableIdx = static_cast<uint16_t>(i);
    e.state = d.state;
    e.numberOfGradientColors = 0;
    e.specificHeatCapacity = d.specificHeatCapacity;
    e.thermalConductivity = d.thermalConductivity;
    e.molarMass = d.molarMass;
    e.solidSurfaceAreaMultiplier = 1.0f;
    e.liquidSurfaceAreaMultiplier = 1.0f;
    e.gasSurfaceAreaMultiplier = 1.0f;
    e.flow = 1.0f;
    e.viscosity = 1.0f;
    e.minHorizontalFlow = 0.01f;
    e.minVerticalFlow = 0.01f;
    e.maxMass = d.maxMass;
    e.lowTemp = d.lowTemp;
    e.highTemp = d.highTemp;
    e.strength = 1.0f;
    e.lowTempTransitionOreID = 0;
    e.lowTempTransitionOreMassConversion = 0.0f;
    e.highTempTransitionOreID = 0;
    e.highTempTransitionOreMassConversion = 0.0f;
    e.sublimateIndex = kNone;
    e.convertIndex = kNone;
    e.materialProperties = 0;
    e.colour = 0xFFFFFFFFu;
    e.sublimateFX = 0;
    e.defaultValues.temperature = 293.15f;
    e.defaultValues.mass = d.maxMass;
    e.defaultValues.pressure = 0.0f;
    // Element.Write serialises field-by-field with no padding, and sizeof(Element)
    // happens to equal that byte count (asserted in sim_abi.h), so a raw copy is the
    // wire format.
    w.PutRaw(&e, sizeof(Element));
  }
  for (int i = 0; i < kNumElements; ++i) w.PutString(kElements[i].name);
  return w;
}

// ---------------------------------------------------------------- world

constexpr int kWidth = 32;
constexpr int kHeight = 32;
constexpr int kNumCells = kWidth * kHeight;

Writer BuildWorld() {
  Writer w;
  w.Put<int32_t>(kWidth);
  w.Put<int32_t>(kHeight);
  w.Put<uint32_t>(12345u);  // simSeed
  w.PutBool(false);         // radiationEnabled
  w.PutBool(true);          // headless

  // Bottom two rows granite floor, a pool of water above it, oxygen elsewhere.
  for (int i = 0; i < kNumCells; ++i) {
    int y = i / kWidth, x = i % kWidth;
    Cell c{};
    if (y < 2) {
      c.elementIdx = g_idxGranite;
      c.mass = 2000.0f;
      c.temperature = 293.15f;
    } else if (y < 6 && x >= 8 && x < 24) {
      c.elementIdx = g_idxWater;
      c.mass = 1000.0f;
      c.temperature = 293.15f;
    } else {
      c.elementIdx = g_idxOxygen;
      c.mass = 1.0f;
      c.temperature = 293.15f;
    }
    c.insulation = 255;
    // Cell.Write zeroes properties and strengthInfo on the wire regardless of the
    // in-memory values; mirror that.
    c.properties = 0;
    c.strengthInfo = 0;
    w.PutRaw(&c, sizeof(Cell));
  }
  for (int i = 0; i < kNumCells; ++i) {
    DiseaseCell d{};
    d.diseaseIdx = 0xFF;
    d.elementCount = 0;
    w.PutRaw(&d, sizeof(DiseaseCell));
  }
  for (int i = 0; i < kNumCells; ++i) {
    SimBackwall b{};
    b.elementIdx = g_idxVacuum;
    b.mass = 0.0f;
    b.temperature = 0.0f;
    w.PutRaw(&b, sizeof(SimBackwall));
  }
  return w;
}

void DumpColumn(const GameDataUpdate* g, int x, const char* label) {
  printf("  %s column x=%d (bottom-up): ", label, x);
  for (int y = 0; y < 10; ++y) {
    int cell = y * kWidth + x;
    printf("[%u %.1fkg %.1fK] ", g->elementIdx[cell], g->mass[cell], g->temperature[cell]);
  }
  printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
  // Unbuffered: the sim spawns worker threads and can block or abort inside a message,
  // so buffered output would be lost exactly when it matters most.
  setvbuf(stdout, nullptr, _IONBF, 0);

  const char* corpus_path = nullptr;
  int g_ticks = 20;
  bool g_profile = false;
  const char* dll = argc > 1 ? argv[1]
                             : "SimDLL_orig.dll";
  // argv[1] is the DLL (kept for compatibility); --corpus <path> supplies the real
  // element and disease tables.
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--corpus") && i + 1 < argc) corpus_path = argv[++i];
    else if (!strcmp(argv[i], "--ticks") && i + 1 < argc) g_ticks = atoi(argv[++i]);
    // Wrap the tick loop in ToggleProfiler, which is what the backtick key does in game.
    // Klei's DLL answers it with its own kprofiler; ours answers it with per-kernel
    // timings. This is the only way to exercise that path without launching the game.
    else if (!strcmp(argv[i], "--profile")) g_profile = true;
  }

  if (!Bind(dll)) return 1;
  printf("bound %s\n", dll);

  sim_initialize(&GameMessageHandler);
  printf("SIM_Initialize ok\n");

  // Element and disease tables: prefer the real ones lifted out of a shim corpus.
  //
  // A hand-built table is what makes the tick fault -- replaying the game's own table
  // ticks fine, a synthetic six-element one dies with a read at -1. Rather than keep
  // guessing which field the sim dereferences, borrow the real tables and keep full
  // control of the world itself, which is the part worth experimenting on.
  std::vector<uint8_t> real_elements, real_diseases;
  if (corpus_path && LoadTablesFromCorpus(corpus_path, &real_elements, &real_diseases)) {
    printf("using real tables from corpus: elements %zu bytes, diseases %zu bytes\n",
           real_elements.size(), real_diseases.size());
    sim_handle_message(static_cast<int32_t>(SimMessageHash::Elements_CreateTable),
                       static_cast<int>(real_elements.size()), real_elements.data());
    sim_handle_message(static_cast<int32_t>(SimMessageHash::Disease_CreateTable),
                       static_cast<int>(real_diseases.size()), real_diseases.data());
    if (!ResolveIndices(real_elements)) {
      printf("FAILED: could not find the needed elements in the real table\n");
      return 1;
    }
  } else {
    Writer elements = BuildElementTable();
    printf("using synthetic table: %zu bytes for %d elements "
           "(known to fault in the tick -- pass a corpus)\n",
           elements.bytes.size(), kNumElements);
    Send(SimMessageHash::Elements_CreateTable, elements);
    Writer diseases;
    diseases.Put<int32_t>(0);
    diseases.Put<int32_t>(kNumElements);
    Send(SimMessageHash::Disease_CreateTable, diseases);
  }

  // Nothing else goes here. A captured worldgen boot is exactly:
  //   Elements_CreateTable, Disease_CreateTable, SimData_InitializeFromCells, Start
  // then alternating NewGameFrame / PrepareGameData. There is no AllocateCells,
  // DefineWorldOffsets, SetWorldZones or ClearUnoccupiedCells -- those belong to the
  // main game load, and SimData_InitializeFromCells does its own allocation from the
  // width/height in its own header.

  Writer world = BuildWorld();
  printf("SimData_InitializeFromCells: %zu bytes (expect 14 + 40*%d = %d)\n",
         world.bytes.size(), kNumCells, 14 + 40 * kNumCells);
  Send(SimMessageHash::SimData_InitializeFromCells, world);

  void* start = SendEmpty(SimMessageHash::Start);
  printf("Start -> GameDataUpdate* = %p\n", start);
  if (!start) {
    printf("FAILED: Start returned null\n");
    sim_shutdown();
    return 1;
  }
  const GameDataUpdate* g = static_cast<const GameDataUpdate*>(start);
  DumpColumn(g, 16, "initial");

  // Tick: NewGameFrame queues the step, PrepareGameData runs it and returns the update.
  std::vector<uint8_t> visible(kNumCells, 1);
  const int kTicks = g_ticks;
  if (g_profile) {
    sim_handle_message(static_cast<int32_t>(SimMessageHash::ToggleProfiler), 0, nullptr);
  }
  for (int t = 0; t < kTicks; ++t) {
    NewGameFrame frame{};
    frame.elapsedSeconds = 0.2f;
    frame.minX = 0;
    frame.minY = 0;
    frame.maxX = kWidth;
    frame.maxY = kHeight - 1;
    frame.currentSunlightIntensity = 0.0f;
    frame.currentCosmicRadiationIntensity = 0.0f;
    // No SetDebugProperties here: the game sends it every frame in the main loop, but
    // a captured worldgen settle sim sends only NewGameFrame + PrepareGameData.
    if (t == 0) printf("  tick0: sending NewGameFrame (%zu bytes)\n", sizeof(NewGameFrame));
    sim_handle_message(static_cast<int32_t>(SimMessageHash::SimFrameManager_NewGameFrame),
                       sizeof(NewGameFrame), reinterpret_cast<const uint8_t*>(&frame));
    if (t == 0) printf("  tick0: NewGameFrame ok, sending PrepareGameData\n");

    void* upd = sim_handle_message(
        static_cast<int32_t>(SimMessageHash::PrepareGameData),
        static_cast<int>(visible.size()), visible.data());
    if (!upd) {
      printf("FAILED: PrepareGameData returned null at tick %d\n", t);
      sim_shutdown();
      return 1;
    }
    g = static_cast<const GameDataUpdate*>(upd);
  }
  if (g_profile) {
    sim_handle_message(static_cast<int32_t>(SimMessageHash::ToggleProfiler), 0, nullptr);
  }
  printf("ticked %d frames, numFramesProcessed=%d\n", kTicks, g->numFramesProcessed);
  DumpColumn(g, 16, "final  ");

  sim_shutdown();
  printf("SIM_Shutdown ok\n");
  return 0;
}
