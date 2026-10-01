// Reverse the SimDLL save blob, offline.
//
// SIM_BeginSave returns an opaque byte blob that the game length-prefixes into the save
// file; SIM_HandleMessage(Load, ...) takes it back. Worldgen round-trips through that
// pair once per asteroid *before a new game can start*, so a replacement sim cannot
// boot a new game without it.
//
// Since we control both ends of the round trip in a replacement, we only need the real
// format for one thing: converting saves that already exist. That is a decoding job, and
// decoding is much cheaper against a sim we can seed with known worlds than against a
// save file we did not write. This tool seeds a world, saves it, and diffs blobs from
// worlds that differ in exactly one field.
//
// Usage:
//   savefmt.exe --corpus <corpus.bin> [--dll <path>] --probe
//   savefmt.exe --corpus <corpus.bin> --dump <out.bin>
//   savefmt.exe --corpus <corpus.bin> --roundtrip

#include <cmath>
#include <cstdlib>

#include "../../sim/saveblob.h"
#include "simhost.h"

using namespace simhost;
using oni_sim::DecodeSaveBlob;
using oni_sim::EncodeSaveBlob;
using oni_sim::SaveBlob;

namespace {

// The probe world is deliberately tiny so a blob is small enough to read by eye, and
// deliberately heterogeneous so no two fields hold the same bytes.
constexpr int kW = 8;
constexpr int kH = 6;

struct World {
  int width = kW;
  int height = kH;
  std::vector<Cell> cells;
  std::vector<DiseaseCell> disease;
  std::vector<SimBackwall> backwall;
  uint32_t seed = 12345u;
  bool radiation = false;

  int Cell_(int x, int y) const { return y * width + x; }
};

World MakeWorld(const Tables& t, uint16_t vacuum, uint16_t oxygen, uint16_t granite) {
  (void)t;
  World w;
  const size_t n = static_cast<size_t>(kW) * kH;
  w.cells.assign(n, Cell{});
  w.disease.assign(n, DiseaseCell{});
  w.backwall.assign(n, SimBackwall{});
  for (size_t i = 0; i < n; ++i) {
    w.cells[i].elementIdx = oxygen;
    w.cells[i].mass = 1.0f;
    w.cells[i].temperature = 293.15f;
    w.cells[i].insulation = 255;
    w.disease[i].diseaseIdx = 0xFF;
    w.backwall[i].elementIdx = vacuum;
  }
  for (int x = 0; x < kW; ++x) {
    for (int y : {0, kH - 1}) {
      w.cells[w.Cell_(x, y)].elementIdx = granite;
      w.cells[w.Cell_(x, y)].mass = 2000.0f;
    }
  }
  for (int y = 0; y < kH; ++y) {
    for (int x : {0, kW - 1}) {
      w.cells[w.Cell_(x, y)].elementIdx = granite;
      w.cells[w.Cell_(x, y)].mass = 2000.0f;
    }
  }
  return w;
}

void SendWorld(const World& w) {
  Writer b;
  b.Put<int32_t>(w.width);
  b.Put<int32_t>(w.height);
  b.Put<uint32_t>(w.seed);
  b.PutBool(w.radiation);
  b.PutBool(true);   // headless
  for (const Cell& c : w.cells) b.PutRaw(&c, sizeof(Cell));
  for (const DiseaseCell& d : w.disease) b.PutRaw(&d, sizeof(DiseaseCell));
  for (const SimBackwall& s : w.backwall) b.PutRaw(&s, sizeof(SimBackwall));
  Send(SimMessageHash::SimData_InitializeFromCells, b);
}

const GameDataUpdate* Boot(const Tables& t, const World& w) {
  SendRaw(SimMessageHash::Elements_CreateTable, t.elements);
  SendRaw(SimMessageHash::Disease_CreateTable, t.diseases);
  SendWorld(w);
  return static_cast<const GameDataUpdate*>(SendEmpty(SimMessageHash::Start));
}

// Save a world through a fresh sim instance and hand back the blob. Every probe gets
// its own instance so nothing carries over between them.
std::vector<uint8_t> SaveWorld(const char* dll, const Tables& t, const World& w, int x,
                               int y) {
  if (!Bind(dll)) return {};
  sim_initialize(&DefaultMessageHandler);
  std::vector<uint8_t> blob;
  if (Boot(t, w)) blob = Save(x, y);
  sim_shutdown();
  return blob;
}

// Byte ranges where two equal-length blobs differ, coalesced so a four-byte float shows
// up as one run rather than four findings.
struct Run {
  size_t begin;
  size_t end;  // exclusive
};

std::vector<Run> Diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  std::vector<Run> runs;
  const size_t n = a.size() < b.size() ? a.size() : b.size();
  size_t i = 0;
  while (i < n) {
    if (a[i] == b[i]) {
      ++i;
      continue;
    }
    const size_t start = i;
    // Join runs separated by up to 3 equal bytes: a changed float often has bytes that
    // happen to match, and splitting on those hides the field.
    size_t last = i;
    while (i < n && i - last <= 4) {
      if (a[i] != b[i]) last = i;
      ++i;
    }
    runs.push_back({start, last + 1});
    i = last + 1;
  }
  return runs;
}

