// Measure ONI's simulation quirks against the real SimDLL, offline.
//
// The project thesis is that the exploits people complain about come from the
// one-element-per-cell data model rather than from cell size, and that volume fractions
// therefore fix more than subdivision would. That is a claim about the actual sim, so
// it should be measured before anything is built on it.
//
// Each scenario seeds a small world, ticks it, and reports conservation and layout.
// Every number comes from running the game's own simulation.
//
// Usage:
//   experiments.exe --corpus <corpus.bin> [--dll <path>] [--scenario <name>] [--ticks N]

#include <cmath>
#include <cstdlib>

#include "simhost.h"

using namespace simhost;

namespace {

struct World {
  int width = 0;
  int height = 0;
  std::vector<uint16_t> element;
  std::vector<float> mass;
  std::vector<float> temperature;

  void Init(int w, int h, uint16_t fill_element, float fill_mass, float temp) {
    width = w;
    height = h;
    const size_t n = static_cast<size_t>(w) * h;
    element.assign(n, fill_element);
    mass.assign(n, fill_mass);
    temperature.assign(n, temp);
  }
  int Cell(int x, int y) const { return y * width + x; }
  void Set(int x, int y, uint16_t e, float m, float t) {
    const int c = Cell(x, y);
    element[c] = e;
    mass[c] = m;
    temperature[c] = t;
  }
  size_t Count() const { return element.size(); }
};

// Totals are computed from the sim's own arrays, so they measure what the sim actually
// holds rather than what it was asked to hold.
struct Totals {
  double mass = 0;
  double energy = 0;  // sum of mass * specific heat * temperature
};

Totals Measure(const GameDataUpdate* g, size_t cells, const Tables& t) {
  Totals out;
  for (size_t i = 0; i < cells; ++i) {
    const float m = g->mass[i];
    if (!(m > 0)) continue;  // also skips NaN
    out.mass += m;
    out.energy += static_cast<double>(m) * t.SpecificHeat(g->elementIdx[i]) *
                  g->temperature[i];
  }
  return out;
}

void SendWorld(const World& w) {
  Writer b;
  b.Put<int32_t>(w.width);
  b.Put<int32_t>(w.height);
  b.Put<uint32_t>(12345u);
  b.PutBool(false);  // radiation
  b.PutBool(true);   // headless
  for (size_t i = 0; i < w.Count(); ++i) {
    Cell c{};
    c.elementIdx = w.element[i];
    c.mass = w.mass[i];
    c.temperature = w.temperature[i];
    c.insulation = 255;
    b.PutRaw(&c, sizeof(Cell));
  }
  for (size_t i = 0; i < w.Count(); ++i) {
    DiseaseCell d{};
    d.diseaseIdx = 0xFF;
    b.PutRaw(&d, sizeof(DiseaseCell));
  }
  for (size_t i = 0; i < w.Count(); ++i) {
    SimBackwall bw{};
    bw.elementIdx = 0;
    b.PutRaw(&bw, sizeof(SimBackwall));
  }
  Send(SimMessageHash::SimData_InitializeFromCells, b);
}

// A worldgen boot is exactly this, in this order, and nothing else.
const GameDataUpdate* Boot(const Tables& t, const World& w) {
  SendRaw(SimMessageHash::Elements_CreateTable, t.elements);
  SendRaw(SimMessageHash::Disease_CreateTable, t.diseases);
  SendWorld(w);
  return static_cast<const GameDataUpdate*>(SendEmpty(SimMessageHash::Start));
}

const GameDataUpdate* Tick(const World& w, std::vector<uint8_t>* visible) {
  NewGameFrame f{};
  f.elapsedSeconds = 0.2f;
  f.minX = 0;
  f.minY = 0;
  f.maxX = w.width;
  f.maxY = w.height - 1;
  sim_handle_message(static_cast<int32_t>(SimMessageHash::SimFrameManager_NewGameFrame),
                     sizeof(NewGameFrame), reinterpret_cast<const uint8_t*>(&f));
  return static_cast<const GameDataUpdate*>(
      sim_handle_message(static_cast<int32_t>(SimMessageHash::PrepareGameData),
                         static_cast<int>(visible->size()), visible->data()));
}

void ReportDrift(const char* label, const Totals& a, const Totals& b) {
  const double dm = b.mass - a.mass;
  const double de = b.energy - a.energy;
  printf("  %-22s mass %14.3f -> %14.3f  (%+.3f kg, %+.6f%%)\n", label, a.mass, b.mass,
         dm, a.mass > 0 ? 100.0 * dm / a.mass : 0.0);
  printf("  %-22s energy %12.1f -> %12.1f  (%+.1f, %+.6f%%)\n", "", a.energy, b.energy,
         de, a.energy > 0 ? 100.0 * de / a.energy : 0.0);
}

void DumpColumn(const GameDataUpdate* g, const World& w, int x, int rows,
                const char* label) {
  printf("  %-10s x=%d: ", label, x);
  for (int y = 0; y < rows && y < w.height; ++y) {
    const int c = w.Cell(x, y);
    printf("[%u %.1fkg] ", g->elementIdx[c], g->mass[c]);
  }
  printf("\n");
}

// IndexOf returns -1 for a hash that is not in the table; casting that to uint16_t
// yields 65535, which is a valid-looking element index that silently poisons a whole
// scenario. Fail loudly instead.
bool Resolve(const Tables& t, int32_t hash, const char* name, uint16_t* out) {
  const int32_t i = t.IndexOf(hash);
  if (i < 0) {
    printf("  FAILED: element %s (hash %d) is not in the table\n", name, hash);
    return false;
  }
  *out = static_cast<uint16_t>(i);
  return true;
}

// ------------------------------------------------------- mass leaving the grid
//
// The sim does not only move mass between cells: it hands mass *out* of the grid to
// the game as falling-sand objects, ore, and falling-water particles, and buildings
// push mass in and pull it out. Ignoring those transfers makes a correct sim look
// like it leaks, which is exactly the false positive the earlier scenarios avoided
// only because they had no buildings and no unstable solids in them.
//
// Sources are read from Game.OnSimDataUpdate: digInfo/spawnOreInfo become resources,
// spawnFallingLiquidInfo becomes a FallingWater particle, unstableCellInfo with
// fallingInfo == 0 becomes an UnstableGroundManager object, removedMassEntries feed
// ElementConsumer.AddMass, and emittedMassEntries come from element emitters.
struct Ledger {
  double out_ore = 0;       // digInfo + spawnOreInfo
  double out_falling = 0;   // spawnFallingLiquidInfo
  double out_unstable = 0;  // unstableCellInfo, fallingInfo == 0
  double out_consumed = 0;  // removedMassEntries + massConsumedCallbacks
  double in_emitted = 0;    // emittedMassEntries + successful massEmittedCallbacks
  int emit_ok = 0;
  int emit_refused = 0;

