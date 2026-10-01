// A fresh cluster boot, and what `Load` does to every cell it reads, checked against whichever
// SimDLL `--dll` names. Every check here was measured against Klei's DLL first and must pass on
// it unchanged; run it on both.
//
// `SaveLoader.LoadFromWorldGen` allocates the whole cluster, defines the worlds, clears, then
// sends one `Load` per world with a blob sized to that world and saved at its offset, then
// `Start`. `WorldGenSimUtil.DoSettleSim` stamps templates before that save with
// `ModifyBackwallData` carrying index 0xFFFF for every template cell with no backwall set.
//
// Usage: worldgen_test.exe --corpus <corpus.bin> --dll <SimDLL.dll>
//        WORLDGEN_DUMP=1 (WSLENV=WORLDGEN_DUMP from WSL) prints the published grid.

#include <cmath>
#include <cstdlib>

#include "simhost.h"
#include "harness.h"

using namespace simhost;

namespace {

constexpr int32_t kNeutronium = 1838482828;

bool g_failed = false;
void Check(bool cond, const char* what) {
  printf("  [%s] %s\n", cond ? "OK" : "FAIL", what);
  if (!cond) g_failed = true;
}

// oni_bench::WorldPayload sends backwall index 0 everywhere, and index 0 is a real element, so a
// painted backwall is needed to tell a copied backwall from an untouched one.
std::vector<uint8_t> WorldPayloadWithBackwall(const oni_bench::SeedWorld& w,
                                               uint16_t backwall_element) {
  std::vector<uint8_t> b;
  auto put = [&](const void* p, size_t n) {
    const uint8_t* s = static_cast<const uint8_t*>(p);
    b.insert(b.end(), s, s + n);
  };
  const int32_t width = w.width, height = w.height;
  const uint32_t seed = 12345u;
  const uint8_t radiation = 0, headless = 1;
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
    c.insulation = 255;
    put(&c, sizeof(Cell));
  }
  for (size_t i = 0; i < w.Count(); ++i) {
    DiseaseCell d{};
    d.diseaseIdx = 0xFF;
    put(&d, sizeof(DiseaseCell));
  }
  for (size_t i = 0; i < w.Count(); ++i) {
    SimBackwall s{};
    s.elementIdx = backwall_element;
    s.mass = 1.0f;
    s.temperature = 300.0f;
    put(&s, sizeof(SimBackwall));
  }
  return b;
}

// Klei's frame runs on a worker thread and `PrepareGameData` publishes whatever has finished,
// so each tick waits before the next. `Start`'s own backwall publish is a 0xFFFF placeholder;
// backwall is only real from the first frame on.
const GameDataUpdate* Ticks(int n, int32_t width, int32_t height, std::vector<uint8_t>* visible) {
  const GameDataUpdate* u = nullptr;
  for (int i = 0; i < n; ++i) {
    NewGameFrame f{};
    f.elapsedSeconds = 0.2f;
    f.maxX = width;
    f.maxY = height - 1;
    sim_handle_message(static_cast<int32_t>(SimMessageHash::SimFrameManager_NewGameFrame),
                       sizeof(NewGameFrame), reinterpret_cast<const uint8_t*>(&f));
    u = static_cast<const GameDataUpdate*>(
        sim_handle_message(static_cast<int32_t>(SimMessageHash::PrepareGameData),
                           static_cast<int>(visible->size()), visible->data()));
    Sleep(60);
  }
  return u;
}

std::vector<uint8_t> WorldBlob(const Tables& t, uint16_t element, float mass, float temperature,
                               uint16_t backwall, int32_t x, int32_t y) {
  sim_initialize(&DefaultMessageHandler);
  SendRaw(SimMessageHash::Elements_CreateTable, t.elements);
  SendRaw(SimMessageHash::Disease_CreateTable, t.diseases);
  oni_bench::SeedWorld w;
  w.Init(5, 3, element, mass, temperature);
  SendRaw(SimMessageHash::SimData_InitializeFromCells, WorldPayloadWithBackwall(w, backwall));
  SendEmpty(SimMessageHash::Start);
  std::vector<uint8_t> blob = Save(x, y);
  sim_shutdown();
  return blob;
}