void PrintDiff(const char* label, const std::vector<uint8_t>& base,
               const std::vector<uint8_t>& other) {
  if (base.size() != other.size()) {
    printf("  %-28s size %zu -> %zu  (%+lld bytes)\n", label, base.size(), other.size(),
           static_cast<long long>(other.size()) - static_cast<long long>(base.size()));
    return;
  }
  const std::vector<Run> runs = Diff(base, other);
  if (runs.empty()) {
    printf("  %-28s identical\n", label);
    return;
  }
  size_t changed = 0;
  for (const Run& r : runs) changed += r.end - r.begin;
  printf("  %-28s %zu run(s), %zu bytes:", label, runs.size(), changed);
  for (size_t i = 0; i < runs.size() && i < 8; ++i) {
    printf(" [%zu,%zu)", runs[i].begin, runs[i].end);
  }
  if (runs.size() > 8) printf(" ...");
  printf("\n");
}

void Hexdump(const std::vector<uint8_t>& b, size_t from, size_t count) {
  for (size_t i = from; i < from + count && i < b.size(); i += 16) {
    printf("  %06zx  ", i);
    for (size_t j = 0; j < 16; ++j) {
      if (i + j < b.size()) printf("%02x ", b[i + j]);
      else printf("   ");
    }
    printf(" |");
    for (size_t j = 0; j < 16 && i + j < b.size(); ++j) {
      const uint8_t c = b[i + j];
      printf("%c", (c >= 32 && c < 127) ? c : '.');
    }
    printf("|\n");
  }
}

// Is the blob plausibly compressed? A compressed blob makes single-field diffs useless,
// so this is worth knowing before reading anything into the diff results.
void ReportEntropy(const std::vector<uint8_t>& b) {
  size_t hist[256] = {0};
  for (uint8_t c : b) ++hist[c];
  double h = 0;
  for (size_t i = 0; i < 256; ++i) {
    if (!hist[i]) continue;
    const double p = static_cast<double>(hist[i]) / b.size();
    h -= p * (log(p) / log(2.0));
  }
  size_t zeros = hist[0];
  printf("  entropy %.3f bits/byte, %.1f%% zero bytes -> %s\n", h,
         100.0 * zeros / b.size(),
         h > 7.5 ? "looks compressed or encrypted" : "looks like raw structured data");
}

// Change exactly one field of one cell and report where the blob moved. Two cells that
// differ only in position give the stride; the stride and one known offset give the
// base. Probing one field at a time matters — a probe that changes element *and* mass
// cannot tell which byte belongs to which.
using Mutate = void (*)(World*, int);