  double NetOut() const {
    return out_ore + out_falling + out_unstable + out_consumed - in_emitted;
  }
};

void Account(const GameDataUpdate* g, Ledger* l) {
  for (int i = 0; i < g->numDigInfo; ++i) l->out_ore += g->digInfo[i].mass;
  for (int i = 0; i < g->numSpawnOreInfo; ++i) l->out_ore += g->spawnOreInfo[i].mass;
  for (int i = 0; i < g->numSpawnFallingLiquidInfo; ++i) {
    l->out_falling += g->spawnFallingLiquidInfo[i].mass;
  }
  for (int i = 0; i < g->numUnstableCellInfo; ++i) {
    if (g->unstableCellInfo[i].fallingInfo == 0) l->out_unstable += g->unstableCellInfo[i].mass;
  }
  for (int i = 0; i < g->numRemovedMassEntries; ++i) {
    l->out_consumed += g->removedMassEntries[i].mass;
  }
  for (int i = 0; i < g->numMassConsumedCallbacks; ++i) {
    l->out_consumed += g->massConsumedCallbacks[i].mass;
  }
  for (int i = 0; i < g->numEmittedMassEntries; ++i) {
    l->in_emitted += g->emittedMassEntries[i].mass;
  }
  for (int i = 0; i < g->numMassEmittedCallbacks; ++i) {
    const MassEmittedCallback& e = g->massEmittedCallbacks[i];
    if (e.suceeded) {
      l->in_emitted += e.mass;
      ++l->emit_ok;
    } else {
      ++l->emit_refused;
    }
  }
}

// grid_after should equal grid_before minus everything the sim handed out plus
// everything it was given. Any residual is mass the sim created or destroyed.
void ReportBalance(const char* label, const Totals& a, const Totals& b, const Ledger& l) {
  const double expected = a.mass - l.NetOut();
  const double residual = b.mass - expected;
  printf("  %-22s grid %14.3f -> %14.3f\n", label, a.mass, b.mass);
  printf("  %-22s out: ore %.3f, falling %.3f, unstable %.3f, consumed %.3f;"
         " in: emitted %.3f\n", "", l.out_ore, l.out_falling, l.out_unstable,
         l.out_consumed, l.in_emitted);
  printf("  %-22s expected %14.3f, residual %+.4f kg (%+.6f%%)\n", "", expected,
         residual, a.mass > 0 ? 100.0 * residual / a.mass : 0.0);
}

// ------------------------------------------------------------ building plumbing

// Sim handles are not returned by SIM_HandleMessage. The sim reports them back in
// componentStateChangedMessages on a later tick, keyed by the callback index that the
// registration message carried, so a registration is only usable after a tick.
struct HandleTable {
  std::vector<std::pair<int32_t, int32_t>> seen;