// A plain saved game of one element, `w` x `h` game cells, saved at (0,0).
std::vector<uint8_t> WorldBlob2(const Tables& t, uint16_t element, float mass, float temperature,
                                int32_t w, int32_t h) {
  sim_initialize(&DefaultMessageHandler);
  SendRaw(SimMessageHash::Elements_CreateTable, t.elements);
  SendRaw(SimMessageHash::Disease_CreateTable, t.diseases);
  oni_bench::SeedWorld world;
  world.Init(w, h, element, mass, temperature);
  SendRaw(SimMessageHash::SimData_InitializeFromCells, WorldPayloadWithBackwall(world, element));
  SendEmpty(SimMessageHash::Start);
  std::vector<uint8_t> blob = Save(0, 0);
  sim_shutdown();
  return blob;
}

struct Snap {
  uint16_t element;
  float mass, temperature;
};

// --- What `Load` does to each cell it reads (Klei's `CellRead` and the loop in
// `Load`, ending in `DoLoadTimeStateTransition`).

constexpr int32_t kLoadW = 8, kLoadH = 4;  // game cells; the blob is 10 x 6 padded

void PutCell(std::vector<uint8_t>* blob, int32_t gx, int32_t gy, int32_t hash, float temperature,
             float mass, float radiation = 0.0f) {
  const size_t pi = static_cast<size_t>(gy + 1) * (kLoadW + 2) + static_cast<size_t>(gx + 1);
  const size_t at = 29 + pi * 16;
  memcpy(blob->data() + at, &hash, 4);
  memcpy(blob->data() + at + 4, &temperature, 4);
  memcpy(blob->data() + at + 8, &mass, 4);
  memcpy(blob->data() + at + 12, &radiation, 4);
}

struct LoadedCell {
  uint16_t element;
  float mass, temperature, radiation;
};