// Captured by the stateless lambdas below, which must stay convertible to Mutate.
uint16_t g_water = 0;
uint16_t g_granite = 0;

struct Located {
  bool found = false;
  size_t first = 0;
  size_t last = 0;  // inclusive
};

Located LocateField(const char* dll, const Tables& t, const World& base,
                    const std::vector<uint8_t>& blob, int cell, Mutate mutate) {
  World w = base;
  mutate(&w, cell);
  const std::vector<uint8_t> other = SaveWorld(dll, t, w, 0, 0);
  Located out;
  if (other.size() != blob.size()) return out;
  for (size_t i = 0; i < blob.size(); ++i) {
    if (blob[i] == other[i]) continue;
    if (!out.found) {
      out.found = true;
      out.first = i;
    }
    out.last = i;
  }
  return out;
}

void ProbeField(const char* dll, const Tables& t, const World& base,
                const std::vector<uint8_t>& blob, const char* name, Mutate mutate) {
  // Three cells: consecutive in x gives the element stride, one row apart gives the
  // row stride, which is what reveals the padded width.
  const int a = base.Cell_(2, 2), b = base.Cell_(3, 2), c = base.Cell_(2, 3);
  const Located la = LocateField(dll, t, base, blob, a, mutate);
  const Located lb = LocateField(dll, t, base, blob, b, mutate);
  const Located lc = LocateField(dll, t, base, blob, c, mutate);
  if (!la.found || !lb.found) {
    printf("  %-22s no change in the blob\n", name);
    return;
  }
  const long long stride = static_cast<long long>(lb.first) - la.first;
  const long long row = static_cast<long long>(lc.first) - la.first;
  printf("  %-22s bytes %zu..%zu at (2,2); stride %lld, row stride %lld",
         name, la.first, la.last, stride, row);
  if (stride > 0 && row > 0 && row % stride == 0) {
    printf(", padded width %lld", row / stride);
  }
  printf("\n");
}