  void Absorb(const GameDataUpdate* g) {
    for (int i = 0; i < g->numComponentStateChangedMessages; ++i) {
      seen.emplace_back(g->componentStateChangedMessages[i].callbackIdx,
                        g->componentStateChangedMessages[i].simHandle);
    }
  }
  int32_t Find(int32_t cb) const {
    for (const auto& p : seen) {
      if (p.first == cb) return p.second;
    }
    return -1;
  }
};

template <typename T>
void SendStruct(SimMessageHash hash, const T& msg) {
  sim_handle_message(static_cast<int32_t>(hash), sizeof(T),
                     reinterpret_cast<const uint8_t*>(&msg));
}

void ConsumeMass(int cell, uint16_t element, float mass, uint8_t radius, int cb) {
  MassConsumptionMessage m{};
  m.cellIdx = cell;
  m.callbackIdx = cb;
  m.mass = mass;
  m.elementIdx = element;
  m.radius = radius;
  m.height = 0;
  SendStruct(SimMessageHash::MassConsumption, m);
}

void EmitMass(int cell, uint16_t element, float mass, float temperature, int cb) {
  MassEmissionMessage m{};
  m.cellIdx = cell;
  m.callbackIdx = cb;
  m.mass = mass;
  m.temperature = temperature;
  m.elementIdx = element;
  m.diseaseIdx = 0xFF;
  m.diseaseCount = 0;
  SendStruct(SimMessageHash::MassEmission, m);
}

void AddElementConsumer(int cell, uint8_t configuration, uint16_t element, uint8_t radius,
                        int cb) {
  AddElementConsumerMessage m{};
  m.cellIdx = cell;
  m.callbackIdx = cb;
  m.radius = radius;
  m.configuration = configuration;
  m.elementIdx = element;
  SendStruct(SimMessageHash::AddElementConsumer, m);
}

void SetElementConsumerData(int handle, int cell, float rate) {
  SetElementConsumerDataMessage m{};
  m.handle = handle;
  m.cell = cell;
  m.consumptionRate = rate;
  SendStruct(SimMessageHash::SetElementConsumerData, m);
}

void AddElementEmitter(float max_pressure, int cb) {
  AddElementEmitterMessage m{};
  m.maxPressure = max_pressure;
  m.callbackIdx = cb;
  m.onBlockedCB = -1;
  m.onUnblockedCB = -1;
  SendStruct(SimMessageHash::AddElementEmitter, m);
}

void ModifyElementEmitter(int handle, int cell, uint16_t element, float interval,
                          float mass, float temperature, float max_pressure) {
  ModifyElementEmitterMessage m{};
  m.handle = handle;
  m.cellIdx = cell;
  m.emitInterval = interval;
  m.emitMass = mass;
  m.emitTemperature = temperature;
  m.maxPressure = max_pressure;
  m.diseaseCount = 0;
  m.elementIdx = element;
  m.maxDepth = 1;
  m.diseaseIdx = 0xFF;
  SendStruct(SimMessageHash::ModifyElementEmitter, m);
}

void AddBuildingHeatExchange(int min_x, int min_y, int max_x, int max_y, uint16_t element,
                             float mass, float temperature, float conductivity,
                             float operating_kw, int cb) {
  AddBuildingHeatExchangeMessage m{};
  m.callbackIdx = cb;
  m.elemIdx = element;
  m.mass = mass;
  m.temperature = temperature;
  m.thermalConductivity = conductivity;
  m.overheatTemperature = 3.4028235e38f;  // float.MaxValue, as the game sends
  m.operatingKilowatts = operating_kw;
  m.minX = min_x;
  m.minY = min_y;
  m.maxX = max_x;
  m.maxY = max_y;
  SendStruct(SimMessageHash::AddBuildingHeatExchange, m);
}

// ModifyCell is how the game puts substance into the world: falling sand landing,
// a building displacing what was there, a pipe emptying. replaceType is
// None/Replace/ReplaceAndDisplace and addSubType is inverted in the game's sender —
// 0 means *do* vertical solid displacement, 1 means only-if-same-element.
void ModifyCell(int cell, uint16_t element, float temperature, float mass,
                uint8_t replace_type, bool vertical_displacement, int cb) {
  ModifyCellMessage m{};
  m.cellIdx = cell;
  m.callbackIdx = cb;
  m.temperature = temperature;
  m.mass = mass;
  m.diseaseCount = 0;
  m.elementIdx = element;
  m.replaceType = replace_type;
  m.diseaseIdx = 0xFF;
  m.addSubType = vertical_displacement ? 0 : 1;
  SendStruct(SimMessageHash::ModifyCell, m);
}

void Dig(int cell, int cb) {
  DigMessage m{};
  m.cellIdx = cell;
  m.callbackIdx = cb;
  m.skipEvent = 0;
  m.backwall = 0;
  SendStruct(SimMessageHash::Dig, m);
}

// A sealed rectangular room of `fill`, walled in granite.
World SealedRoom(int w, int h, uint16_t fill, float fill_mass, uint16_t granite,
                 float temperature) {
  World world;
  world.Init(w, h, fill, fill_mass, temperature);
  for (int x = 0; x < w; ++x) {
    world.Set(x, 0, granite, 2000.0f, temperature);
    world.Set(x, h - 1, granite, 2000.0f, temperature);
  }
  for (int y = 0; y < h; ++y) {
    world.Set(0, y, granite, 2000.0f, temperature);
    world.Set(w - 1, y, granite, 2000.0f, temperature);
  }
  return world;
}

// -------------------------------------------------------------------- scenarios

// Does the sim conserve mass and energy while liquid settles and gas moves?
// This is the direct test of the "displacement deletes mass" and "heat vanishes"
// claims.
void ScenarioConservation(const Tables& t, int ticks) {
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));

  World w;
  w.Init(32, 32, oxygen, 1.0f, 293.15f);
  for (int x = 0; x < w.width; ++x) {
    for (int y = 0; y < 2; ++y) w.Set(x, y, granite, 2000.0f, 293.15f);
    // Border walls, so nothing leaves through the sides.
    for (int y = 2; y < w.height; ++y) {
      if (x == 0 || x == w.width - 1) w.Set(x, y, granite, 2000.0f, 293.15f);
    }
  }
  for (int y = w.height - 4; y < w.height - 1; ++y) {
    for (int x = 8; x < 24; ++x) w.Set(x, y, water, 1000.0f, 350.0f);
  }
  for (int x = 1; x < w.width - 1; ++x) w.Set(x, w.height - 1, granite, 2000.0f, 293.15f);

  const GameDataUpdate* g = Boot(t, w);
  if (!g) {
    printf("  boot failed\n");
    return;
  }
  std::vector<uint8_t> visible(w.Count(), 1);
  const Totals before = Measure(g, w.Count(), t);
  DumpColumn(g, w, 16, 10, "before");

  for (int i = 0; i < ticks; ++i) {
    const GameDataUpdate* n = Tick(w, &visible);
    if (!n) {
      printf("  tick %d failed\n", i);
      return;
    }
    g = n;
  }
  const Totals after = Measure(g, w.Count(), t);
  DumpColumn(g, w, 16, 10, "after");
  ReportDrift("falling water", before, after);
}