// Loads `blob` into a fresh sim and reads the `Start` publication, before any frame could have
// run a transition of its own. `cluster` puts it down as one world of a larger allocation.
std::vector<LoadedCell> LoadAndRead(const Tables& t, const std::vector<uint8_t>& blob,
                                    bool cluster) {
  constexpr int32_t kOffX = 3, kOffY = 2, kCW = 16, kCH = 10;
  const int32_t cw = cluster ? kCW : kLoadW, ch = cluster ? kCH : kLoadH;
  std::vector<uint8_t> b = blob;
  if (cluster) {
    memcpy(b.data() + 20, &kOffX, 4);
    memcpy(b.data() + 24, &kOffY, 4);
  }
  sim_initialize(&DefaultMessageHandler);
  SendRaw(SimMessageHash::Elements_CreateTable, t.elements);
  {
    Writer a;
    a.Put<int32_t>(cw);
    a.Put<int32_t>(ch);
    Send(SimMessageHash::AllocateCells, a);
  }
  SendRaw(SimMessageHash::Disease_CreateTable, t.diseases);
  if (cluster) {
    Writer o;
    for (int32_t v : {1, kOffX, kOffY, kLoadW, kLoadH}) o.Put<int32_t>(v);
    Send(SimMessageHash::DefineWorldOffsets, o);
  }
  SendEmpty(SimMessageHash::ClearUnoccupiedCells);
  std::vector<LoadedCell> out;
  if (!SendRaw(SimMessageHash::Load, b)) {
    sim_shutdown();
    return out;
  }
  const GameDataUpdate* u = static_cast<const GameDataUpdate*>(SendEmpty(SimMessageHash::Start));
  if (u) {
    // The padded blob goes down unshifted at its header, so game cell (x,y) of the world is
    // cluster cell (kOffX + x, kOffY + y).
    for (int32_t y = 0; y < kLoadH; ++y) {
      for (int32_t x = 0; x < kLoadW; ++x) {
        const int32_t c = cluster ? (kOffY + y) * cw + (kOffX + x) : y * cw + x;
        out.push_back({u->elementIdx[c], u->mass[c], u->temperature[c],
                       u->radiation ? u->radiation[c] : 0.0f});
      }
    }
  }
  sim_shutdown();
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char* dll = nullptr;
  const char* corpus = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--dll") && i + 1 < argc) dll = argv[++i];
    else if (!strcmp(argv[i], "--corpus") && i + 1 < argc) corpus = argv[++i];
  }
  if (!dll || !corpus) {
    printf("usage: worldgen_test.exe --corpus <corpus.bin> --dll <SimDLL.dll>\n");
    return 2;
  }

  Tables t;
  if (!LoadTables(corpus, &t)) return 1;
  const uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  const uint16_t co2 = static_cast<uint16_t>(t.IndexOf(kCarbonDioxide));
  const uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  const uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const uint16_t neutronium = static_cast<uint16_t>(t.IndexOf(kNeutronium));
  if (!Bind(dll)) return 1;

  constexpr int32_t kClusterW = 20, kClusterH = 10;
  constexpr int32_t kAX = 2, kAY = 2;    // world A's 5x3 at game (2,2)-(6,4)
  constexpr int32_t kBX = 12, kBY = 2;   // world B's 5x3 at game (12,2)-(16,4)
  auto at = [&](int32_t x, int32_t y) { return y * kClusterW + x; };

  const std::vector<uint8_t> blobA = WorldBlob(t, oxygen, 2.0f, 320.0f, granite, kAX, kAY);
  const std::vector<uint8_t> blobB = WorldBlob(t, co2, 3.0f, 310.0f, granite, kBX, kBY);

  sim_initialize(&DefaultMessageHandler);
  SendRaw(SimMessageHash::Elements_CreateTable, t.elements);
  SendRaw(SimMessageHash::Disease_CreateTable, t.diseases);
  {
    Writer alloc;
    alloc.Put<int32_t>(kClusterW);
    alloc.Put<int32_t>(kClusterH);
    Send(SimMessageHash::AllocateCells, alloc);
  }
  {
    // Listed B first while A loads first: a blob goes down at its own header, so the order of
    // this list must not matter.
    Writer offsets;
    offsets.Put<int32_t>(2);
    for (int32_t v : {kBX, kBY, 5, 3, kAX, kAY, 5, 3}) offsets.Put<int32_t>(v);
    Send(SimMessageHash::DefineWorldOffsets, offsets);
  }
  SendEmpty(SimMessageHash::ClearUnoccupiedCells);
  Check(SendRaw(SimMessageHash::Load, blobA) != nullptr, "Load accepts world A");
  Check(SendRaw(SimMessageHash::Load, blobB) != nullptr, "Load accepts world B");

  // --- Start: element, mass and temperature are real here, and the game reads them at once.
  const GameDataUpdate* start =
      static_cast<const GameDataUpdate*>(SendEmpty(SimMessageHash::Start));
  Check(start != nullptr, "Start publishes an update");
  if (!start) return 1;
  bool in_range = true;
  for (int32_t i = 0; i < kClusterW * kClusterH; ++i) {
    in_range = in_range && start->elementIdx[i] < static_cast<uint16_t>(t.count);
  }
  auto snap = [&](int32_t x, int32_t y) {
    const int32_t c = at(x, y);
    return Snap{start->elementIdx[c], start->mass[c], start->temperature[c]};
  };
  const Snap a = snap(kAX, kAY), b = snap(kBX, kBY), gap = snap(9, 5);
  const Snap ring_left = snap(kAX - 1, kAY), ring_top = snap(kAX + 2, kAY + 3);
  Check(in_range, "every cell of the cluster is a valid element index (Grid.InitializeCells)");
  Check(a.element == oxygen && std::fabs(a.mass - 2.0f) < 1e-3f && a.temperature == 320.0f,
        "world A lands at its own offset");
  Check(b.element == co2 && std::fabs(b.mass - 3.0f) < 1e-3f && b.temperature == 310.0f,
        "world B lands at its own offset");
  Check(gap.element == vacuum && gap.mass == 0.0f && gap.temperature == 0.0f,
        "a cell between the worlds is Vacuum, 0 kg, 0 K");
  Check(ring_left.element == neutronium && ring_left.temperature == 293.0f,
        "a world's Neutronium border ring loads at 293 K, not the blob's 0 K");
  Check(ring_top.element == vacuum && ring_top.mass == 0.0f,
        "a world's top border ring loads as Vacuum with no mass, not the blob's 9999 kg");

  // --- After frames: backwall.
  std::vector<uint8_t> visible(static_cast<size_t>(kClusterW) * kClusterH, 1);
  const GameDataUpdate* u = Ticks(4, kClusterW, kClusterH, &visible);
  Check(u != nullptr, "frames publish");
  if (!u) return 1;
  Check(u->backwallElement[at(kAX, kAY)] == granite && u->backwallMass[at(kAX, kAY)] == 1.0f,
        "world A keeps its painted backwall");
  Check(u->backwallElement[at(kBX, kBY)] == granite && u->backwallMass[at(kBX, kBY)] == 1.0f,
        "world B keeps its painted backwall");
  Check(u->backwallElement[at(9, 5)] == vacuum && u->backwallMass[at(9, 5)] == 0.0f &&
            u->backwallTemperature[at(9, 5)] == 0.0f,
        "a cell between the worlds has a Vacuum backwall");

  // --- A template cell's unset backwall: index 0xFFFF is "no element", not element 0.
  constexpr int32_t kTX = 4, kTY = 3;
  SetBackwallDataMsg m{};
  m.gameCell = at(kTX, kTY);
  m.elementIdx = 0xFFFF;
  sim_handle_message(static_cast<int32_t>(SimMessageHash::ModifyBackwallData), sizeof(m),
                     reinterpret_cast<const uint8_t*>(&m));
  u = Ticks(4, kClusterW, kClusterH, &visible);
  Check(u->backwallElement[at(kTX, kTY)] == 0xFFFF && u->backwallMass[at(kTX, kTY)] == 0.0f,
        "ModifyBackwallData with index 0xFFFF publishes 0xFFFF, not element 0 (Crushed Ice)");

  if (getenv("WORLDGEN_DUMP")) {
    auto glyph = [&](uint16_t e) {
      if (e == oxygen) return 'O';
      if (e == co2) return 'C';
      if (e == vacuum) return '.';
      if (e == granite) return 'G';
      if (e == neutronium) return 'N';
      if (e == 0) return '0';
      if (e == 0xFFFF) return '-';
      return '#';
    };
    printf("  element / backwall, y=%d at the top:\n", kClusterH - 1);
    for (int32_t y = kClusterH - 1; y >= 0; --y) {
      printf("    ");
      for (int32_t x = 0; x < kClusterW; ++x) putchar(glyph(u->elementIdx[at(x, y)]));
      printf("   ");
      for (int32_t x = 0; x < kClusterW; ++x) putchar(glyph(u->backwallElement[at(x, y)]));
      putchar('\n');
    }
  }

  const std::vector<uint8_t> saved = Save(0, 0);
  const size_t pw = kClusterW + 2, n = pw * (kClusterH + 2);
  const size_t pi = static_cast<size_t>(kTY + 1) * pw + (kTX + 1);
  int32_t saved_hash = 0;
  if (saved.size() == 29 + n * 36) memcpy(&saved_hash, saved.data() + 29 + n * 24 + pi * 12, 4);
  Check(saved_hash == kVacuum, "and saves it as Vacuum");

  sim_shutdown();

  // --- Every cell `Load` reads, on the saved-game path and the cluster path alike.
  {
    const Element& eg = *t.At(granite);
    const uint16_t water = static_cast<uint16_t>(t.IndexOf(kWater));
    const uint16_t ice = static_cast<uint16_t>(t.IndexOf(kIce));
    const uint16_t magma = static_cast<uint16_t>(t.IndexOf(oni_bench::kMagma));
    constexpr int32_t kVoid = -1456075980;
    // `CellRead` renames two element hashes before it looks them up.
    constexpr int32_t kLegacyFrom = 0x0280cf79, kLegacyTo = static_cast<int32_t>(0x987dac06);
    const int32_t void_idx = t.IndexOf(kVoid), legacy_idx = t.IndexOf(kLegacyTo);
    // An element whose high transition drops an ore in a frame: `Load` drops nothing.
    int32_t ore_elem = -1;
    for (int32_t i = 0; i < t.count && ore_elem < 0; ++i) {
      const Element& e = *t.At(i);
      if (e.highTempTransitionIdx != 0xFFFF && e.highTempTransitionOreID != 0 &&
          e.highTempTransitionOreMassConversion > 0.0f &&
          e.highTempTransitionOreMassConversion < 1.0f && e.highTemp + 50.0f < 9000.0f) {
        ore_elem = i;
      }
    }
    const float nan = std::nanf(""), inf = HUGE_VALF;
    const float at_margin = eg.highTemp + 3.0f;
    const float past_margin = eg.highTemp + 3.05f;
    const float ore_t = ore_elem >= 0 ? t.At(ore_elem)->highTemp + 50.0f : 300.0f;

    std::vector<uint8_t> blob =
        WorldBlob2(t, granite, 100.0f, 300.0f, kLoadW, kLoadH);
    PutCell(&blob, 0, 0, kGranite, 5000.0f, 100.0f);
    PutCell(&blob, 1, 0, kGranite, 5000.0f, 0.0f);
    PutCell(&blob, 2, 0, kWater, 200.0f, 100.0f);
    PutCell(&blob, 3, 0, kWater, 0.0f, 100.0f);
    PutCell(&blob, 4, 0, kGranite, at_margin, 100.0f);
    PutCell(&blob, 5, 0, kGranite, past_margin, 100.0f);
    if (ore_elem >= 0) PutCell(&blob, 6, 0, t.At(ore_elem)->id, ore_t, 100.0f);
    PutCell(&blob, 7, 0, kWater, 1.0f, 100.0f);
    if (void_idx >= 0) PutCell(&blob, 0, 1, kVoid, 300.0f, 50.0f, 9.0f);
    PutCell(&blob, 1, 1, kGranite, nan, 100.0f);
    PutCell(&blob, 2, 1, kGranite, inf, 100.0f);
    PutCell(&blob, 3, 1, kGranite, 300.0f, nan);
    PutCell(&blob, 4, 1, 12345, 300.0f, 50.0f, 9.0f);
    PutCell(&blob, 5, 1, kLegacyFrom, 300.0f, 50.0f);
    PutCell(&blob, 6, 1, kGranite, 300.0f, 100.0f, -5.0f);
    PutCell(&blob, 7, 1, kVacuum, 300.0f, 50.0f, 7.0f);
    PutCell(&blob, 0, 2, kIce, 5000.0f, 100.0f);
    PutCell(&blob, 1, 2, kGranite, -inf, 100.0f);
    PutCell(&blob, 2, 2, kGranite, 300.0f, 100.0f, 4.0f);

    for (bool cluster : {false, true}) {
      printf(" %s:\n", cluster ? "cluster path" : "saved-game path");
      const std::vector<LoadedCell> c = LoadAndRead(t, blob, cluster);
      Check(c.size() == static_cast<size_t>(kLoadW * kLoadH), "Load accepts the blob");
      if (c.size() != static_cast<size_t>(kLoadW * kLoadH)) continue;
      auto cell = [&](int32_t x, int32_t y) -> const LoadedCell& { return c[y * kLoadW + x]; };
      if (getenv("WORLDGEN_DUMP")) {
        for (int32_t y = 0; y < 3; ++y) {
          for (int32_t x = 0; x < kLoadW; ++x) {
            const LoadedCell& l = cell(x, y);
            printf("    (%d,%d) elem=%u mass=%.9g temp=%.9g rad=%.9g\n", x, y, l.element, l.mass,
                   l.temperature, l.radiation);
          }
        }
      }
      Check(cell(0, 0).element == magma && cell(0, 0).mass == 100.0f &&
                cell(0, 0).temperature == 4998.5f,
            "Granite at 5000 K loads as Magma at 4998.5 K");
      Check(cell(1, 0).element == magma && cell(1, 0).temperature == 4998.5f,
            "a massless cell transitions too");
      Check(cell(2, 0).element == ice && cell(2, 0).temperature == 201.5f &&
                cell(2, 0).mass == 100.0f,
            "Water at 200 K loads as Ice at 201.5 K");
      Check(cell(3, 0).element == water && cell(3, 0).temperature == 293.0f,
            "Water at 0 K is raised to 293 K first and does not freeze");
      Check(cell(4, 0).element == granite && cell(4, 0).temperature == at_margin,
            "exactly highTemp + 3 does not transition");
      Check(cell(5, 0).element == eg.highTempTransitionIdx &&
                cell(5, 0).temperature == past_margin - 1.5f,
            "just past highTemp + 3 does");
      if (ore_elem >= 0) {
        Check(cell(6, 0).element == t.At(ore_elem)->highTempTransitionIdx &&
                  cell(6, 0).mass == 100.0f && cell(6, 0).temperature == ore_t - 1.5f,
              "a transition with an ore keeps the whole mass in the cell");
      }
      Check(cell(7, 0).element == ice && cell(7, 0).temperature == 2.5f,
            "Water at 1 K loads as Ice at 2.5 K");
      if (void_idx >= 0) {
        Check(cell(0, 1).element == void_idx && cell(0, 1).mass == 0.0f &&
                  cell(0, 1).temperature == 0.0f && cell(0, 1).radiation == 0.0f,
              "Void loads with no mass, heat or radiation");
      }
      Check(cell(1, 1).element == granite && cell(1, 1).temperature == 293.0f,
            "a NaN temperature loads as 293 K");
      Check(cell(2, 1).element == granite && cell(2, 1).temperature == 293.0f,
            "an infinite temperature loads as 293 K");
      Check(cell(1, 2).element == granite && cell(1, 2).temperature == 293.0f,
            "a negative infinite temperature loads as 293 K");
      Check(cell(3, 1).element == granite && cell(3, 1).mass == 100.0f,
            "a NaN mass loads as 100 kg");
      Check(cell(4, 1).element == vacuum && cell(4, 1).mass == 0.0f &&
                cell(4, 1).temperature == 0.0f && cell(4, 1).radiation == 0.0f,
            "an unknown element hash loads as Vacuum");
      if (legacy_idx >= 0) {
        Check(cell(5, 1).element == legacy_idx && cell(5, 1).mass == 50.0f,
              "a legacy element hash loads as its renamed element");
      }
      Check(cell(6, 1).radiation == 0.0f, "negative radiation loads as 0");
      Check(cell(7, 1).element == vacuum && cell(7, 1).radiation == 0.0f,
            "Vacuum loads with no radiation");
      Check(cell(2, 2).radiation == 4.0f, "positive radiation is kept");
      Check(cell(0, 2).element == water && cell(0, 2).temperature == 4998.5f,
            "one step only: Ice at 5000 K loads as Water, not Steam");
    }
  }
  printf("\n%s\n", g_failed ? "FAILED" : "all checks passed");
  return g_failed ? 1 : 0;
}