int RunProbe(const char* dll, const Tables& t) {
  uint16_t vacuum, oxygen, granite, water;
  if (t.IndexOf(kVacuum) < 0 || t.IndexOf(kOxygen) < 0 || t.IndexOf(kGranite) < 0 ||
      t.IndexOf(kWater) < 0) {
    printf("required elements missing from the table\n");
    return 1;
  }
  vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  water = static_cast<uint16_t>(t.IndexOf(kWater));

  const World base = MakeWorld(t, vacuum, oxygen, granite);
  const std::vector<uint8_t> blob = SaveWorld(dll, t, base, 0, 0);
  if (blob.empty()) {
    printf("baseline save produced nothing\n");
    return 1;
  }
  const size_t cells = static_cast<size_t>(kW) * kH;
  printf("baseline blob: %zu bytes for %zu cells (%.2f bytes/cell)\n", blob.size(), cells,
         static_cast<double>(blob.size()) / cells);
  ReportEntropy(blob);
  printf("\nfirst 128 bytes:\n");
  Hexdump(blob, 0, 128);
  printf("\nlast 64 bytes:\n");
  Hexdump(blob, blob.size() > 64 ? blob.size() - 64 : 0, 64);

  printf("\nper-field location (one field changed at a time):\n");
  g_water = water;
  g_granite = granite;
  ProbeField(dll, t, base, blob, "cell.elementIdx",
             [](World* w, int c) { w->cells[c].elementIdx = g_water; });
  ProbeField(dll, t, base, blob, "cell.mass",
             [](World* w, int c) { w->cells[c].mass = 7.5f; });
  ProbeField(dll, t, base, blob, "cell.temperature",
             [](World* w, int c) { w->cells[c].temperature = 400.0f; });
  ProbeField(dll, t, base, blob, "cell.insulation",
             [](World* w, int c) { w->cells[c].insulation = 7; });
  ProbeField(dll, t, base, blob, "cell.strengthInfo",
             [](World* w, int c) { w->cells[c].strengthInfo = 0x5A; });
  ProbeField(dll, t, base, blob, "cell.properties",
             [](World* w, int c) { w->cells[c].properties = 0x0C; });
  ProbeField(dll, t, base, blob, "disease.diseaseIdx",
             [](World* w, int c) { w->disease[c].diseaseIdx = 0; });
  ProbeField(dll, t, base, blob, "disease.elementCount", [](World* w, int c) {
    w->disease[c].diseaseIdx = 0;
    w->disease[c].elementCount = 12345;
  });
  ProbeField(dll, t, base, blob, "backwall.elementIdx",
             [](World* w, int c) { w->backwall[c].elementIdx = g_granite; });
  ProbeField(dll, t, base, blob, "backwall.mass",
             [](World* w, int c) { w->backwall[c].mass = 123.5f; });
  ProbeField(dll, t, base, blob, "backwall.temperature",
             [](World* w, int c) { w->backwall[c].temperature = 305.0f; });

  // Insulation, strength and properties do not survive SimData_InitializeFromCells, so
  // seeding them in the world tells us nothing. Set them with the messages the game uses
  // instead, after boot and before saving.
  printf("\npost-boot probes (fields the world seed cannot reach):\n");
  {
    struct FloatMsg {
      int32_t cellIdx;
      float value;
    };
    struct PropMsg {
      int32_t cellIdx;
      int32_t callbackIdx;
      uint8_t properties;
      uint8_t set;
      uint8_t pad0;
      uint8_t pad1;
    };
    const int cell = base.Cell_(2, 2);
    // Messages are queued, not applied on receipt, so a save taken before the next tick
    // would show nothing whatever the format holds. Both arms tick the same number of
    // times so the diff isolates the message rather than the tick.
    auto after_boot = [&](SimMessageHash hash, const void* msg, int len) {
      if (!Bind(dll)) return std::vector<uint8_t>();
      sim_initialize(&DefaultMessageHandler);
      std::vector<uint8_t> out;
      if (Boot(t, base)) {
        if (len > 0) {
          sim_handle_message(static_cast<int32_t>(hash), len,
                             static_cast<const uint8_t*>(msg));
        }
        NewGameFrame f{};
        f.elapsedSeconds = 0.2f;
        f.maxX = kW;
        f.maxY = kH - 1;
        sim_handle_message(
            static_cast<int32_t>(SimMessageHash::SimFrameManager_NewGameFrame),
            sizeof(f), reinterpret_cast<const uint8_t*>(&f));
        std::vector<uint8_t> visible(static_cast<size_t>(kW) * kH, 1);
        sim_handle_message(static_cast<int32_t>(SimMessageHash::PrepareGameData),
                           static_cast<int>(visible.size()), visible.data());
        out = Save(0, 0);
      }
      sim_shutdown();
      return out;
    };
    // Ticking moves fluid, so the no-message arm is the baseline for these three.
    const std::vector<uint8_t> ticked = after_boot(SimMessageHash::Start, nullptr, 0);
    FloatMsg ins{cell, 0.25f};
    PrintDiff("SetInsulationValue 0.25", ticked,
              after_boot(SimMessageHash::SetInsulationValue, &ins, sizeof(ins)));
    FloatMsg str{cell, 0.5f};
    PrintDiff("SetStrengthValue 0.5", ticked,
              after_boot(SimMessageHash::SetStrengthValue, &str, sizeof(str)));
    PropMsg prop{cell, -1, 0x04, 1, 0, 0};
    PrintDiff("ChangeCellProperties +0x04", ticked,
              after_boot(SimMessageHash::ChangeCellProperties, &prop, sizeof(prop)));
  }

  printf("\nwhole-blob probes:\n");
  {
    World w = base;
    w.seed = 999u;
    PrintDiff("world seed 12345 -> 999", blob, SaveWorld(dll, t, w, 0, 0));
  }
  PrintDiff("BeginSave(x=3, y=2)", blob, SaveWorld(dll, t, base, 3, 2));
  {
    // A real game blob has a non-zero float in the last four bytes of every cell
    // record, and this world does not. Radiation is the only per-cell float left in
    // GameDataUpdate that is unaccounted for, so turn it on and see.
    World w = base;
    w.radiation = true;
    const std::vector<uint8_t> rad = SaveWorld(dll, t, w, 0, 0);
    PrintDiff("radiation enabled", blob, rad);

    // The flag alone changes nothing because an empty world has no rads in it. Put
    // some into one cell and see whether the unaccounted float is where it lands.
    struct RadMsg {
      int32_t cellIdx;
      float radiationDelta;
      int32_t callbackIdx;
    };
    std::vector<uint8_t> radcell;
    if (Bind(dll)) {
      sim_initialize(&DefaultMessageHandler);
      if (Boot(t, w)) {
        RadMsg m{base.Cell_(2, 2), 250.0f, -1};
        sim_handle_message(static_cast<int32_t>(SimMessageHash::CellRadiationModification),
                           sizeof(m), reinterpret_cast<const uint8_t*>(&m));
        NewGameFrame f{};
        f.elapsedSeconds = 0.2f;
        f.maxX = kW;
        f.maxY = kH - 1;
        sim_handle_message(
            static_cast<int32_t>(SimMessageHash::SimFrameManager_NewGameFrame),
            sizeof(f), reinterpret_cast<const uint8_t*>(&f));
        std::vector<uint8_t> visible(static_cast<size_t>(kW) * kH, 1);
        sim_handle_message(static_cast<int32_t>(SimMessageHash::PrepareGameData),
                           static_cast<int>(visible.size()), visible.data());
        radcell = Save(0, 0);
      }
      sim_shutdown();
    }
    PrintDiff("+250 rads at cell(2,2)", rad, radcell);
    if (radcell.size() == rad.size()) {
      float v = 0;
      memcpy(&v, radcell.data() + 29 + 16 * (3 * 10 + 3) + 12, 4);
      printf("  cell(2,2) record +12 after adding rads: %.4f\n", v);
    }
    if (rad.size() == blob.size()) {
      const size_t rec = 29 + 16 * (3 * 10 + 3);
      float before_v = 0, after_v = 0;
      memcpy(&before_v, blob.data() + rec + 12, 4);
      memcpy(&after_v, rad.data() + rec + 12, 4);
      printf("  cell(2,2) record +12 as float: %.4f -> %.4f\n", before_v, after_v);
      printf("  header byte 28: %u -> %u\n", blob[28], rad[28]);
    }
  }
  return 0;
}