// The "one tile rule": a cell holds one element. Put far more water in a single cell
// than maxMass allows and see what the sim does with the excess.
void ScenarioOneTile(const Tables& t, int ticks) {
  const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  printf("  water maxMass from the real element table: %.1f kg\n",
         t.At(water)->maxMass);

  World w;
  w.Init(16, 16, oxygen, 1.0f, 293.15f);
  for (int x = 0; x < w.width; ++x) {
    w.Set(x, 0, granite, 2000.0f, 293.15f);
    w.Set(x, w.height - 1, granite, 2000.0f, 293.15f);
  }
  for (int y = 0; y < w.height; ++y) {
    w.Set(0, y, granite, 2000.0f, 293.15f);
    w.Set(w.width - 1, y, granite, 2000.0f, 293.15f);
  }
  // One cell, wildly over the cap.
  w.Set(8, 1, water, 20000.0f, 293.15f);

  const GameDataUpdate* g = Boot(t, w);
  if (!g) {
    printf("  boot failed\n");
    return;
  }
  std::vector<uint8_t> visible(w.Count(), 1);
  const Totals before = Measure(g, w.Count(), t);
  printf("  seeded one cell with 20000 kg water; sim reports %.1f kg there at t=0\n",
         g->mass[w.Cell(8, 1)]);

  for (int i = 0; i < ticks; ++i) {
    const GameDataUpdate* n = Tick(w, &visible);
    if (!n) {
      printf("  tick %d failed\n", i);
      return;
    }
    g = n;
  }
  const Totals after = Measure(g, w.Count(), t);
  DumpColumn(g, w, 8, 6, "after");
  ReportDrift("overfilled cell", before, after);
}