// The acid test: save a world, boot a completely fresh sim, load the blob into it, and
// compare what the sim reports back against what it reported before. If this passes, a
// replacement only has to be self-consistent to boot a new game.
int RunRoundTrip(const char* dll, const Tables& t) {
  uint16_t vacuum, oxygen, granite, water;
  vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  water = static_cast<uint16_t>(t.IndexOf(kWater));

  World w = MakeWorld(t, vacuum, oxygen, granite);
  w.cells[w.Cell_(3, 3)].elementIdx = water;
  w.cells[w.Cell_(3, 3)].mass = 850.0f;
  w.cells[w.Cell_(3, 3)].temperature = 310.0f;
  w.disease[w.Cell_(4, 2)].diseaseIdx = 0;
  w.disease[w.Cell_(4, 2)].elementCount = 5000;

  const size_t cells = static_cast<size_t>(kW) * kH;
  std::vector<uint16_t> element(cells);
  std::vector<float> mass(cells), temperature(cells);
  std::vector<uint8_t> disease_idx(cells);
  std::vector<int32_t> disease_count(cells);
  std::vector<uint8_t> blob;

  if (!Bind(dll)) return 1;
  sim_initialize(&DefaultMessageHandler);
  const GameDataUpdate* g = Boot(t, w);
  if (!g) {
    printf("boot failed\n");
    sim_shutdown();
    return 1;
  }
  for (size_t i = 0; i < cells; ++i) {
    element[i] = g->elementIdx[i];
    mass[i] = g->mass[i];
    temperature[i] = g->temperature[i];
    disease_idx[i] = g->diseaseIdx[i];
    disease_count[i] = g->diseaseCount[i];
  }
  blob = Save(0, 0);
  sim_shutdown();
  printf("saved %zu bytes\n", blob.size());
  if (blob.empty()) return 1;

  // A load path boot is not an InitializeFromCells boot: the game allocates the grid
  // first, then feeds the blob in. Order taken from SaveLoader.Load.
  if (!Bind(dll)) return 1;
  sim_initialize(&DefaultMessageHandler);
  SendRaw(SimMessageHash::Elements_CreateTable, t.elements);
  {
    Writer b;
    b.Put<int32_t>(kW);
    b.Put<int32_t>(kH);
    b.PutBool(false);  // radiation
    b.PutBool(true);   // headless
    Send(SimMessageHash::AllocateCells, b);
  }
  SendRaw(SimMessageHash::Disease_CreateTable, t.diseases);
  SendEmpty(SimMessageHash::ClearUnoccupiedCells);
  const void* loaded = SendRaw(SimMessageHash::Load, blob);
  printf("Load returned %s\n", loaded ? "non-null (accepted)" : "NULL (rejected)");
  if (!loaded) {
    sim_shutdown();
    return 1;
  }
  const GameDataUpdate* g2 = static_cast<const GameDataUpdate*>(
      SendEmpty(SimMessageHash::Start));
  if (!g2) {
    printf("Start after Load returned NULL\n");
    sim_shutdown();
    return 1;
  }

  size_t bad_element = 0, bad_mass = 0, bad_temp = 0, bad_disease = 0;
  for (size_t i = 0; i < cells; ++i) {
    if (g2->elementIdx[i] != element[i]) ++bad_element;
    if (g2->mass[i] != mass[i]) ++bad_mass;
    if (g2->temperature[i] != temperature[i]) ++bad_temp;
    if (g2->diseaseIdx[i] != disease_idx[i] || g2->diseaseCount[i] != disease_count[i]) {
      ++bad_disease;
    }
  }
  printf("round trip over %zu cells: element %zu wrong, mass %zu, temperature %zu,"
         " disease %zu\n", cells, bad_element, bad_mass, bad_temp, bad_disease);
  const bool ok = !bad_element && !bad_mass && !bad_temp && !bad_disease;
  printf("%s\n", ok ? "  exact round trip" : "  ROUND TRIP LOSSY");
  sim_shutdown();
  return ok ? 0 : 1;
}

// Decode a blob with sim/saveblob.h, re-encode it, and require the result to be byte
// identical. Run against the real blob the game produced (captured in the corpus) this
// is the strongest available check: anything the decoder misunderstands, drops, or pads
// shows up as a mismatch.
bool VerifyCodec(const std::vector<uint8_t>& blob, const char* label) {
  std::string error;
  SaveBlob decoded;
  if (!DecodeSaveBlob(blob.data(), blob.size(), &decoded, &error)) {
    printf("  %-22s decode failed: %s\n", label, error.c_str());
    return false;
  }
  const std::vector<uint8_t> re = EncodeSaveBlob(decoded);
  if (re.size() != blob.size()) {
    printf("  %-22s re-encoded to %zu bytes, expected %zu\n", label, re.size(),
           blob.size());
    return false;
  }
  size_t bad = 0, first = 0;
  for (size_t i = 0; i < blob.size(); ++i) {
    if (re[i] != blob[i]) {
      if (!bad) first = i;
      ++bad;
    }
  }
  printf("  %-22s %d x %d padded (game %d x %d), %zu bytes, %zu cells", label,
         decoded.width, decoded.height, decoded.GameWidth(), decoded.GameHeight(),
         blob.size(), decoded.Count());
  if (bad) {
    printf("  -- %zu bytes differ, first at %zu\n", bad, first);
    return false;
  }
  printf("  -- byte identical\n");
  return true;
}