// A single tile of liquid sealing a gap is what makes liquid locks work. Seal a
// one-tile hole with water, put high-pressure gas on one side, and see whether any
// crosses.
void ScenarioLiquidLock(const Tables& t, int ticks) {
  uint16_t water, oxygen, co2, granite;
  if (!Resolve(t, kWater, "Water", &water) ||
      !Resolve(t, kOxygen, "Oxygen", &oxygen) ||
      !Resolve(t, kCarbonDioxide, "CarbonDioxide", &co2) ||
      !Resolve(t, kGranite, "Granite", &granite)) {
    return;
  }
  printf("  indices: water=%u oxygen=%u co2=%u granite=%u\n", water, oxygen, co2,
         granite);

  World w;
  w.Init(16, 9, oxygen, 0.1f, 293.15f);
  // Solid everywhere except two chambers joined by a single cell at (8,4).
  for (size_t i = 0; i < w.Count(); ++i) {
    w.element[i] = granite;
    w.mass[i] = 2000.0f;
    w.temperature[i] = 293.15f;
  }
  for (int y = 1; y < 8; ++y) {
    for (int x = 1; x < 8; ++x) w.Set(x, y, co2, 20.0f, 293.15f);   // pressurised
    for (int x = 9; x < 15; ++x) w.Set(x, y, oxygen, 0.1f, 293.15f);  // near vacuum
  }
  w.Set(8, 4, water, 1000.0f, 293.15f);  // the plug

  const GameDataUpdate* g = Boot(t, w);
  if (!g) {
    printf("  boot failed\n");
    return;
  }
  std::vector<uint8_t> visible(w.Count(), 1);

  // Gas mass in a chamber, whatever element it is: reporting only CO2 hides the case
  // where CO2 crossed and then mixed or was displaced.
  auto chamber = [&](const GameDataUpdate* u, int x0, int x1, uint16_t want) {
    double total = 0, of_element = 0;
    for (int y = 1; y < 8; ++y) {
      for (int x = x0; x < x1; ++x) {
        const int c = w.Cell(x, y);
        if (t.Phase(u->elementIdx[c]) == 3) continue;  // skip solids
        total += u->mass[c];
        if (u->elementIdx[c] == want) of_element += u->mass[c];
      }
    }
    printf("    x=%d..%d  total gas/liquid %.4f kg, of which CO2 %.4f kg\n", x0, x1 - 1,
           total, of_element);
  };

  const Totals before = Measure(g, w.Count(), t);
  printf("  t=0\n");
  chamber(g, 1, 8, co2);
  chamber(g, 9, 15, co2);
  for (int i = 0; i < ticks; ++i) {
    const GameDataUpdate* n = Tick(w, &visible);
    if (!n) {
      printf("  tick %d failed\n", i);
      return;
    }
    g = n;
  }
  printf("  after %d ticks\n", ticks);
  chamber(g, 1, 8, co2);
  chamber(g, 9, 15, co2);
  printf("  plug cell (8,4): element %u, %.4f kg  (water=%u)\n",
         g->elementIdx[w.Cell(8, 4)], g->mass[w.Cell(8, 4)], water);
  ReportDrift("liquid lock", before, Measure(g, w.Count(), t));
}

// A gas pump and a vent: an element consumer pulling gas out of one corner and an
// element emitter pushing it back in at the other. These are the real building
// components, registered the way the game registers them, so this exercises the
// consumer/emitter paths that the sealed scenarios never touched.
void ScenarioPump(const Tables& t, int ticks) {
  uint16_t oxygen, granite;
  if (!Resolve(t, kOxygen, "Oxygen", &oxygen) ||
      !Resolve(t, kGranite, "Granite", &granite)) {
    return;
  }

  World w = SealedRoom(24, 12, oxygen, 1.5f, granite, 293.15f);
  const int intake = w.Cell(3, 3);
  const int outlet = w.Cell(20, 8);

  const GameDataUpdate* g = Boot(t, w);
  if (!g) {
    printf("  boot failed\n");
    return;
  }
  std::vector<uint8_t> visible(w.Count(), 1);
  const Totals before = Measure(g, w.Count(), t);

  constexpr int kConsumerCb = 101;
  constexpr int kEmitterCb = 102;
  AddElementConsumer(intake, 2 /* AllGas */, oxygen, 1, kConsumerCb);
  AddElementEmitter(2.0f /* max pressure, kg */, kEmitterCb);

  HandleTable handles;
  Ledger ledger;

  // Registration handles only come back on a tick, so the pump cannot be configured
  // until one has been run.
  g = Tick(w, &visible);
  if (!g) {
    printf("  registration tick failed\n");
    return;
  }
  handles.Absorb(g);
  Account(g, &ledger);

  const int32_t consumer = handles.Find(kConsumerCb);
  const int32_t emitter = handles.Find(kEmitterCb);
  printf("  handles: consumer=%d emitter=%d\n", consumer, emitter);
  if (consumer < 0 || emitter < 0) {
    printf("  FAILED: the sim did not report a handle for one of the components\n");
    return;
  }
  SetElementConsumerData(consumer, intake, 0.5f /* kg/s */);
  ModifyElementEmitter(emitter, outlet, oxygen, 0.2f /* interval s */, 0.1f /* kg */,
                       310.0f, 2.0f /* max pressure */);

  // Buildings that take a fixed bite rather than running a consumer component (the
  // electrolyzer, for one) go through ConsumeMass instead, so exercise that too.
  const int direct = w.Cell(12, 6);
  for (int i = 0; i < ticks; ++i) {
    if (i % 10 == 0) ConsumeMass(direct, oxygen, 0.2f, 1, 500 + i);
    g = Tick(w, &visible);
    if (!g) {
      printf("  tick %d failed\n", i);
      return;
    }
    Account(g, &ledger);
  }
  const Totals after = Measure(g, w.Count(), t);
  printf("  intake cell %.4f kg, outlet cell %.4f kg\n", g->mass[intake], g->mass[outlet]);
  ReportBalance("pump + vent", before, after, ledger);
  ReportDrift("pump (raw grid)", before, after);
}