// Pull the Load payload out of a shim corpus. Corpus record: seq | id | length | count |
// length*count bytes.
bool LoadBlobFromCorpus(const char* path, std::vector<uint8_t>* out) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  bool found = false;
  for (;;) {
    int32_t h[4];
    if (fread(h, sizeof(h), 1, f) != 1) break;
    const size_t bytes = static_cast<size_t>(h[2]) * (h[3] > 0 ? h[3] : 1);
    if (h[1] == static_cast<int32_t>(SimMessageHash::Load)) {
      out->resize(bytes);
      found = bytes && fread(out->data(), 1, bytes, f) == bytes;
      break;
    }
    if (fseek(f, static_cast<long>(bytes), SEEK_CUR) != 0) break;
  }
  fclose(f);
  return found;
}

int RunVerify(const char* dll, const Tables& t, const char* corpus) {
  bool ok = true;
  uint16_t vacuum = static_cast<uint16_t>(t.IndexOf(kVacuum));
  uint16_t oxygen = static_cast<uint16_t>(t.IndexOf(kOxygen));
  uint16_t granite = static_cast<uint16_t>(t.IndexOf(kGranite));
  const std::vector<uint8_t> mine =
      SaveWorld(dll, t, MakeWorld(t, vacuum, oxygen, granite), 0, 0);
  if (mine.empty()) {
    printf("  could not produce a blob to verify against\n");
    return 1;
  }
  ok &= VerifyCodec(mine, "synthetic 8x6 world");

  std::vector<uint8_t> real;
  if (LoadBlobFromCorpus(corpus, &real)) {
    ok &= VerifyCodec(real, "real blob from corpus");
  } else {
    printf("  %-22s no Load message in the corpus, skipped\n", "real blob");
  }
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  const char* dll = "SimDLL_orig.dll";
  const char* corpus = nullptr;
  const char* dump = nullptr;
  bool probe = false, roundtrip = false, verify = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--dll") && i + 1 < argc) dll = argv[++i];
    else if (!strcmp(argv[i], "--corpus") && i + 1 < argc) corpus = argv[++i];
    else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
    else if (!strcmp(argv[i], "--probe")) probe = true;
    else if (!strcmp(argv[i], "--roundtrip")) roundtrip = true;
    else if (!strcmp(argv[i], "--verify")) verify = true;
  }
  if (!corpus || (!probe && !roundtrip && !dump && !verify)) {
    printf("usage: savefmt.exe --corpus <corpus.bin> [--dll <p>]"
           " (--probe | --roundtrip | --verify | --dump <out.bin>)\n");
    return 2;
  }

  Tables tables;
  if (!LoadTables(corpus, &tables)) return 1;
  printf("element table: %d elements\n\n", tables.count);

  if (dump) {
    uint16_t vacuum = static_cast<uint16_t>(tables.IndexOf(kVacuum));
    uint16_t oxygen = static_cast<uint16_t>(tables.IndexOf(kOxygen));
    uint16_t granite = static_cast<uint16_t>(tables.IndexOf(kGranite));
    const std::vector<uint8_t> blob =
        SaveWorld(dll, tables, MakeWorld(tables, vacuum, oxygen, granite), 0, 0);
    FILE* f = fopen(dump, "wb");
    if (!f) {
      printf("cannot write %s\n", dump);
      return 1;
    }
    fwrite(blob.data(), 1, blob.size(), f);
    fclose(f);
    printf("wrote %zu bytes to %s\n", blob.size(), dump);
  }
  if (verify) {
    printf("codec round trip (sim/saveblob.h):\n");
    if (RunVerify(dll, tables, corpus) != 0) return 1;
  }
  if (probe && RunProbe(dll, tables) != 0) return 1;
  if (roundtrip && RunRoundTrip(dll, tables) != 0) return 1;
  return 0;
}