// Over-pressure. Emit gas into a one-cell sealed pocket far past anything the sim
// would normally hold, and see whether the excess is refused or silently absorbed.
// This is the closest reachable probe at the DoPressureBreak path without a full
// building set, and the `suceeded` flag on the emission callback distinguishes
// "refused" from "deleted".
void ScenarioOverpressure(const Tables& t, int ticks) {
  uint16_t oxygen, granite;
  if (!Resolve(t, kOxygen, "Oxygen", &oxygen) ||
      !Resolve(t, kGranite, "Granite", &granite)) {
    return;
  }
  printf("  oxygen maxMass %.3f kg\n", t.At(oxygen)->maxMass);

  // A single open cell, granite on all sides.
  World w;
  w.Init(5, 5, granite, 2000.0f, 293.15f);
  const int pocket = w.Cell(2, 2);
  w.Set(2, 2, oxygen, 1.0f, 293.15f);

  const GameDataUpdate* g = Boot(t, w);
  if (!g) {
    printf("  boot failed\n");
    return;
  }
  std::vector<uint8_t> visible(w.Count(), 1);
  const Totals before = Measure(g, w.Count(), t);

  Ledger ledger;
  double requested = 0;
  for (int i = 0; i < ticks; ++i) {
    EmitMass(pocket, oxygen, 50.0f, 293.15f, 200 + i);
    requested += 50.0;
    g = Tick(w, &visible);
    if (!g) {
      printf("  tick %d failed\n", i);
      return;
    }
    Account(g, &ledger);
  }
  const Totals after = Measure(g, w.Count(), t);
  printf("  requested %.1f kg in %d emissions: %d accepted, %d refused, %.3f kg added\n",
         requested, ticks, ledger.emit_ok, ledger.emit_refused, ledger.in_emitted);
  printf("  pocket cell now %.3f kg\n", g->mass[pocket]);
  ReportBalance("overpressure", before, after, ledger);
}

// Solid into liquid, along two routes: an unsupported sand tile that the sim declares
// unstable and hands to the game, and a direct solid emission into a cell that is
// already full of water. Both are displacement paths that the sealed scenarios never
// reached.
void ScenarioSolidDrop(const Tables& t, int ticks) {
  uint16_t water, oxygen, granite, sand;
  if (!Resolve(t, kWater, "Water", &water) ||
      !Resolve(t, kOxygen, "Oxygen", &oxygen) ||
      !Resolve(t, kGranite, "Granite", &granite) ||
      !Resolve(t, kSand, "Sand", &sand)) {
    return;
  }

  printf("  sand state 0x%02x (unstable=%d), granite state 0x%02x (unstable=%d)\n",
         t.State(sand), t.IsUnstable(sand), t.State(granite), t.IsUnstable(granite));

  World w = SealedRoom(20, 16, oxygen, 1.0f, granite, 293.15f);
  for (int y = 1; y <= 4; ++y) {
    for (int x = 1; x < 19; ++x) w.Set(x, y, water, 900.0f, 293.15f);
  }
  // Sand resting on a granite ledge over the pool. The sim does not re-check
  // stability spontaneously — a floating sand tile seeded at worldgen just sits there
  // — so the ledge is dug out mid-run to make the sand actually fall.
  for (int x = 6; x < 10; ++x) {
    w.Set(x, 9, granite, 1200.0f, 293.15f);
    w.Set(x, 10, sand, 1500.0f, 293.15f);
  }

  const GameDataUpdate* g = Boot(t, w);
  if (!g) {
    printf("  boot failed\n");
    return;
  }
  std::vector<uint8_t> visible(w.Count(), 1);
  const Totals before = Measure(g, w.Count(), t);

  Ledger ledger;
  // Three separate routes into an already-settled pool, a third of the way in.
  const int drop_tick = ticks / 3;
  const int emit_target = w.Cell(14, 3);
  const int land_target = w.Cell(11, 3);
  const int build_target = w.Cell(4, 3);
  double displaced_water_before = 0;
  for (int i = 0; i < ticks; ++i) {
    if (i == drop_tick) {
      displaced_water_before = static_cast<double>(g->mass[land_target]) +
                               g->mass[build_target];
      printf("  tick %d: pool cells hold (14,3) %.3f, (11,3) %.3f, (4,3) %.3f kg\n", i,
             g->mass[emit_target], g->mass[land_target], g->mass[build_target]);
      // 1. EmitMass, the route a vent or a dupe's breath takes.
      EmitMass(emit_target, sand, 500.0f, 293.15f, 300);
      // 2. ModifyCell with vertical solid displacement: sand landing on the pool,
      //    which is exactly what UnstableGroundManager sends when a puff lands.
      ModifyCell(land_target, sand, 293.15f, 500.0f, 0 /* None */, true, 301);
      // 3. ReplaceAndDisplace: a solid building placed into liquid.
      ModifyCell(build_target, granite, 293.15f, 800.0f, 2 /* ReplaceAndDisplace */,
                 false, 302);
      // And pull the ledge out from under the sand shelf.
      for (int x = 6; x < 10; ++x) Dig(w.Cell(x, 9), -1);
    }
    g = Tick(w, &visible);
    if (!g) {
      printf("  tick %d failed\n", i);
      return;
    }
    Account(g, &ledger);
  }
  const Totals after = Measure(g, w.Count(), t);
  printf("  emit  (14,3) after: element %u, %.3f kg\n", g->elementIdx[emit_target],
         g->mass[emit_target]);
  printf("  land  (11,3) after: element %u, %.3f kg\n", g->elementIdx[land_target],
         g->mass[land_target]);
  printf("  build (4,3)  after: element %u, %.3f kg\n", g->elementIdx[build_target],
         g->mass[build_target]);
  printf("  the two displaced cells held %.3f kg of water before the drop\n",
         displaced_water_before);
  printf("  emissions: %d accepted, %d refused\n", ledger.emit_ok, ledger.emit_refused);
  printf("  1300 kg of solid was inserted by ModifyCell, which the ledger cannot see:"
         " expect the residual to be about +1300 kg, not 0\n");
  ReportBalance("solid into liquid", before, after, ledger);
}

// The crux of the "displacement deletes mass" claim. Fill a sealed box completely
// with liquid at capacity so there is nowhere for anything to go, then place a solid
// building into the middle of it with ReplaceAndDisplace. If the sim is going to
// destroy mass anywhere, it is here.
void ScenarioDisplace(const Tables& t, int ticks) {
  uint16_t water, granite;
  if (!Resolve(t, kWater, "Water", &water) ||
      !Resolve(t, kGranite, "Granite", &granite)) {
    return;
  }

  // No gas headroom at all: every interior cell is water at its maximum.
  const float max_mass = t.At(water)->maxMass;
  World w = SealedRoom(12, 12, water, max_mass, granite, 293.15f);

  const GameDataUpdate* g = Boot(t, w);
  if (!g) {
    printf("  boot failed\n");
    return;
  }
  std::vector<uint8_t> visible(w.Count(), 1);

  // Let it settle first, so the "full" state is the sim's own idea of full rather
  // than the one that was handed to it.
  Ledger ledger;
  for (int i = 0; i < ticks / 2; ++i) {
    g = Tick(w, &visible);
    if (!g) return;
    Account(g, &ledger);
  }
  const Totals before = Measure(g, w.Count(), t);
  const int target = w.Cell(6, 6);
  printf("  sealed box full of water at %.1f kg/cell; grid holds %.3f kg\n", max_mass,
         before.mass);
  printf("  placing 800 kg granite into (6,6), which holds %.3f kg of water\n",
         g->mass[target]);
  ModifyCell(target, granite, 293.15f, 800.0f, 2 /* ReplaceAndDisplace */, false, 600);

  for (int i = 0; i < ticks; ++i) {
    g = Tick(w, &visible);
    if (!g) return;
    Account(g, &ledger);
  }
  const Totals after = Measure(g, w.Count(), t);
  printf("  target cell now element %u, %.3f kg\n", g->elementIdx[target],
         g->mass[target]);
  printf("  inserted 800 kg by ModifyCell, which the ledger cannot see:"
         " a conserving sim leaves residual +800\n");
  ReportBalance("displace, no headroom", before, after, ledger);
}

// Digging removes a solid cell and hands its mass to the game as ore. If the sim
// spawns less than it removed, that is real mass deletion on an extremely common
// player action.
void ScenarioDig(const Tables& t, int ticks) {
  uint16_t oxygen, granite;
  if (!Resolve(t, kOxygen, "Oxygen", &oxygen) ||
      !Resolve(t, kGranite, "Granite", &granite)) {
    return;
  }

  World w = SealedRoom(20, 20, oxygen, 1.0f, granite, 293.15f);
  for (int y = 1; y < 10; ++y) {
    for (int x = 1; x < 19; ++x) w.Set(x, y, granite, 1200.0f, 293.15f);
  }

  const GameDataUpdate* g = Boot(t, w);
  if (!g) {
    printf("  boot failed\n");
    return;
  }
  std::vector<uint8_t> visible(w.Count(), 1);
  const Totals before = Measure(g, w.Count(), t);

  double dug_grid_mass = 0;
  for (int y = 5; y < 9; ++y) {
    for (int x = 5; x < 15; ++x) {
      dug_grid_mass += g->mass[w.Cell(x, y)];
      Dig(w.Cell(x, y), -1);
    }
  }
  printf("  digging 40 cells holding %.3f kg\n", dug_grid_mass);

  Ledger ledger;
  for (int i = 0; i < ticks; ++i) {
    g = Tick(w, &visible);
    if (!g) {
      printf("  tick %d failed\n", i);
      return;
    }
    Account(g, &ledger);
  }
  const Totals after = Measure(g, w.Count(), t);
  printf("  ore handed to the game: %.3f kg (dug cells held %.3f kg)\n", ledger.out_ore,
         dug_grid_mass);
  ReportBalance("dig", before, after, ledger);
}

// A powered building. AddBuildingHeatExchange with operatingKilowatts is the sim's
// only heat *source*, so this is a quantitative check rather than a conservation one:
// after N ticks the world should hold exactly operating_kw * elapsed extra kilojoules.
void ScenarioBuildingHeat(const Tables& t, int ticks) {
  uint16_t vacuum, granite;
  if (!Resolve(t, kVacuum, "Vacuum", &vacuum) ||
      !Resolve(t, kGranite, "Granite", &granite)) {
    return;
  }

  // Vacuum around the building, so nothing conducts away and the only thing moving
  // energy is the building's own operating wattage. Conduction is already covered by
  // the conservation scenario; what is under test here is the heat *source*.
  World w = SealedRoom(24, 16, vacuum, 0.0f, granite, 293.15f);

  const GameDataUpdate* g = Boot(t, w);
  if (!g) {
    printf("  boot failed\n");
    return;
  }
  std::vector<uint8_t> visible(w.Count(), 1);
  const Totals before = Measure(g, w.Count(), t);

  constexpr int kBuildingCb = 401;
  constexpr float kBuildingMass = 400.0f;
  constexpr float kOperatingKw = 1.0f;
  const float building_shc = t.SpecificHeat(granite);
  AddBuildingHeatExchange(8, 4, 10, 6, granite, kBuildingMass, 293.15f, 10.0f,
                          kOperatingKw, kBuildingCb);

  HandleTable handles;
  Ledger ledger;
  float building_temp = 293.15f;
  const double building_energy_before =
      static_cast<double>(kBuildingMass) * building_shc * building_temp;
  // One PrepareGameData does not have to mean one simulated frame; the sim reports
  // what it actually ran, and the difference is the whole explanation for any drift
  // between requested and delivered kilowatts.
  long long frames = 0;

  for (int i = 0; i < ticks; ++i) {
    g = Tick(w, &visible);
    if (!g) {
      printf("  tick %d failed\n", i);
      return;
    }
    frames += g->numFramesProcessed;
    handles.Absorb(g);
    Account(g, &ledger);
    const int32_t handle = handles.Find(kBuildingCb);
    for (int k = 0; k < g->numBuildingTemperatures; ++k) {
      if (g->buildingTemperatures[k].handle == handle) {
        building_temp = g->buildingTemperatures[k].temperature;
      }
    }
  }
  const Totals after = Measure(g, w.Count(), t);
  const double building_energy_after =
      static_cast<double>(kBuildingMass) * building_shc * building_temp;
  const double measured = (after.energy + building_energy_after) -
                          (before.energy + building_energy_before);
  const double expected = static_cast<double>(kOperatingKw) * 0.2 * ticks;

  printf("  building handle %d, temperature %.3f -> %.3f K (heat capacity %.3f kJ/K)\n",
         handles.Find(kBuildingCb), 293.15, building_temp, kBuildingMass * building_shc);
  printf("  grid energy   %14.1f -> %14.1f kJ\n", before.energy, after.energy);
  printf("  building      %14.1f -> %14.1f kJ\n", building_energy_before,
         building_energy_after);
  printf("  total energy added %.1f kJ, expected %.1f kJ from %.1f kW over %.1f s"
         "  (%+.3f%%)\n", measured, expected, kOperatingKw, 0.2 * ticks,
         expected > 0 ? 100.0 * (measured - expected) / expected : 0.0);
  printf("  %d PrepareGameData calls, %lld frames actually simulated"
         " (%.1f s of sim time)\n", ticks, frames, 0.2 * frames);
  ReportBalance("building heat", before, after, ledger);
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  const char* dll = "SimDLL_orig.dll";
  const char* corpus = nullptr;
  const char* scenario = "all";
  int ticks = 200;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--dll") && i + 1 < argc) dll = argv[++i];
    else if (!strcmp(argv[i], "--corpus") && i + 1 < argc) corpus = argv[++i];
    else if (!strcmp(argv[i], "--scenario") && i + 1 < argc) scenario = argv[++i];
    else if (!strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = atoi(argv[++i]);
  }
  if (!corpus) {
    printf("usage: experiments.exe --corpus <corpus.bin> [--dll <p>]"
           " [--ticks N]\n"
           "       --scenario conservation|onetile|liquidlock|pump|overpressure"
           "|soliddrop|displace|dig|buildingheat|all\n");
    return 2;
  }

  Tables tables;
  if (!LoadTables(corpus, &tables)) return 1;
  printf("element table: %d elements\n", tables.count);

  struct Entry {
    const char* name;
    void (*fn)(const Tables&, int);
  };
  const Entry entries[] = {
      {"conservation", &ScenarioConservation},
      {"onetile", &ScenarioOneTile},
      {"liquidlock", &ScenarioLiquidLock},
      {"pump", &ScenarioPump},
      {"overpressure", &ScenarioOverpressure},
      {"soliddrop", &ScenarioSolidDrop},
      {"displace", &ScenarioDisplace},
      {"dig", &ScenarioDig},
      {"buildingheat", &ScenarioBuildingHeat},
  };

  for (const Entry& e : entries) {
    if (strcmp(scenario, "all") && strcmp(scenario, e.name)) continue;
    printf("\n=== %s (%d ticks) ===\n", e.name, ticks);
    // Each scenario gets a fresh sim: SIM_Initialize/SIM_Shutdown cycle repeatedly
    // within one process in the real game too, so this is a supported pattern.
    if (!Bind(dll)) return 1;
    sim_initialize(&DefaultMessageHandler);
    e.fn(tables, ticks);
    sim_shutdown();
  }
  printf("\ndone\n");
  return 0;
}
