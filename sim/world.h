// The replacement sim's world state.
//
// Storage is padded exactly the way Klei's is — a one-cell border ring, so the save blob
// is a straight serialisation and neighbour loops never need a bounds test. The game,
// however, indexes `GameDataUpdate` arrays by *unpadded* game cell, so every projection
// has to convert. Getting that wrong shifts the whole world by one row and looks like
// corruption rather than an off-by-one.
//
// Cells hold a list of phases rather than one element. Today that list is always length
// one, which is what makes this "vanilla-equivalent"; the volume-fraction model fills it
// in later. The projection is already written against the list, so it does not change.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../abi/sim_abi.h"
#include "saveblob.h"

namespace oni_sim {

// Element.state packs the phase into the low two bits and flags above it.
enum : uint8_t {
  kStateMask = 3,
  kStateVacuum = 0,
  kStateGas = 1,
  kStateLiquid = 2,
  kStateSolid = 3,
  kStateUnbreakable = 4,
  kStateUnstable = 8,
};

// Cell.properties bits, from Sim.Cell.Properties.
enum : uint8_t {
  kGasImpermeable = 1,
  kLiquidImpermeable = 2,
  kSolidImpermeable = 4,
  kUnbreakable = 8,
  kTransparent = 0x10,
  kOpaque = 0x20,
  kNotifyOnMelt = 0x40,
  // The bit the radiation absorption walk tests, and the only reader of it in the sim:
  // a constructed tile absorbs `RADIATION_CONSTRUCTED_FACTOR` of its element's factor
  // instead of the mass-weighted mix every other cell gets. Klei tests it as the sign of
  // the signed byte, which is what makes 0x80 and "negative" the same question.
  kConstructedTile = 0x80,
};

// ------------------------------------------------------------- SSE min/max, spelled out
//
// SSE scalar min is `dst = (dst < src) ? dst : src`. On finite operands that is `std::min`
// with the arguments in this order; on a NaN it is the opposite of it. Per the Intel SDM, if
// *either* operand is a NaN the instruction returns `src` outright — so a game clamp written as
// SSE min-then-max doubles as a NaN scrubber and lands on the bound, while the C++ it reads as
// (`if (v > hi) v = hi;`, or `std::min(v, hi)`) compares false against the NaN and keeps it.
// SSE max has the same asymmetry.
//
// That is not a theoretical difference. Klei's sim divides without guarding the denominator
// in at least a dozen places, and every one of those is a 0/0 the moment some mass or heat
// capacity is exactly zero — which the game supplies routinely (neutronium's specific heat is
// 0, a cell of the right element can hold no mass, a building can register with no cells).
// Writing one of those clamps as comparisons gives a NaN where the game gives a bound: the
// element consumer's temperature (`emitters.h`) and the germ fraction in `UpdatePressure`
// (`physics.h`) are two places it shows.
//
// The `ClampSS` argument order is the game's own: min against `hi` first, then max against
// `lo`, which is what makes a NaN come out as `hi` rather than as `lo`.
inline float MinSS(float dst, float src) { return dst < src ? dst : src; }
inline float MaxSS(float dst, float src) { return dst > src ? dst : src; }
inline double MinSD(double dst, double src) { return dst < src ? dst : src; }
inline double MaxSD(double dst, double src) { return dst > src ? dst : src; }
inline float ClampSS(float v, float lo, float hi) { return MaxSS(MinSS(v, hi), lo); }

// One substance in a cell. A single-element cell has exactly one of these; a
// volume-fraction cell will have several and they must stay sorted by element index so
// the dominant-phase tie-break is deterministic.
struct PhaseEntry {
  uint16_t element = 0;
  float mass = 0.0f;
  float temperature = 0.0f;
};

// Per disease, per element. Eight fields, and the order is the order the game writes them
// in — the order `Disease::Disease` fills its eight per-element vectors, reading
// one entry of each per element per record. Six floats,
// one int and one byte is the 29-byte stride the old size-only parser had already measured.
//
// Which field is which came from the two kernels rather than from a name: `GetDiffusionScale`
// returns vector 3 and gates on vectors 6 and 7, and `PostProcess`
// picks vector 1 in range, vector 2 over, and vector 0 under. That is exactly
// Klei's own `ElemGrowthInfo` declaration order.
struct ElemGrowthInfo {
  float underPopulationDeathRate = 0.0f;
  float populationHalfLife = 0.0f;
  float overPopulationHalfLife = 0.0f;
  float diffusionScale = 0.0f;
  float minCountPerKG = 0.0f;
  float maxCountPerKG = 0.0f;
  int32_t minDiffusionCount = 0;
  uint8_t minDiffusionInfestationTickCount = 0;
};

// One disease. 0x150 bytes in the DLL; the four `RangeInfo`s are `{minViable, minGrowth,
// maxGrowth, maxViable}` and are read in ascending order by the temperature sweep.
//
// The two pressure ranges are parsed because the game sends them and the stride depends on
// them, but **nothing in the sim reads them**. `PostProcess` looks at temperature and at the
// per-element population bounds and at nothing else; pressure lives entirely game-side.
struct DiseaseInfo {
  int32_t id_hash = 0;
  float strength = 0.0f;
  float temperature_range[4] = {0, 0, 0, 0};
  float temperature_half_lives[4] = {0, 0, 0, 0};
  float pressure_range[4] = {0, 0, 0, 0};
  float pressure_half_lives[4] = {0, 0, 0, 0};
  float radiation_kill_rate = 0.0f;
  std::vector<ElemGrowthInfo> growth;
};

// The id hashes turn a cell's disease index into the hash the save format stores and back;
// the rest is the growth and half-life data the two disease kernels run on.
class DiseaseTable {
 public:
  bool Load(const uint8_t* data, size_t size) {
    // 0 int32 diseaseCount | 4 int32 elementCount | per disease: KleiString name,
    // int32 idHashCode, ... The record is variable length because of the name and the
    // per-element growth table, so walk it rather than indexing.
    hashes_.clear();
    infos_.clear();
    if (size < 8) return false;
    int32_t disease_count = 0, element_count = 0;
    memcpy(&disease_count, data, 4);
    memcpy(&element_count, data + 4, 4);
    if (disease_count < 0 || element_count < 0) return false;
    size_t off = 8;
    for (int32_t i = 0; i < disease_count; ++i) {
      int32_t name_len = 0;
      if (off + 4 > size) return false;
      memcpy(&name_len, data + off, 4);
      off += 4;
      if (name_len < 0 || off + static_cast<size_t>(name_len) + 4 > size) return false;
      off += static_cast<size_t>(name_len);
      int32_t hash = 0;
      memcpy(&hash, data + off, 4);
      hashes_.push_back(hash);
      // Everything after the name, in the order `Disease::Disease` reads it. The record is
      // walked twice — once to keep the hash list this class has always published, once to
      // fill the info — and the two agree by construction because both walk `off`.
      if (off + kDiseaseFixedSize +
              static_cast<size_t>(element_count) * kElemGrowthInfoSize >
          size) {
        return false;
      }
      DiseaseInfo info;
      size_t p = off;
      auto f32 = [&]() {
        float v = 0.0f;
        memcpy(&v, data + p, 4);
        p += 4;
        return v;
      };
      auto i32 = [&]() {
        int32_t v = 0;
        memcpy(&v, data + p, 4);
        p += 4;
        return v;
      };
      info.id_hash = i32();
      info.strength = f32();
      for (int k = 0; k < 4; ++k) info.temperature_range[k] = f32();
      for (int k = 0; k < 4; ++k) info.temperature_half_lives[k] = f32();
      for (int k = 0; k < 4; ++k) info.pressure_range[k] = f32();
      for (int k = 0; k < 4; ++k) info.pressure_half_lives[k] = f32();
      info.radiation_kill_rate = f32();
      info.growth.resize(static_cast<size_t>(element_count));
      for (int32_t e = 0; e < element_count; ++e) {
        ElemGrowthInfo& g = info.growth[static_cast<size_t>(e)];
        g.underPopulationDeathRate = f32();
        g.populationHalfLife = f32();
        g.overPopulationHalfLife = f32();
        g.diffusionScale = f32();
        g.minCountPerKG = f32();
        g.maxCountPerKG = f32();
        g.minDiffusionCount = i32();
        g.minDiffusionInfestationTickCount = data[p];
        p += 1;
      }
      infos_.push_back(std::move(info));
      // The fixed part that follows the name, then the per-element growth table.
      //
      // Both sizes were guessed the first time round and both were wrong, which made this
      // function return false on the *real* game's table — the DLL reported "disease table
      // rejected" and every disease hash silently became 0. It went unnoticed because
      // disease is stubbed and no scenario has an infected cell in it.
      //
      // Recovered by walking the real 31,220-byte payload: the five records are Food
      // Poisoning (14), Slimelung (9), Floral Scent (12), Zombie Spores (13) and
      // Radioactive Contaminants (24), and with a 76-byte fixed part and a **29-byte**
      // growth record they land on 6250 / 12487 / 18727 / 24968 and end on exactly 31220.
      // Every one of those matches an observed name offset. 29 is not a mistake for 32:
      // this is one of the byte-packed serialisations, 7 floats plus a trailing byte, and
      // a stride scan over the payload picks 29 out by a factor of seven over any other
      // stride.
      off += kDiseaseFixedSize;
      off += static_cast<size_t>(element_count) * kElemGrowthInfoSize;
      if (off > size) return false;
    }
    return true;
  }

  int32_t Count() const { return static_cast<int32_t>(hashes_.size()); }
  int32_t HashOf(uint8_t idx) const {
    return idx < hashes_.size() ? hashes_[idx] : 0;
  }
  uint8_t IndexOfHash(int32_t hash) const {
    for (size_t i = 0; i < hashes_.size(); ++i) {
      if (hashes_[i] == hash) return static_cast<uint8_t>(i);
    }
    return 0xFF;
  }

  // Null for an index the table does not hold, which is what a world loaded before the
  // table arrives looks like. Both kernels check.
  const DiseaseInfo* Get(uint8_t idx) const {
    return idx < infos_.size() ? &infos_[idx] : nullptr;
  }

 private:
  // Measured, not derived — see Load. The fixed part covers idHashCode and everything up
  // to the growth table; the individual fields in it are not needed here, only its size.
  static constexpr size_t kDiseaseFixedSize = 76;
  static constexpr size_t kElemGrowthInfoSize = 29;
  std::vector<int32_t> hashes_;
  std::vector<DiseaseInfo> infos_;
};

class ElementTable {
 public:
  // The table arrives as int32 count followed by that many Element records.
  bool Load(const uint8_t* data, size_t size) {
    if (size < 4) return false;
    int32_t n = 0;
    memcpy(&n, data, 4);
    if (n <= 0 || 4 + static_cast<size_t>(n) * sizeof(Element) > size) return false;
    elements_.resize(n);
    memcpy(elements_.data(), data + 4, static_cast<size_t>(n) * sizeof(Element));
    by_hash_.clear();
    ++generation_;
    return true;
  }

  int32_t Count() const { return static_cast<int32_t>(elements_.size()); }
  // Bumped by every `Load`. Anything that caches a value derived from the table keys its
  // cache on this as well as on the table's address, because a table can be reloaded in
  // place and the address alone would not notice.
  uint32_t Generation() const { return generation_; }
  bool Empty() const { return elements_.empty(); }
  const Element& At(uint16_t i) const { return elements_[i < Count() ? i : 0]; }

  // A backwall index past the end of the table is "no element", not element 0: worldgen sends
  // 0xFFFF for every template cell that leaves `backwallElement` unset. Klei keeps the raw
  // index, publishes 0xFFFF and saves it as Vacuum. Stored here as hash 0, which no element has.
  static constexpr int32_t kNoElementHash = 0;
  int32_t BackwallHash(uint16_t i) const {
    return i < Count() ? elements_[i].id : kNoElementHash;
  }

  // Saves key elements by SimHashes value, not by index, so this lookup is on the
  // critical path of every load — and it is not only a load-time lookup: `Project` asks
  // for the backwall element of every cell in the world, every frame. The index used to
  // be an unsorted vector walked linearly, which is 212 compares for a hash the table does
  // not hold, and a cell with no backwall carries hash 0, which the table never holds.
  // Sorted plus `lower_bound` instead. Duplicate hashes keep resolving to the *lowest*
  // index, the way a linear scan from the front did, because the sort is by (hash, index).
  uint16_t IndexOfHash(int32_t hash) const {
    const std::pair<int32_t, uint16_t>* e = Find(hash);
    return e ? e->second : 0;  // vacuum-ish fallback; callers check HasHash first
  }
  bool HasHash(int32_t hash) const { return Find(hash) != nullptr; }

  // Klei reports a cell that flow has emptied as Vacuum, not as a massless cell of
  // whatever used to be there — a shaft a drop of water has just left reads back as
  // element 211 with zero mass, not as water.
  uint16_t VacuumIndex() const {
    constexpr int32_t kVacuumHash = 758759285;
    return HasHash(kVacuumHash) ? IndexOfHash(kVacuumHash) : static_cast<uint16_t>(0);
  }

  // The Void index. Void is the element whose cells destroy whatever is pushed into
  // them: `UpdatePressure` compares the destination's element against this index
  // and, when it matches, zeroes the cell's mass and temperature instead of
  // filling it. No `diffsim` scenario carries one, so when the table has no Void this
  // returns an index no element can hold and the comparison never fires.
  uint16_t VoidIndex() const {
    constexpr int32_t kVoidHash = -1456075980;
    return HasHash(kVoidHash) ? IndexOfHash(kVoidHash) : static_cast<uint16_t>(0xFFFF);
  }

  // The world border. `SimData::ResizeAndInitializeVacuumCells` rings the rectangle it
  // opens with this at a flat 9999 kg and 0 K — measured against Klei on `vacrect`, and
  // 9999 is a constant in the function rather than anything in Unobtanium's own row (its
  // table entry carries 10000 and 20000, and neither is what lands in the cell).
  uint16_t UnobtaniumIndex() const {
    constexpr int32_t kUnobtaniumHash = 1838482828;
    return HasHash(kUnobtaniumHash) ? IndexOfHash(kUnobtaniumHash)
                                    : static_cast<uint16_t>(0xFFFF);
  }

  uint8_t Phase(uint16_t i) const { return At(i).state & kStateMask; }
  bool IsSolid(uint16_t i) const { return Phase(i) == kStateSolid; }
  bool IsVacuum(uint16_t i) const { return Phase(i) == kStateVacuum; }
  float SpecificHeat(uint16_t i) const { return At(i).specificHeatCapacity; }

 private:
  const std::pair<int32_t, uint16_t>* Find(int32_t hash) const {
    if (by_hash_.empty()) BuildIndex();
    const auto it = std::lower_bound(
        by_hash_.begin(), by_hash_.end(), hash,
        [](const std::pair<int32_t, uint16_t>& p, int32_t h) { return p.first < h; });
    if (it == by_hash_.end() || it->first != hash) return nullptr;
    return &*it;
  }
  void BuildIndex() const {
    by_hash_.reserve(elements_.size());
    for (size_t i = 0; i < elements_.size(); ++i) {
      by_hash_.emplace_back(elements_[i].id, static_cast<uint16_t>(i));
    }
    std::sort(by_hash_.begin(), by_hash_.end());
  }
  std::vector<Element> elements_;
  mutable std::vector<std::pair<int32_t, uint16_t>> by_hash_;
  uint32_t generation_ = 0;
};

// One game cell, carrying both of the indices every whole-grid loop needs.
//
// `World::Padded` is a 64-bit division by a width that is only known at run time, and the
// two loops that walk the whole grid — `Project` and `FillPropertyTextures` — were calling
// it once per cell. On a 512x768 grid that was 0.44 ms in each of them, a third of the
// projection and most of the texture pass, spent recomputing an index that advances by one
// per cell and by two more at the end of every row (`padded - game` is `2y + gw + 3`).
// Walking it costs an add and a loop-carried compare instead.
struct CellWalk {
  size_t game;
  size_t padded;
};

class GameCellRange {
 public:
  GameCellRange(int32_t game_width, size_t count, int32_t padded_width)
      : game_width_(game_width), count_(count), padded_width_(padded_width) {}

  class Iterator {
   public:
    Iterator(size_t game, size_t padded, int32_t game_width)
        : game_(game), padded_(padded), x_(0), game_width_(game_width) {}
    CellWalk operator*() const { return CellWalk{game_, padded_}; }
    Iterator& operator++() {
      ++game_;
      ++padded_;
      if (++x_ == game_width_) {
        x_ = 0;
        padded_ += 2;  // skip this row's right border and the next row's left
      }
      return *this;
    }
    bool operator!=(const Iterator& other) const { return game_ != other.game_; }

   private:
    size_t game_;
    size_t padded_;
    int32_t x_;
    int32_t game_width_;
  };

  Iterator begin() const {
    return Iterator(0, static_cast<size_t>(padded_width_) + 1, game_width_);
  }
  Iterator end() const { return Iterator(count_, 0, game_width_); }

 private:
  int32_t game_width_;
  size_t count_;
  int32_t padded_width_;
};

// The two `CellSOA::CopyFrom` snapshots a substep takes. Both live here rather than beside
// the kernels that fill them, because the flow texture reads one of them after the substep
// is over.
//
// The snapshot the *two* gas sweeps share. `SimBase::UpdateData` takes one
// and then runs both the pressure sweep and the displacement sweep off it — there is no
// second copy between them — so `StepGasDisplacement` reads a `start` that predates every
// transfer `StepGasPressure` made, and the two must be called in that order with nothing in
// between.
//
// It is *not* what the flow texture gates on — that is `LiquidSweepStart` below, and
// swapping the two was tried and put nineteen scenarios wrong.
inline std::vector<PhaseEntry>& GasSweepStart() {
  static thread_local std::vector<PhaseEntry> start;
  return start;
}

// The `CellSOA::CopyFrom` — the one a substep takes between the two gas
// sweeps and `UpdateLiquid`, and the last one it takes at all. `StepFlow` prices its
// transfers against it, and `UpdateFlowTexture` reads it a second time after the substep is
// over: a cell whose element has changed since publishes no flow at all.
inline std::vector<PhaseEntry>& LiquidSweepStart() {
  static thread_local std::vector<PhaseEntry> start;
  return start;
}

// What a cell's disease looked like at the same instant. `Disease::UpdateCells`
// reads both cells of a pair out of the snapshot and accumulates into the live grid
// through `AddDiseaseToCell`, which is the gas-pressure
// pattern: the amount a pair moves is independent of the order the sweep visits pairs in,
// but a cell can receive from all four of its neighbours in one sweep.
//
// It is filled beside `LiquidSweepStart` and not at the top of the disease sweep, because
// the snapshot the disease sweep reads is the one taken — *before*
// `UpdateLiquid`, not after it.
struct DiseaseEntry {
  uint8_t idx = 0xFF;
  int32_t count = 0;
  // The infestation age goes in the snapshot too, because `Disease::GetDiffusionScale` gates
  // on it and reads it from `SimData::cells` like everything else it reads.
  uint8_t infest = 0;
};
inline std::vector<DiseaseEntry>& DiseaseSweepStart() {
  static thread_local std::vector<DiseaseEntry> start;
  return start;
}

// The same shape, one substep-phase earlier: `UpdatePressure`'s inline germ transfer
// reads the *count* it prices its fraction against from a snapshot too —
// `Disease::AddDiseaseToCell`'s live grid is distinct from the snapshot the divide reads,
// matching the mass split (`start[c].mass` vs `cells[c].mass`) in the same function. Reading
// live instead cascades a source cell's germs across an entire
// vacuum room in one substep — the destination of one pair becomes a fresh, undrained
// source for its own next pair before the snapshot would have reset it — which `germvac`'s
// tick-2 output does not show (Klei moves exactly one row, not the whole chamber). Filled
// once per `StepGasPressure` call, the same cadence `GasSweepStart` uses.
inline std::vector<DiseaseEntry>& GasDiseaseSweepStart() {
  static thread_local std::vector<DiseaseEntry> start;
  return start;
}

// ------------------------------------------------------- refreshing a snapshot per region
//
// Klei takes `CellSOA::CopyFrom` once per SUBSTEP, four times in all, and then runs its cell
// tasks off that one copy.
// This sim takes one per REGION instead, because the region loop in `StepPhysics` wraps every
// kernel rather than sitting inside one. On a single-region world those are the same thing.
// On a multi-region one they are not, and the difference is R copies of the whole grid where
// Klei makes one.
//
// THAT IS NOT A ROUNDING ERROR. `Game.UnsafeSim200ms` builds one `SimActiveRegion` per
// DISCOVERED WORLD, so a Spaced Out cluster save sends one region per asteroid — 5 to 12 in a
// late game, against the 1 the base game sends, which is the only shape anything in this
// repository had ever run. Measured here with `bench --regions cluster:N`, which cuts one
// asteroid into N disjoint strips so only the region count moves. Asteroid, 60 ticks, min ms:
//
//   regions           1        2        4        8       12
//   StepFlow      0.159    0.360    0.585    0.819    1.176   <- 7.4x for the same sweep
//   StepGasPressure 1.437  1.581    1.726    2.165    2.514
//   frame         7.247    7.888    8.377    9.082   10.277   <- +42 %, zero extra physics
//
// `StepFlow` copies 12 bytes a cell over a 99,588-cell padded grid — 1.19 MB per region, and
// twelve of those is 14.3 MB of traffic a substep. A real cluster is worse than this table,
// which holds the GRID fixed: the game's grid grows with the asteroid count too, so the true
// shape is R copies of an R-times-larger grid.
//
// THE FIX, AND WHY IT IS SHAPED LIKE THIS. Region 0 still copies the whole grid, so a
// single-region world — the base game, and every scenario in the offline suite — executes
// literally the code it did before and cannot regress. Regions 1..R-1 refresh only the
// rectangle their own sweeps can read. The values inside that rectangle are the ones a
// whole-grid copy would have written; the cells outside it keep region 0's copy from earlier
// in this same substep, which is what Klei's single copy would have left there anyway.
//
// THE MARGIN IS 3 because that is the widest reach any reader of these snapshots has. The
// displacement sweeps read a cell two past the source they are moving (`beyond` in the gas
// sweep, `other` in the liquid one) and are bounded by the region otherwise, and
// `StepGasPressure` reads its far end out of the region taken INCLUSIVELY, one row above
// `r.y1`. The y range starts at row 0 rather than at the region because of Klei's
// `min(y0, 3)` — the displacement sweeps deliberately begin near the bottom of the GRID
// whatever the region says, which `StepGasDisplacement` documents in full. A rectangle that
// is too big is safe and still O(region); one that is too small is a stale read, which is why
// these margins are generous rather than tight.
//
// WHAT THE ABLATION ACTUALLY PROVES, AND WHAT IT DOES NOT. Margins of 0, 1, 2, 4, 8 and 16
// were built and run. The offline suite cannot tell any of them apart — `diffsim --scenario
// all` reports the same md5 and 0 FAILED at margin 0, `regionsplit`, `regionadj` and
// `regionlap` included, because those scenarios are two regions on a small, quiet world. The
// detector is `bench --regions cluster:12`'s world digest against the whole-grid build: it
// separates margin 0 from every other margin, and margins 1 through 16 all reproduce the
// whole-grid world exactly, at 1, 2, 4, 8 and 12 regions.
//
// So the measurement says the rectangle matters and that 1 is already enough ON THIS WORLD.
// It does not say 3 is necessary — that comes from the reach in the code above, which is where
// a margin should come from. The y margin is weaker still: `rows:N` splits detect nothing at
// any margin here, so `ry1 + 3` is unexercised by measurement and rests entirely on the same
// reading.
struct SnapshotRect {
  int32_t x0, y0, x1, y1;
};
inline SnapshotRect SnapshotReadRect(int32_t pw, int32_t ph, int32_t rx0, int32_t ry0,
                                     int32_t rx1, int32_t ry1) {
  (void)ry0;
  SnapshotRect q;
  q.x0 = rx0 - 3 > 0 ? rx0 - 3 : 0;
  q.y0 = 0;
  q.x1 = rx1 + 3 < pw ? rx1 + 3 : pw;
  q.y1 = ry1 + 3 < ph ? ry1 + 3 : ph;
  return q;
}

// Whether this region has to take the whole grid. Region 0 always does — it is Klei's copy,
// and it is what every single-region world has always executed — and so does the first call
// on a world whose grid has changed size, so that no cell outside any region is ever left
// holding a default-constructed entry.
inline bool SnapshotNeedsWholeGrid(size_t have, size_t n, size_t ri) {
  return ri == 0 || have != n;
}

// The rows of `q`, as half-open [begin, end) index runs into a padded grid.
template <class Row>
inline void ForEachSnapshotRow(int32_t pw, const SnapshotRect& q, Row row) {
  if (q.x1 <= q.x0) return;
  for (int32_t y = q.y0; y < q.y1; ++y) {
    const size_t base = static_cast<size_t>(y) * static_cast<size_t>(pw);
    row(base + static_cast<size_t>(q.x0), base + static_cast<size_t>(q.x1));
  }
}

// The world. Padded dimensions internally, unpadded on the way out.
class World {
 public:
  void Allocate(int32_t game_width, int32_t game_height) {
    // Seeding and loading write the static arrays through the members directly rather than
    // through the Mutable* accessors, so they say "all of it" by hand.
    static_dirty_.clear();
    static_dirty_all_ = true;
    game_width_ = game_width;
    game_height_ = game_height;
    // `AllocateCells` builds a *new* `SimData`, so the five radiation tunables go back to
    // the constructor's defaults with it — a params message does not survive a reallocation.
    radiation_linger_rate_ = 1.1f;
    radiation_max_mass_ = 2000.0f;
    radiation_base_weight_ = 0.3f;
    radiation_density_weight_ = 0.7f;
    radiation_constructed_factor_ = 0.8f;
    width_ = game_width + 2;
    height_ = game_height + 2;
    const size_t n = PaddedCount();
    phases_.assign(n, {});
    radiation_.assign(n, 0.0f);
    disease_.assign(n, SaveDisease{});
    disease_idx_.assign(n, 0xFF);
    disease_accum_.assign(n, 0.0f);
    disease_infest_.assign(n, 0);
    backwall_.assign(n, SaveBackwall{});
    properties_.assign(n, 0);
    // 255, not 0. `CellSOA::GetInsulationValue` squares this over 255^2, so 0 zeroes solid
    // conduction outright and 255 is the game's own "not insulated".
    // `InitializeFromCells` overwrites every one of these explicitly per cell right after
    // Allocate, so this default only reaches daylight on the path that doesn't: `FromBlob`,
    // where the save carries no insulation byte at all (confirmed against SIM_BeginSave's own
    // 36-byte cell record) and the only other writer is the game's Insulator/Door components,
    // which only ever touch cells a building sits on. A cell nothing wrote to needs the neutral
    // value, not the minimum one.
    insulation_.assign(n, 255);
    strength_.assign(n, 0);
    active_.assign(n, 1);
    active_incl_.assign(n, 1);
    // A world that has never been sent a region behaves as though the whole grid were in
    // play, and the rectangles have to say the same thing the masks do.
    padded_.assign(1, {1, 1, width_ - 1, height_ - 1});
    padded_incl_ = padded_;
    region_cosmic_.assign(1, 0.0f);
    substance_touched_.assign(n, 0);
    stable_ticks_.assign(n, kStableTicksReroll);
    flow_.assign(n * 4, 0.0f);
    flow_touched_.clear();
    project_row_x0_.assign(static_cast<size_t>(height_), kNoDirtyRow);
    project_row_x1_.assign(static_cast<size_t>(height_), -1);
    project_y0_ = height_;
    project_y1_ = -1;
    project_dirty_all_ = true;
    ledger_ = Ledger{};
    allocated_ = true;
  }

  // ------------------------------------------------------------------- the flow accumulator
  //
  // Four floats a cell, in padded cell indices, and the only per-cell
  // field in the sim that records a *transfer* rather than a state. Nothing reads it but
  // `UpdateFlowTexture`, which takes `[0] - [1]` for the published x and
  // `[3] - [2]` for the published y — so both ends of a transfer land on the same signed
  // value, and the sign is the direction the mass went in.
  //
  // Every write goes through `AddFlow`, which records the cell as well. The recording is
  // what keeps the buffer affordable: it is 6 MB on a 512x768 asteroid and both the clear
  // and the texture pass would otherwise be whole-grid walks for a field that is zero
  // wherever nothing is moving — the solid two thirds of a real world, always.
  //
  // A write of exactly zero is dropped rather than recorded. Klei performs it — the refused
  // half of a pressure pair still runs the accumulate with the returned 0.0 — but adding
  // zero cannot change a float, and every cell in the world takes one of those every
  // substep, so recording them would put the whole grid on the list and undo the point.
  std::vector<float>& FlowAccum() { return flow_; }
  const std::vector<float>& FlowAccum() const { return flow_; }
  // `{padded, game}` for each recorded cell. The texture pass has neither index and needs
  // both — it reads the accumulator and the cell by padded index and writes the texture by
  // game index — and recovering one from the other is `GameIndex`, a 64-bit division by a
  // run-time width. Every caller already has both for free, so they are carried.
  //
  // Worth 0.03 ms of the texture pass's 0.34 on an asteroid, and no more: the division was
  // the obvious suspect and the cost is really the scattered traffic either side of it.
  // Kept because it is measurably faster and the callers pay nothing, not because it was
  // the fix it was expected to be.
  struct FlowCell {
    uint32_t padded;
    uint32_t game;
  };
  const std::vector<FlowCell>& FlowTouched() const { return flow_touched_; }
  void AddFlow(size_t cell, size_t game_cell, int slot, float amount) {
    if (amount == 0.0f) return;
    float* f = &flow_[cell * 4];
    // Recorded once, on the write that takes the cell off zero. A gas cell on a live
    // asteroid takes four or five of these a substep and the texture pass wants it once.
    if (f[0] == 0.0f && f[1] == 0.0f && f[2] == 0.0f && f[3] == 0.0f) {
      flow_touched_.push_back(
          FlowCell{static_cast<uint32_t>(cell), static_cast<uint32_t>(game_cell)});
    }
    f[static_cast<size_t>(slot)] += amount;
  }
  void ClearFlow() {
    for (const FlowCell& c : flow_touched_) {
      float* f = &flow_[static_cast<size_t>(c.padded) * 4];
      f[0] = 0.0f;
      f[1] = 0.0f;
      f[2] = 0.0f;
      f[3] = 0.0f;
    }
    flow_touched_.clear();
    flow_element_.clear();
  }

  // What the flow texture's gate compares against.
  //
  // `UpdateFlowTexture` publishes nothing for a cell whose `updatedCells`
  // element differs from its `cells` element, and the `cells` copy it reads is the substep's
  // **fourth and last** `CellSOA::CopyFrom`, — after `UpdateLiquid` *and*
  // after the liquid displacement pass, not the copy that `LiquidSweepStart`
  // holds. The two were indistinguishable for as long as nothing between them changed an
  // element, and the comment on `LiquidSweepStart` calling "the last one it
  // takes at all" was simply wrong.
  //
  // So the gate is really "did anything change this cell's element *after* the liquid
  // section" — the disease sweep, the components, `PostProcessCell`, `ZeroMasslessCells` —
  // and a cell the liquid mover poured into publishes its flow rather than being silenced.
  // That was `sunliquid`'s last divergence: one cell, (12,1), where water arrived during
  // `UpdateLiquid` and our texture read `-0` because the scale was zero.
  //
  // Recorded per flow-touched cell rather than as a grid snapshot: the texture only ever
  // asks about cells the accumulator wrote, so this is a few hundred entries on an asteroid
  // where a `CellSOA::CopyFrom` is a megabyte. Every substep overwrites it, so what survives
  // to publish time is the last substep's — which is Klei's copy.
  void SnapshotFlowElements() {
    flow_element_.resize(flow_touched_.size());
    for (size_t i = 0; i < flow_touched_.size(); ++i) {
      flow_element_[i] = phases_[flow_touched_[i].padded].element;
    }
  }
  const std::vector<uint16_t>& FlowElements() const { return flow_element_; }

  // Which cells a kernel wrote a substance into this frame.
  //
  // This is not a diff and it is not a set of cells whose element changed. Klei records it
  // by hand: `SimEvents::ChangeSubstance` pushes `{ gameCell, 0xffff, 0xffff }`
  // — the element indices are *not* captured at the call — and `SimBase::CopySimDataToGame`
  // fills `oldElemIdx` from the previously published element array and `newElemIdx` from the
  // one it is publishing,. A cell swapped with a neighbour
  // holding the same gas is therefore announced as `183 -> 183`, and that is the bulk of the
  // traffic in any room with a shuffle in it.
  //
  // Klei sorts the list by cell and runs `std::unique` over it before publishing (the
  // `_Buffered_rotate_unchecked<SubstanceChangeInfo>` in `.text` is `std::stable_sort`'s
  // merge, and is the unique pass walking backwards). A flag per cell scanned in
  // game-cell order at projection time gives sorted-and-deduplicated for free, which is why
  // this is a bitmap and not a list — and it means the *order* kernels touch cells in cannot
  // leak into the output.
  //
  // `ChangeSubstance` does one other thing on the way out, on **both** exits — the one that
  // pushed and the one whose game index fell outside the world — and it is the reason this
  // and `MarkUnstableDirty` are the same call: both set the cell's unstable bits (`|= 0x1f`).
  // Writing a substance into a cell restarts that cell's
  // unstable-solid countdown.
  void TouchSubstance(size_t padded) {
    substance_touched_[padded] = 1;
    stable_ticks_[padded] |= kStableTicksReroll;
  }
  bool SubstanceTouched(size_t padded) const { return substance_touched_[padded] != 0; }

  // `SimEvents::cellMeltedInfo`, the game cells whose tile melted this frame, in the order the
  // transitions ran. Klei's only writer is `DoStateTransition`'s high branch: a cell that heats past its melting point while its property byte
  // carries `kNotifyOnMelt` is reported, and the game destroys the tile that set the bit and
  // raises "building melted". Kept here, beside the touch bitmap, rather than threaded through
  // `TransitionCell`'s three callers, because every one of them reaches the report in Klei.
  // A record of one frame, like the touch bitmap: cleared by the frame's event reset.
  void NoteCellMelted(int32_t game) { cell_melted_.push_back(game); }
  const std::vector<int32_t>& CellMelted() const { return cell_melted_; }
  void ClearCellMelted() { cell_melted_.clear(); }
  void ClearSubstanceTouched() {
    std::fill(substance_touched_.begin(), substance_touched_.end(), 0);
  }

  // One byte a cell, **persistent across frames** — unlike the touch
  // bitmap above, which is a record of one frame. The low five bits are an unstable solid's
  // countdown to falling and the high three are never written by anything.
  //
  // `0x1f` is not a count of 31. It is the sentinel `SimData::GetStableTicksRemaining`
  // reads as "roll a fresh one", and rolling is the **only** place in the
  // unstable path that touches the random stream. Everything else about falling sand is
  // deterministic; this one byte decides which cells draw and when, which is why it has to
  // be exact rather than approximately right.
  //
  // The array starts out all-sentinel: a world that has just loaded has had nothing written
  // into any of its cells, so every unstable solid the active region reaches rolls before it
  // moves. Measured — see `sand` in `diffsim` — not assumed.
  static constexpr uint8_t kStableTicksReroll = 0x1f;
  void MarkUnstableDirty(size_t padded) { stable_ticks_[padded] |= kStableTicksReroll; }

  // `GetStableTicksRemaining` verbatim, including the two things about it that look like
  // mistakes and are not. It **returns the decremented value**, so a cell that reads 1 this
  // substep reads 0 and falls on the next one rather than this one. And on the reroll path
  // it returns the whole seeded byte while storing only its low five bits — which is the
  // same number here, because the seed is `(int)(r * 3/32767) + 3` and so is 3, 4, 5 or 6.
  uint8_t StableTicksRemaining(size_t padded) {
    uint8_t& b = stable_ticks_[padded];
    const uint8_t v = static_cast<uint8_t>(b & kStableTicksReroll);
    if (v == kStableTicksReroll) {
      // The same `rand()` the shuffle draws from, and the same 1/32767 scale, times three.
      const uint32_t s = NextRandomState();
      const uint32_t scale_bits = 0x38C00180u;  // 3.0f / 32767.0f
      float scale;
      memcpy(&scale, &scale_bits, sizeof(scale));
      const float f = static_cast<float>((s >> 16) & 0x7FFFu) * scale;
      const uint8_t rolled = static_cast<uint8_t>(static_cast<int32_t>(f) + 3);
      b = static_cast<uint8_t>((b & 0xE0u) | (rolled & kStableTicksReroll));
      return rolled;
    }
    if (v == 0) return 0;
    const uint8_t next = static_cast<uint8_t>(v - 1);
    b = static_cast<uint8_t>((b & 0xE0u) | next);
    return next;
  }

  // A flag in Klei's `SimData`, out of `AllocateCells` and `SimData_InitializeFromCells`. It decides
  // which half of the unstable path runs: the game gets an `unstableCellInfo` and an emptied
  // cell, so that it can spawn a falling-object entity, and an offline sim gets the fall
  // simulated in the grid instead. `diffsim` sends 1; the game sends 0 in normal play and 1
  // from worldgen.
  bool Headless() const { return headless_; }

  // The active region, from `NewGameFrame`.
  //
  // The sim does not step the whole grid. Each frame the game sends one or more regions and
  // only cells inside them are simulated — measured by putting a hot cell one row under the
  // bound and watching it exchange heat downward and not upward. A pair whose far side is
  // outside the region exchanges *nothing*: the same cell cooled 22.6 K with four active
  // neighbours and 17.1 K with three, which is exactly three quarters.
  //
  // This matters more than it looks. The game clamps `maxY` to `HeightInCells - 1`, so the
  // world's top row is never simulated in a real game either, and a replacement that steps
  // it anyway drifts from Klei in every world whose top row is not uniform. It went
  // unnoticed here for as long as it did because every scenario's top row was identical
  // granite; the first scenario with something interesting up there found it immediately.
  //
  // Cells default to active, so a sim that has not been told otherwise behaves as though
  // the whole grid were in play rather than freezing.
  void SetActiveRegions(const int32_t* regions, size_t count,
                        const float* cosmic = nullptr) {
    if (!allocated_ || count == 0) return;
    active_.assign(PaddedCount(), 0);
    active_incl_.assign(PaddedCount(), 0);
    regions_.clear();
    padded_.clear();
    padded_incl_.clear();
    region_cosmic_.clear();
    for (size_t r = 0; r < count; ++r) {
      const int32_t* q = regions + r * 4;
      // Kept as rectangles as well as as a mask. The per-cell kernels want the mask; the
      // building components want the rectangle, because Klei tests a building's *extents*
      // against the region rather than testing its cells (`BuildingHeatExchange::Update`,
      //), and a building straddling the edge is skipped whole.
      regions_.push_back({q[0], q[1], q[2], q[3]});
      const int32_t min_x = q[0] < 0 ? 0 : q[0];
      const int32_t min_y = q[1] < 0 ? 0 : q[1];
      const int32_t max_x = q[2] > game_width_ ? game_width_ : q[2];
      const int32_t max_y = q[3] > game_height_ ? game_height_ : q[3];
      for (int32_t y = min_y; y < max_y; ++y) {
        for (int32_t x = min_x; x < max_x; ++x) {
          active_[static_cast<size_t>(y + 1) * width_ + (x + 1)] = 1;
        }
      }
      const int32_t max_xi = q[2] >= game_width_ ? game_width_ - 1 : q[2];
      const int32_t max_yi = q[3] >= game_height_ ? game_height_ - 1 : q[3];
      for (int32_t y = min_y; y <= max_yi; ++y) {
        for (int32_t x = min_x; x <= max_xi; ++x) {
          active_incl_[static_cast<size_t>(y + 1) * width_ + (x + 1)] = 1;
        }
      }
      // The same two masks as rectangles, in padded coordinates and half-open, so a sweep
      // can iterate exactly the cells the mask would have accepted instead of walking the
      // grid and asking. `PaddedRect(x0,y0,x1,y1)` and `Active(padded)` are two spellings
      // of one set by construction: everything below is the loop bounds above, shifted by
      // the border ring. The inclusive variant is the same rectangle with its far edge one
      // cell further out, which is what `max_xi`/`max_yi` mean.
      padded_.push_back({min_x + 1, min_y + 1, max_x + 1, max_y + 1});
      region_cosmic_.push_back(cosmic == nullptr ? 0.0f : cosmic[r]);
      padded_incl_.push_back({min_x + 1, min_y + 1, max_xi + 2, max_yi + 2});
    }
  }
  bool Active(size_t padded) const { return active_[padded] != 0; }

  // The same regions, read **inclusively** and in game coordinates.
  //
  // `NewGameFrame` sends `maxY = height - 1`, and the mask above treats that half-open, so
  // the world's top row is never conducted — which is measured and load-bearing: making the
  // mask inclusive instead puts eight scenarios tens of kelvin out. The gas pressure sweep
  // does *not* agree with it. `sunlit` is the only scenario that can tell, because it is the
  // only one whose top row holds something other than uniform border: Klei announces a
  // substance change for every vacuum cell in that row on every frame, and does so whether
  // the harness sends `maxY = height - 1` or `height`, so Klei is clamping internally and
  // then including the clamped row. Nothing in the suite has *movable* gas up there, so this
  // is the announce reproducing exactly rather than a claim about the flow: the two are
  // indistinguishable here, and a scenario with gas on the top row would separate them.
  // Kept as a second mask rather than a rectangle scan: `StepGasPressure` asks this once
  // per *pair*, and walking the region list there cost 0.5 ms a frame on the asteroid.
  bool ActiveInclusive(size_t padded) const { return active_incl_[padded] != 0; }
  const std::vector<uint8_t>& ActiveMask() const { return active_; }

  // The two masks above, as rectangles a sweep can iterate.
  //
  // Padded coordinates, half-open on both axes, already clamped to the interior — so
  // `for (y = r.y0; y < r.y1; ++y) for (x = r.x0; x < r.x1; ++x)` visits exactly the cells
  // `Active` accepts, in exactly the order a full-grid scan would visit them, and a sweep
  // that switches from one to the other is unchanged on any world with a single region.
  //
  // **Klei's structure is region-outer, and since the multi-region refactor so is this.**
  // `SimBase::UpdateData` is one loop over the region list at stride 0x18, and
  // the *entire* frame body sits inside it — conduction, both gas sweeps, liquid,
  // post-process, the components, right down to `Disease::PostProcess`. Klei
  // runs the whole physics frame once per region; two overlapping regions get the frame run
  // twice over their overlap. So `StepPhysics` owns the loop and every sweep now takes the
  // index of the region it is running over, rather than each sweep walking the list itself.
  // The two orderings agree exactly when there is one region, which is every scenario in the
  // suite that predates the region probes and every single-asteroid game; `regionadj` and
  // `regionlap` are the two that can tell them apart.
  struct PaddedRect {
    int32_t x0, y0, x1, y1;
  };
  const std::vector<PaddedRect>& PaddedRegions() const { return padded_; }
  const std::vector<PaddedRect>& PaddedRegionsInclusive() const { return padded_incl_; }

  // How many times the frame body runs. Always at least one: a world that has never been
  // sent a region carries a single rectangle covering the whole interior, which is what
  // makes "no region" and "one region spelled out" the same run — `regionone` is the
  // control that says so.
  //
  // `padded_`, `padded_incl_` and `regions_` are three spellings of one list and are pushed
  // together, except that `regions_` is empty in the defaulted case where the other two hold
  // the whole-world rectangle. Index with this and go through the accessors below.
  size_t RegionCount() const { return padded_.size(); }
  const PaddedRect& PaddedRegion(size_t ri) const { return padded_[ri]; }
  // `NewGameFrame` carries a sunlight and a cosmic-radiation intensity **per region**, and
  // Klei's `activeRegions` record is six ints wide because of it. The cosmic figure is the
  // one the sim reads: every unoccluded cell gains `intensity / RADIATION_LINGER_RATE` of it
  // per substep. A world that has never been sent a frame has none.
  float RegionCosmic(size_t ri) const {
    return ri < region_cosmic_.size() ? region_cosmic_[ri] : 0.0f;
  }
  const PaddedRect& PaddedRegionInclusive(size_t ri) const { return padded_incl_[ri]; }

  // Game coordinates, half-open in x and y, exactly as `NewGameFrame` sends them.
  //
  // A world that has never been sent a region reports none, and the building components
  // read that as "everything is in play" — the same default the mask uses.
  struct Rect {
    int32_t min_x, min_y, max_x, max_y;
  };
  const std::vector<Rect>& Regions() const { return regions_; }

  // The region the frame body is currently running over, in game coordinates. The defaulted
  // case has no `regions_` entry to hand back, so it reports the whole world — which is the
  // rectangle `padded_` holds for that case, read back without the border ring.
  Rect Region(size_t ri) const {
    if (regions_.empty()) return Rect{0, 0, game_width_, game_height_};
    return regions_[ri];
  }

  // A building exchanges heat only if its **extents** sit inside the region, rather than
  // cell by cell (`BuildingHeatExchange::Update`), so one straddling an edge is
  // skipped whole. Since the components run inside `UpdateData`'s region loop
  // (`SimData::UpdateComponents`) the question is asked of one region at a
  // time, and a building inside two of them exchanges twice.
  bool ExtentsInRegion(size_t ri, int32_t min_x, int32_t min_y, int32_t max_x,
                       int32_t max_y) const {
    if (regions_.empty()) return true;
    const Rect& r = regions_[ri];
    return min_x >= r.min_x && min_y >= r.min_y && max_x <= r.max_x && max_y <= r.max_y;
  }

  bool Allocated() const { return allocated_; }
  // The worlds a cluster map is made of, from `DefineWorldOffsets`: four
  // int32s each, in game coordinates. A single-asteroid game sends one; an offline harness
  // that never sends the message has none, and the sunlight texture stays zero.
  struct WorldOffset {
    int32_t x = 0, y = 0, w = 0, h = 0;
  };
  const std::vector<WorldOffset>& WorldOffsets() const { return world_offsets_; }
  // `DefineWorldOffsets` is an immediate message — Klei acts on it inside the handler — but
  // the sunlight texture it feeds does **not** light up on the frame the message arrives:
  // Klei publishes zeros for that frame and the geometry from the next one. Measured, not
  // assumed: `sunlight` sends the offsets before tick 1, and Klei's texture is zero at tick 1
  // and filled from tick 2. So the offsets are staged here and promoted once the frame that
  // received them has published.
  void SetWorldOffsets(std::vector<WorldOffset> v) { pending_world_offsets_ = std::move(v); }
  // `SimData::ResizeAndInitializeVacuumCells` opens a rectangle and that rectangle is a
  // *world*: the sunlight sweep lights it on the next published frame, and a solid dropped
  // into it afterwards casts a shadow down its own column — which is what says the message
  // registers geometry rather than writing 255 into the texture. Measured on `vacrect`.
  //
  // It appends rather than replaces. No scenario has both a `DefineWorldOffsets` and this
  // message, so which one Klei does is **not** established; appending is the reading that
  // matches the name — the message opens *a* world, it does not redefine the cluster.
  //
  // It takes effect **immediately**, unlike `DefineWorldOffsets`, and that is measured too:
  // staged, our rectangle lit at tick 5 against Klei's tick 4, and every later tick agreed.
  // So the two messages do not share the one-frame delay, and the delay is not a property of
  // "world geometry" — whatever produces it for `DefineWorldOffsets` does not apply here. The
  // live list and the pending one both take the rectangle so a promote cannot drop it.
  void AddWorldOffset(const WorldOffset& o) {
    if (pending_world_offsets_.empty()) pending_world_offsets_ = world_offsets_;
    pending_world_offsets_.push_back(o);
    world_offsets_.push_back(o);
  }
  void PromoteWorldOffsets() {
    if (pending_world_offsets_.empty()) return;
    world_offsets_ = pending_world_offsets_;
  }

  int32_t GameWidth() const { return game_width_; }
  int32_t GameHeight() const { return game_height_; }
  size_t GameCount() const {
    return static_cast<size_t>(game_width_) * game_height_;
  }
  int32_t PaddedWidth() const { return width_; }
  int32_t PaddedHeight() const { return height_; }
  size_t PaddedCount() const { return static_cast<size_t>(width_) * height_; }

  // Game cell index -> padded index. The game numbers cells row-major over the
  // unpadded grid; we store the border, so every row shifts by one and every cell
  // shifts by one more.
  // Every game cell in order, with its padded index carried alongside — see `CellWalk`.
  // Identical order to `for (size_t i = 0; i < GameCount(); ++i)` with `Padded(i)`, and the
  // same answer for every cell, without the division.
  GameCellRange GameCells() const {
    return GameCellRange(game_width_, GameCount(), width_);
  }

  size_t Padded(size_t game_cell) const {
    const size_t y = game_cell / static_cast<size_t>(game_width_);
    const size_t x = game_cell - y * static_cast<size_t>(game_width_);
    return (y + 1) * static_cast<size_t>(width_) + (x + 1);
  }
  bool ValidGameCell(int64_t c) const {
    return c >= 0 && static_cast<size_t>(c) < GameCount();
  }
  // The other direction, spelled the way Klei spells it — `cell / width` and `cell % width`
  // for the row and column, then `(width - 2) * (y - 1) + (x - 1)`. Every event that names a
  // cell computes it inline; `ChangeSubstance` is the shortest copy of it.
  // A border cell comes back negative or past the end, which is what callers test.
  int64_t GameIndex(size_t padded) const {
    const int64_t y = static_cast<int64_t>(padded) / width_;
    const int64_t x = static_cast<int64_t>(padded) % width_;
    return (y - 1) * game_width_ + (x - 1);
  }

  PhaseEntry& Phase(size_t padded) { return phases_[padded]; }
  const PhaseEntry& Phase(size_t padded) const { return phases_[padded]; }

  std::vector<PhaseEntry>& Phases() { return phases_; }
  const std::vector<PhaseEntry>& Phases() const { return phases_; }

  // The seven arrays below are the ones `Project` copies straight through to the game with
  // no physics in between: properties, insulation, strength, radiation, the two disease
  // fields and the three backwall fields. On a 512x768 grid, copying all of them for every
  // cell cost 1.5 ms of a 3.4 ms projection, every frame, to reproduce bytes that were
  // already there — the buffers persist between frames and the game re-reads the same
  // pointers, so an unwritten array is not a stale array, it is the same array.
  //
  // So every write to one is recorded by cell, and `Project` copies exactly the cells
  // recorded. That is only sound if *every* writer records, which is why the mutable
  // spellings are named apart from the const ones rather than overloaded on constness: an
  // overload set would hand a mutable reference to `w->Properties()` on a non-const
  // `World*` — which is what several sweeps in physics.h do to *read* it — and the writer
  // set would silently include every reader. Named apart, the compiler finds the writers.
  //
  // They are also per cell rather than per array, and that is not cosmetic either: the
  // first version of this recorded a single "something changed" epoch, and the sublimation
  // tail writes `Disease()[n].count -= 0` on every subliming cell of every substep, so a
  // world with any sublimation in it re-copied everything anyway. A whole-array hook is a
  // hook on the wrong thing.
  float& MutableRadiation(size_t p) { return MarkStatic(p), radiation_[p]; }
  // The radiation field is rewritten for **every cell of the region on every substep** — the
  // decay pass writes each cell whether or not the value moved — so going through
  // `MutableRadiation` would mark the whole grid static-dirty every frame and hand `Project`
  // a full copy of seven arrays it does not need. Writing through this instead marks only
  // the cells whose value actually changed, which is the same guarantee the projection's
  // own comment makes: each copy is either performed or provably redundant.
  void SetRadiation(size_t p, float v) {
    if (radiation_[p] == v) return;
    MarkStatic(p);
    radiation_[p] = v;
  }
  SaveDisease& MutableDisease(size_t p) { return MarkStatic(p), disease_[p]; }
  SaveBackwall& MutableBackwall(size_t p) {
    MarkBackwall(p);
    return MarkStatic(p), backwall_[p];
  }
  uint8_t& MutableProperties(size_t p) { return MarkStatic(p), properties_[p]; }
  uint8_t& MutableInsulation(size_t p) { return MarkStatic(p), insulation_[p]; }
  uint8_t& MutableStrength(size_t p) { return MarkStatic(p), strength_[p]; }
  uint8_t& MutableDiseaseIdx(size_t p) { return MarkStatic(p), disease_idx_[p]; }

  // The cells written since the last projection, in write order and with duplicates. Empty
  // with `StaticDirtyAll()` set means "all of them" — the state a fresh or freshly loaded
  // world is in, and the state the list collapses to once it is longer than the full copy
  // would be. `Project` is the only consumer and clears it; the list is bookkeeping about
  // what a *cache* still owes, not world state, which is why clearing it is const.
  const std::vector<uint32_t>& StaticDirty() const { return static_dirty_; }
  bool StaticDirtyAll() const { return static_dirty_all_; }
  // Forced by `Start`, which publishes the backwall arrays the way Klei does — untouched —
  // and then owes the next projection a full re-copy to fill them in.
  void MarkStaticDirtyAll() {
    static_dirty_all_ = true;
    static_dirty_.clear();
  }

  // The backwalls written since the last frame checked them, in write order. The backwall
  // never changes by itself — no kernel of Klei's touches it, measured — so the set of
  // cells that could newly need a transition is exactly the set something wrote, which is
  // a load (all of them) or a `ModifyBackwallData`.
  const std::vector<uint32_t>& BackwallDirty() const { return backwall_dirty_; }
  bool BackwallDirtyAll() const { return backwall_dirty_all_; }
  void ClearBackwallDirty() {
    backwall_dirty_.clear();
    backwall_dirty_all_ = false;
  }
  void ClearStaticDirty() const {
    static_dirty_.clear();
    static_dirty_all_ = false;
  }

  // ---------------------------------------- what the projection still has to look at
  //
  // `Project` publishes element, mass, temperature and solidity for every game cell, and it
  // walked all of them every frame. It does not have to: the buffers persist between frames
  // and the game re-reads the same pointers, so a cell nothing wrote is not a stale cell,
  // it is the same cell. The set it has to re-derive is the set something *could* have
  // written since the last projection.
  //
  // That set is recorded here as a rectangle per producer: each substep marks the region it
  // is about to drive, and the four message handlers that write a phase entry mark their one
  // cell. Marking is dilated by `kProjectReach` on the way in, because a kernel can write
  // past the rectangle it drives from — the pair sweeps test the far end separately, and
  // `StepGasDisplacement` moves a cell's contents two cells along. Four is a bound, not a
  // derivation: it is one more than the furthest write in the tree, and `bench --verify`
  // is what proves it, by rebuilding every cell from the world and comparing bytes.
  //
  // Kept as one x-span per row rather than as a list of rectangles, and that is the whole
  // reason this shape was chosen. `Project` emits `substanceChangeInfo` in increasing game
  // index, which is how it reproduces Klei's sort and `std::unique` without doing either.
  // A row-major walk over per-row spans is still increasing; a walk over two rectangles in
  // turn is not, and would have to sort afterwards.
  static constexpr int32_t kProjectReach = 4;

  void MarkProjectDirtyAll() const { project_dirty_all_ = true; }

  // One cell, undilated — a property write changes that cell's solidity and nothing else.
  void MarkProjectDirty(size_t padded) const {
    const int32_t y = static_cast<int32_t>(padded / static_cast<size_t>(width_));
    const int32_t x =
        static_cast<int32_t>(padded - static_cast<size_t>(y) * static_cast<size_t>(width_));
    MarkProjectRows(y, y, x, x);
  }

  // Padded and half-open, the way every kernel spells its own loop bounds.
  void MarkProjectDirtyRect(int32_t x0, int32_t y0, int32_t x1, int32_t y1) const {
    if (x1 <= x0 || y1 <= y0) return;
    MarkProjectRows(y0 - kProjectReach, y1 - 1 + kProjectReach, x0 - kProjectReach,
                    x1 - 1 + kProjectReach);
  }

  bool ProjectDirtyAll() const { return project_dirty_all_; }
  int32_t ProjectDirtyY0() const { return project_y0_; }
  int32_t ProjectDirtyY1() const { return project_y1_; }
  int32_t ProjectRowX0(int32_t y) const { return project_row_x0_[static_cast<size_t>(y)]; }
  int32_t ProjectRowX1(int32_t y) const { return project_row_x1_[static_cast<size_t>(y)]; }

  // Only the rows that were marked are reset, so a frame that moved one cell pays for one
  // row rather than for the grid.
  void ClearProjectDirty() const {
    for (int32_t y = project_y0_; y <= project_y1_; ++y) {
      project_row_x0_[static_cast<size_t>(y)] = kNoDirtyRow;
      project_row_x1_[static_cast<size_t>(y)] = -1;
    }
    project_y0_ = height_;
    project_y1_ = -1;
    project_dirty_all_ = false;
  }

  // ------------------------------------------------------------------------- the ledger
  //
  // The conservation check, the one piece of verification that survives the gas mixture
  // intact. `diffsim` compares cell against cell, and a mixed cell has no game cell to compare
  // against; a ledger
  // needs no oracle at all.
  //
  // Mass is conserved by every kernel except a short list of places that export it to the
  // game or destroy it on purpose. So the check is
  //
  //   (total now - total at load) == (what came in) - (what went out)
  //
  // and everything the list does not name lands in the driver's `drift` column. Drift is
  // the pass/fail; the buckets are only there to be subtracted.
  //
  // Buckets are per **call site**, not per cause. One "cleared" bucket that every deleting
  // site charged would balance the books by construction and prove nothing. Split, each
  // site has to say what it claims to be doing, and a bucket that moves in a scenario with
  // no business moving it is itself a finding.
  //
  // Charging is one `+=` at events that already do far more work than that, so it is always
  // on and not gated on anything. The grid total is O(cells) and is *not* — it is computed
  // only when the driver asks, through `SIM_DebugLedger`.
  struct Ledger {
    // Mass the game moved across the boundary, all four of them signed the way the driver
    // reads them: `emitted` and `modified` are additions, the rest are removals.
    double emitted = 0;      // MassEmission
    double modified = 0;     // ModifyCell, which can go either way
    double consumed = 0;     // MassConsumption
    double dug = 0;          // DigInfo — the cell's contents become an entity
    double ore = 0;          // transition ore, handed over as SpawnOreInfo
    double unstable = 0;     // a falling solid, handed to the game or dropped through
    // Mass the physics destroys on purpose. Klei's behaviour in every case, measured, and
    // the reason §2b's original two-item list is not the whole story.
    double sublimated = 0;   // taken * (1 - sublimateEfficiency), sublimation and off-gas
    double wisp = 0;         // gas under 0.001 kg, deleted where it stands
    double thin_liquid = 0;  // liquid under 0.01 kg, likewise
    double cleared = 0;      // `ClearCell` reached a cell that still held mass
    // The two components that move mass across the boundary on their own, without a
    // message per transfer. Separate buckets because they are separate call sites: an
    // emitter that puts mass in the grid and a `MassEmission` that does the same thing are
    // different code and a shared bucket would hide either one going wrong.
    double component_consumed = 0;  // ElementConsumer::Update
    double component_emitted = 0;   // ElementEmitter::Emit
    double emitter_ore = 0;         // a solid emitter, which spawns ore instead of filling a cell
  };

  void NoteEmitted(float kg) const { ledger_.emitted += kg; }
  void NoteModified(float kg) const { ledger_.modified += kg; }
  void NoteConsumed(float kg) const { ledger_.consumed += kg; }
  void NoteDug(float kg) const { ledger_.dug += kg; }
  void NoteOre(float kg) const { ledger_.ore += kg; }
  void NoteUnstable(float kg) const { ledger_.unstable += kg; }
  void NoteSublimated(float kg) const { ledger_.sublimated += kg; }
  void NoteWisp(float kg) const { ledger_.wisp += kg; }
  void NoteThinLiquid(float kg) const { ledger_.thin_liquid += kg; }
  void NoteCleared(float kg) const { ledger_.cleared += kg; }
  void NoteComponentConsumed(float kg) const { ledger_.component_consumed += kg; }
  void NoteComponentEmitted(float kg) const { ledger_.component_emitted += kg; }
  void NoteEmitterOre(float kg) const { ledger_.emitter_ore += kg; }
  const Ledger& Books() const { return ledger_; }

  // Game cells only, which is what the game's own total would be. A mover that wrote into
  // the border ring would take mass out of this sum and show up as drift — which is the
  // right answer, because that mass is gone as far as the game is concerned.
  //
  // Summed in double over floats: the grid is 400k cells and a naive float accumulator
  // loses the low bits of a 1 kg cell long before the last row.
  double TotalGridMass() const {
    double total = 0.0;
    for (const CellWalk cw : GameCells()) total += phases_[cw.padded].mass;
    return total;
  }

  const std::vector<float>& Radiation() const { return radiation_; }
  const std::vector<SaveDisease>& Disease() const { return disease_; }
  const std::vector<SaveBackwall>& Backwall() const { return backwall_; }
  const std::vector<uint8_t>& Properties() const { return properties_; }

  // The random stream and the shuffle's stride. `SetRandomState` is what makes a gas world
  // reproducible: the game stores the seed argument as the stream state raw, with no
  // scramble, so the world seed *is* the first state.
  uint32_t RandomState() const { return rng_; }
  void SetRandomState(uint32_t s) { rng_ = s; }
  uint32_t NextRandomState() {
    rng_ = rng_ * 214013u + 2531011u;
    return rng_;
  }
  int32_t ShuffleDir() const { return shuffle_dir_; }
  void SetShuffleDir(int32_t d) { shuffle_dir_ = d; }
  const std::vector<uint8_t>& Insulation() const { return insulation_; }
  const std::vector<uint8_t>& Strength() const { return strength_; }

  // --------------------------------------------------------------- seeding

  // SimData_InitializeFromCells: unpadded arrays of Sim.Cell / DiseaseCell /
  // SimBackwall, in that order, after the header.
  bool InitializeFromCells(const uint8_t* data, size_t size, const ElementTable& table,
                           const DiseaseTable& diseases) {
    size_t off = 0;
    auto take = [&](void* dst, size_t n) {
      if (off + n > size) return false;
      memcpy(dst, data + off, n);
      off += n;
      return true;
    };
    int32_t w = 0, h = 0;
    uint32_t seed = 0;
    uint8_t radiation_enabled = 0, headless = 0;
    if (!take(&w, 4) || !take(&h, 4) || !take(&seed, 4) || !take(&radiation_enabled, 1) ||
        !take(&headless, 1)) {
      return false;
    }
    if (w <= 0 || h <= 0) return false;
    Allocate(w, h);
    radiation_enabled_ = radiation_enabled != 0;
    headless_ = headless != 0;
    // The seed this message carries is the random stream, verbatim. The game stores it as the
    // stream state and nothing else ever writes it, so
    // a world's whole gas shuffle is determined here. The *other* constructor call site,
    // `AllocateCells`, passes `_time64(NULL)` instead — a loaded save is not reproducible.
    rng_ = seed;
    shuffle_dir_ = -1;

    const size_t n = GameCount();
    for (size_t i = 0; i < n; ++i) {
      Cell c{};
      if (!take(&c, sizeof(Cell))) return false;
      const size_t p = Padded(i);
      phases_[p] = {c.elementIdx, c.mass, c.temperature};
      properties_[p] = c.properties;
      insulation_[p] = c.insulation;
      strength_[p] = c.strengthInfo;
    }
    for (size_t i = 0; i < n; ++i) {
      DiseaseCell d{};
      if (!take(&d, sizeof(DiseaseCell))) return false;
      const size_t p = Padded(i);
      // 0xFF means "no disease". The save format keys diseases by hash, the game data
      // update by index, so both are kept and neither is derived on the fly.
      disease_idx_[p] = d.diseaseIdx;
      disease_[p].diseaseHash = (d.diseaseIdx == 0xFF) ? 0 : diseases.HashOf(d.diseaseIdx);
      disease_[p].count = (d.diseaseIdx == 0xFF) ? 0 : d.elementCount;
      // The game writes zero into both of these, but the payload carries them and the sim
      // owns them from here on, so take what was sent rather than assuming the zero.
      disease_infest_[p] = d.reservedInfestationTickCount;
      disease_accum_[p] = d.reservedAccumulatedError;
    }
    for (size_t i = 0; i < n; ++i) {
      SimBackwall b{};
      if (!take(&b, sizeof(SimBackwall))) return false;
      const size_t p = Padded(i);
      backwall_[p] = {table.BackwallHash(b.elementIdx), b.mass, b.temperature};
    }
    // A freshly loaded world owes the game a transition check on every backwall it just
    // took; Klei announces its whole out-of-range set on the first frame after a load.
    backwall_dirty_all_ = true;
    SealBorder(table);
    return true;
  }

  // --------------------------------------------------------------- save / load

  static constexpr int32_t kVacuumHash = 758759285;

  // `Load` rewrites every cell it takes, on the saved-game path and the cluster path alike;
  // `SimData_InitializeFromCells` does not. This is Klei's `CellRead` followed by
  // the rest of `Load`'s per-cell loop, in Klei's order, and
  // `driver/src/worldgen_test` checks each step against Klei's DLL:
  //
  //   1. Two renamed element hashes are mapped to their new names before the lookup.
  //   2. A temperature that is not finite becomes 293 K, a mass that is not finite 100 kg, and
  //      radiation that is not finite 0.
  //   3. A hash the table does not hold loads as Vacuum with no mass, heat or radiation.
  //   4. A temperature at or below 0 K becomes exactly 293 K, massless or not, and radiation at
  //      or below 0 becomes 0.
  //   5. Vacuum and Void keep no mass, heat or radiation.
  //   6. `DoLoadTimeStateTransition`: one transition if the cell is out of its
  //      element's range.
  //
  // A world's own border ring is 0 K Neutronium under 9999 kg of 0 K Vacuum, and on the
  // cluster path that ring lands inside the playable grid, so step 4 is what keeps it from
  // freezing the gas next to it.
  static constexpr int32_t kLegacyHashA = 0x0280cf79;
  static constexpr int32_t kLegacyHashATo = static_cast<int32_t>(0x987dac06);
  static constexpr int32_t kLegacyHashB = 0x2391c22b;
  static constexpr int32_t kLegacyHashBTo = 0x4c76ae31;

  static void ReadLoadedCell(const SaveCell& in, const ElementTable& table, PhaseEntry* out,
                             float* radiation) {
    int32_t hash = in.elementHash;
    if (hash == kLegacyHashA) {
      hash = kLegacyHashATo;
    } else if (hash == kLegacyHashB) {
      hash = kLegacyHashBTo;
    }
    float temperature = std::isfinite(in.temperature) ? in.temperature : 293.0f;
    float mass = std::isfinite(in.mass) ? in.mass : 100.0f;
    float rad = std::isfinite(in.radiation) ? in.radiation : 0.0f;
    uint16_t element;
    if (table.HasHash(hash)) {
      element = table.IndexOfHash(hash);
    } else {
      element = table.VacuumIndex();
      mass = 0.0f;
      temperature = 0.0f;
      rad = 0.0f;
    }
    if (temperature <= 0.0f) temperature = 293.0f;
    if (rad <= 0.0f) rad = 0.0f;
    if (element == table.VoidIndex() || element == table.VacuumIndex()) {
      temperature = 0.0f;
      mass = 0.0f;
      rad = 0.0f;
    }
    LoadTimeStateTransition(table, &element, &temperature);
    *out = {element, mass, temperature};
    *radiation = rad;
  }

  // `DoLoadTimeStateTransition`. The frame's `DoStateTransition` with the same 3 K margin and
  // 1.5 K overshoot (sim/physics.h), but a different function, and it differs in three ways:
  //
  //   * no mass gate: a massless Granite cell at 5000 K loads as massless Magma;
  //   * no transition ore: the whole mass stays in the cell, and nothing is announced;
  //   * the low side is tested first. A cell is never out of range on both sides, so this
  //     changes nothing, but it is Klei's order.
  //
  // It runs once, so a cell two transitions out of range takes one: Ice at 5000 K loads as
  // Water at 4998.5 K, and the first frame finishes the job.
  static void LoadTimeStateTransition(const ElementTable& table, uint16_t* element,
                                      float* temperature) {
    constexpr float kMargin = 3.0f, kOvershoot = 1.5f;
    constexpr uint16_t kNone = 0xFFFF;
    if (*element >= table.Count()) return;
    const Element& e = table.At(*element);
    if (*temperature < e.lowTemp - kMargin && e.lowTempTransitionIdx != kNone &&
        e.lowTempTransitionIdx < table.Count()) {
      *temperature += kOvershoot;
      *element = e.lowTempTransitionIdx;
    } else if (*temperature > e.highTemp + kMargin && e.highTempTransitionIdx != kNone &&
               e.highTempTransitionIdx < table.Count()) {
      *temperature -= kOvershoot;
      *element = e.highTempTransitionIdx;
    }
  }

  SaveBlob ToBlob(int32_t x, int32_t y, const ElementTable& table) const {
    SaveBlob b;
    b.width = width_;
    b.height = height_;
    b.x = x;
    b.y = y;
    const size_t n = PaddedCount();
    b.cells.resize(n);
    b.disease = disease_;
    b.backwall = backwall_;
    for (SaveBackwall& bw : b.backwall) {
      if (bw.elementHash == ElementTable::kNoElementHash) bw.elementHash = kVacuumHash;
    }
    for (size_t i = 0; i < n; ++i) {
      b.cells[i].elementHash = table.At(phases_[i].element).id;
      b.cells[i].temperature = phases_[i].temperature;
      b.cells[i].mass = phases_[i].mass;
      b.cells[i].radiation = radiation_[i];
    }
    return b;
  }

  // Loading resolves element *hashes*, so a table whose ordering changed since the save
  // was written still produces the right materials. A hash the table does not hold loads as
  // Vacuum, as it does in Klei's `CellRead`; only a missing table is refused.
  bool FromBlob(const SaveBlob& b, const ElementTable& table,
                const DiseaseTable& diseases, std::string* error) {
    if (b.width <= 2 || b.height <= 2) {
      if (error) *error = "blob has degenerate dimensions";
      return false;
    }
    if (table.Empty()) {
      if (error) *error = "Load before Elements_CreateTable";
      return false;
    }
    Allocate(b.GameWidth(), b.GameHeight());
    const size_t n = PaddedCount();
    if (b.cells.size() != n || b.disease.size() != n || b.backwall.size() != n) {
      if (error) *error = "blob arrays do not match its own dimensions";
      return false;
    }
    for (size_t i = 0; i < n; ++i) {
      ReadLoadedCell(b.cells[i], table, &phases_[i], &radiation_[i]);
    }
    disease_ = b.disease;
    backwall_ = b.backwall;
    backwall_dirty_all_ = true;
    disease_idx_.assign(n, 0xFF);
    // The blob carries the hash and the count and nothing else, so a reload starts every
    // cell's growth remainder and infestation age from zero. That is what Klei's own save
    // does — `SaveDisease` is eight bytes there too.
    disease_accum_.assign(n, 0.0f);
    disease_infest_.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
      if (disease_[i].diseaseHash != 0) {
        disease_idx_[i] = diseases.IndexOfHash(disease_[i].diseaseHash);
      }
    }
    // Insulation, strength and properties are deliberately absent from the blob; the
    // game re-applies them as buildings load. Leaving them zeroed matches Klei.
    return true;
  }

  // A fresh game allocates the whole cluster once and then sends one `Load` per world, each
  // blob sized to its own world (`SaveLoader.LoadFromWorldGen`). `FromBlob` reallocates to the
  // blob, so the second world shrank the grid under the game's fixed `Grid.WidthInCells` and
  // `Grid.InitializeCells` read past the end of `elementIdx`.
  //
  // Klei places each blob at **its own header** `x, y` — the arguments worldgen passed to
  // `SIM_BeginSave` — and not by `DefineWorldOffsets`: two blobs saved at (0,0) land on top of
  // each other at the origin even with both worlds' offsets defined (driver/src/worldgen_test,
  // run against Klei's DLL). The padded blob goes down unshifted, so its border ring sits one
  // cell outside the world's game rectangle, inside the cluster.
  bool LoadIntoCluster(const SaveBlob& b, const ElementTable& table,
                       const DiseaseTable& diseases, std::string* error) {
    if (b.width <= 2 || b.height <= 2) {
      if (error) *error = "blob has degenerate dimensions";
      return false;
    }
    if (table.Empty()) {
      if (error) *error = "Load before Elements_CreateTable";
      return false;
    }
    const size_t n = static_cast<size_t>(b.width) * static_cast<size_t>(b.height);
    if (b.cells.size() != n || b.disease.size() != n || b.backwall.size() != n) {
      if (error) *error = "blob arrays do not match its own dimensions";
      return false;
    }
    for (int32_t ly = 0; ly < b.height; ++ly) {
      const int32_t gy = b.y + ly;
      if (gy < 0 || gy >= height_) continue;
      for (int32_t lx = 0; lx < b.width; ++lx) {
        const int32_t gx = b.x + lx;
        if (gx < 0 || gx >= width_) continue;
        const size_t li = static_cast<size_t>(ly) * static_cast<size_t>(b.width) +
                           static_cast<size_t>(lx);
        const size_t gi = static_cast<size_t>(gy) * static_cast<size_t>(width_) +
                           static_cast<size_t>(gx);
        ReadLoadedCell(b.cells[li], table, &phases_[gi], &radiation_[gi]);
        disease_[gi] = b.disease[li];
        backwall_[gi] = b.backwall[li];
        disease_idx_[gi] = 0xFF;
        disease_accum_[gi] = 0.0f;
        disease_infest_[gi] = 0;
        if (disease_[gi].diseaseHash != 0) {
          disease_idx_[gi] = diseases.IndexOfHash(disease_[gi].diseaseHash);
        }
      }
    }
    backwall_dirty_all_ = true;
    return true;
  }

  bool RadiationEnabled() const { return radiation_enabled_; }
  void SetRadiationEnabled(bool v) { radiation_enabled_ = v; }

  // The five tunables `ProcessCellRadiationChanges` writes from a `RadiationParamsModification`
  // and `SimData::SimData` seeds. The defaults are the
  // constructor's own, matched against the shipped library rather than guessed, and a world
  // that never receives the message runs on them forever.
  //
  // Type 1 is missing from Klei's `if` chain: the message can carry it and nothing happens.
  float RadiationLingerRate() const { return radiation_linger_rate_; }
  float RadiationMaxMass() const { return radiation_max_mass_; }
  float RadiationBaseWeight() const { return radiation_base_weight_; }
  float RadiationDensityWeight() const { return radiation_density_weight_; }
  float RadiationConstructedFactor() const { return radiation_constructed_factor_; }
  void SetRadiationParam(int32_t type, float v) {
    switch (type) {
      case 0: radiation_linger_rate_ = v; break;
      case 2: radiation_base_weight_ = v; break;
      case 3: radiation_density_weight_ = v; break;
      case 4: radiation_constructed_factor_ = v; break;
      case 5: radiation_max_mass_ = v; break;
      default: break;
    }
  }
  uint8_t DiseaseIdx(size_t padded) const { return disease_idx_[padded]; }
  const std::vector<uint8_t>& DiseaseIdx() const { return disease_idx_; }

  // The two per-cell disease fields the *sim* owns and the game never sees. They are the
  // `reservedAccumulatedError` and `reservedInfestationTickCount` slots of `Sim.DiseaseCell`
  // — "reserved" because the game writes zero into both and only reads them back — and
  // `SimData::ClearCell` zeroes them along with everything else.
  //
  // No `MarkStatic`: neither is projected, so a write to one owes the game nothing.
  float& MutableDiseaseAccum(size_t p) { return disease_accum_[p]; }
  const std::vector<float>& DiseaseAccum() const { return disease_accum_; }
  uint8_t& MutableDiseaseInfest(size_t p) { return disease_infest_[p]; }
  const std::vector<uint8_t>& DiseaseInfest() const { return disease_infest_; }

  // Whether any cell is infected at all. Recomputed once a frame rather than tracked per
  // write: a scan for "any byte that is not 0xFF" over the grid costs microseconds, and a
  // flag maintained by hand at every writer is a flag that goes stale at the one writer
  // nobody thought of. Every scenario in the suite but the disease ones answers false here
  // and skips the snapshot, the pair sweep and the growth sweep entirely.
  // Answered once a frame and cached, because the sweeps that ask are inside the substep
  // and region loops. Refreshing at the top of the frame is sound rather than merely cheap:
  // nothing creates disease mid-frame except a kernel moving germs that were already
  // somewhere, so a frame that starts clean stays clean.
  void RefreshDiseaseActive() { disease_active_ = AnyDisease(); }
  bool DiseaseActive() const { return disease_active_; }

  bool AnyDisease() const {
    for (uint8_t v : disease_idx_) {
      if (v != 0xFF) return true;
    }
    return false;
  }

 private:
  // Past a sixteenth of the grid the list stops being cheaper than the copy it replaces,
  // and it also stops being bounded, so it collapses to "all".
  void MarkBackwall(size_t padded) {
    if (backwall_dirty_all_) return;
    if (backwall_dirty_.size() >= PaddedCount() / 16) {
      backwall_dirty_all_ = true;
      backwall_dirty_.clear();
      return;
    }
    backwall_dirty_.push_back(static_cast<uint32_t>(padded));
  }

  void MarkStatic(size_t padded) {
    if (static_dirty_all_) return;
    if (static_dirty_.size() >= PaddedCount() / 16) {
      static_dirty_all_ = true;
      static_dirty_.clear();
      return;
    }
    static_dirty_.push_back(static_cast<uint32_t>(padded));
  }

  // Inclusive on both axes, clamped to the interior. An empty row is one whose `x1` is
  // below its `x0`, which is the state `kNoDirtyRow` and -1 leave it in.
  void MarkProjectRows(int32_t y0, int32_t y1, int32_t x0, int32_t x1) const {
    if (project_dirty_all_) return;
    if (y0 < 1) y0 = 1;
    if (x0 < 1) x0 = 1;
    if (y1 > height_ - 2) y1 = height_ - 2;
    if (x1 > width_ - 2) x1 = width_ - 2;
    if (y1 < y0 || x1 < x0) return;
    for (int32_t y = y0; y <= y1; ++y) {
      const size_t row = static_cast<size_t>(y);
      if (x0 < project_row_x0_[row]) project_row_x0_[row] = x0;
      if (x1 > project_row_x1_[row]) project_row_x1_[row] = x1;
    }
    if (y0 < project_y0_) project_y0_ = y0;
    if (y1 > project_y1_) project_y1_ = y1;
  }

  // The border ring is not a real part of the world; it exists so neighbour loops have
  // something impassable to hit.
  //
  // Klei fills it with **Neutronium at 9999 kg and 0 K, except the top row, which is
  // Vacuum** — the world is open to space at the top and sealed everywhere else. Both
  // the mass and the choice of element were recovered by saving an identically seeded
  // world from Klei's sim and from this one and diffing the blobs, which is the only way
  // to observe the ring: the game cannot address it. 9999 kg of vacuum in the top row is
  // contradictory on its face, which is precisely why it is copied rather than derived.
  void SealBorder(const ElementTable& table) {
    constexpr int32_t kNeutroniumHash = 1838482828;
    constexpr int32_t kVacuumHash = 758759285;
    const uint16_t wall = table.HasHash(kNeutroniumHash)
                              ? table.IndexOfHash(kNeutroniumHash)
                              : static_cast<uint16_t>(0);
    const uint16_t space = table.HasHash(kVacuumHash) ? table.IndexOfHash(kVacuumHash)
                                                      : static_cast<uint16_t>(0);
    // The border's backwall is Vacuum everywhere, including behind the Neutronium ring.
    const int32_t vacuum_hash = table.HasHash(kVacuumHash) ? kVacuumHash : 0;
    auto seal = [&](size_t p, uint16_t element) {
      phases_[p] = {element, 9999.0f, 0.0f};
      properties_[p] = kSolidImpermeable | kUnbreakable;
      backwall_[p] = {vacuum_hash, 0.0f, 0.0f};
    };
    // Row index height_ - 1 is the highest y, which is the top of the world.
    for (int32_t x = 0; x < width_; ++x) {
      seal(static_cast<size_t>(x), wall);
      seal(static_cast<size_t>(height_ - 1) * width_ + x, space);
    }
    for (int32_t y = 0; y < height_ - 1; ++y) {
      seal(static_cast<size_t>(y) * width_, wall);
      seal(static_cast<size_t>(y) * width_ + (width_ - 1), wall);
    }
  }

  bool allocated_ = false;
  std::vector<WorldOffset> world_offsets_, pending_world_offsets_;
  int32_t game_width_ = 0, game_height_ = 0;
  int32_t width_ = 0, height_ = 0;
  bool radiation_enabled_ = false;
  bool headless_ = false;
  float radiation_linger_rate_ = 1.1f;
  float radiation_max_mass_ = 2000.0f;
  float radiation_base_weight_ = 0.3f;
  float radiation_density_weight_ = 0.7f;
  float radiation_constructed_factor_ = 0.8f;

  // The state of the game's `rand()`, and the gas shuffle's tie-break stride. Both are set
  // once at construction and never touched by anything else; the stride starts at -1, as it
  // does in the game.
  uint32_t rng_ = 0;
  int32_t shuffle_dir_ = -1;

  std::vector<PhaseEntry> phases_;
  std::vector<float> radiation_;
  std::vector<SaveDisease> disease_;
  std::vector<SaveBackwall> backwall_;
  std::vector<uint32_t> backwall_dirty_;
  bool backwall_dirty_all_ = false;
  std::vector<uint16_t> flow_element_;
  std::vector<uint8_t> properties_;
  std::vector<uint8_t> insulation_;
  std::vector<uint8_t> strength_;
  std::vector<uint8_t> disease_idx_;
  std::vector<float> disease_accum_;
  std::vector<uint8_t> disease_infest_;
  bool disease_active_ = false;
  std::vector<uint8_t> active_;
  std::vector<uint8_t> active_incl_;
  std::vector<float> region_cosmic_;
  std::vector<PaddedRect> padded_;
  std::vector<PaddedRect> padded_incl_;
  std::vector<uint8_t> substance_touched_;
  std::vector<int32_t> cell_melted_;
  std::vector<uint8_t> stable_ticks_;
  std::vector<float> flow_;
  std::vector<FlowCell> flow_touched_;
  std::vector<Rect> regions_;
  mutable std::vector<uint32_t> static_dirty_;
  mutable bool static_dirty_all_ = true;

  // The projection's per-row dirty spans. `project_y0_ > project_y1_` means nothing is
  // marked; a marked row always has `project_row_x1_ >= project_row_x0_`.
  static constexpr int32_t kNoDirtyRow = INT32_MAX;
  mutable std::vector<int32_t> project_row_x0_;
  mutable std::vector<int32_t> project_row_x1_;
  mutable int32_t project_y0_ = 0;
  mutable int32_t project_y1_ = -1;
  mutable bool project_dirty_all_ = true;

  // Cumulative since the world was allocated, which is also when the driver takes the
  // baseline total the buckets are subtracted from.
  mutable Ledger ledger_;
};

// -------------------------------------------------- the per-region snapshot refreshes
//
// THESE ARE FUNCTIONS, NOT BRANCHES INSIDE THE SWEEPS, and this was a measured
// decision rather than a tidiness one: written inline in `StepFlow` the two-way branch cost
// that kernel 0.156 -> 0.184 ms on a SINGLE-region world — the path where the branch is not
// even taken and the code executed is the same `assign` it always was — because the extra
// body pushed the sweep past some inlining or register budget. `ONI_SNAPSHOT_NOINLINE` is the
// other half of the same finding: behind a plain call GCC inlined these straight back into the
// sweeps and the cost came back with them. The copy is a memmove over a megabyte, so there is
// nothing to gain by inlining it and a measured amount to lose.
//
// See `SnapshotReadRect` above for what the rectangle is and why the margins are what they
// are.
#if defined(__GNUC__)
#define ONI_SNAPSHOT_NOINLINE __attribute__((noinline))
#else
#define ONI_SNAPSHOT_NOINLINE
#endif

ONI_SNAPSHOT_NOINLINE inline void RefreshPhaseSnapshot(std::vector<PhaseEntry>& snap,
                                                       const std::vector<PhaseEntry>& cells,
                                                       size_t ri, int32_t pw, int32_t ph,
                                                       int32_t rx0, int32_t ry0, int32_t rx1,
                                                       int32_t ry1) {
  if (SnapshotNeedsWholeGrid(snap.size(), cells.size(), ri)) {
    snap.assign(cells.begin(), cells.end());
    return;
  }
  ForEachSnapshotRow(pw, SnapshotReadRect(pw, ph, rx0, ry0, rx1, ry1), [&](size_t b, size_t e) {
    std::copy(cells.begin() + static_cast<ptrdiff_t>(b),
              cells.begin() + static_cast<ptrdiff_t>(e),
              snap.begin() + static_cast<ptrdiff_t>(b));
  });
}

// The disease half of the same copy. Gathered a field at a time out of three separate arrays
// rather than copied, which is why it is a loop and not a `std::copy`.
ONI_SNAPSHOT_NOINLINE inline void RefreshDiseaseSnapshot(std::vector<DiseaseEntry>& snap,
                                                         const World& w, size_t ri, int32_t pw,
                                                         int32_t ph, int32_t rx0, int32_t ry0,
                                                         int32_t rx1, int32_t ry1) {
  const size_t n = w.PaddedCount();
  auto gather = [&](size_t b, size_t e) {
    for (size_t i = b; i < e; ++i) {
      snap[i].idx = w.DiseaseIdx(i);
      snap[i].count = w.Disease()[i].count;
      snap[i].infest = w.DiseaseInfest()[i];
    }
  };
  if (SnapshotNeedsWholeGrid(snap.size(), n, ri)) {
    snap.resize(n);
    gather(0, n);
    return;
  }
  ForEachSnapshotRow(pw, SnapshotReadRect(pw, ph, rx0, ry0, rx1, ry1), gather);
}

// `StepConduction`'s snapshot is temperature only — 4 bytes a cell against 12 — and its
// readers are the tile summary and the pair sweep, neither of which looks past the region
// grown by one. So this one takes its own rectangle rather than `SnapshotReadRect`'s margins,
// which exist for the displacement sweeps' much longer reach.
ONI_SNAPSHOT_NOINLINE inline void RefreshTemperatureSnapshot(std::vector<float>& snap,
                                                             const std::vector<PhaseEntry>& cells,
                                                             size_t ri, int32_t pw, int32_t ph,
                                                             int32_t rx0, int32_t ry0,
                                                             int32_t rx1, int32_t ry1) {
  const size_t n = cells.size();
  auto gather = [&](size_t b, size_t e) {
    for (size_t i = b; i < e; ++i) snap[i] = cells[i].temperature;
  };
  if (SnapshotNeedsWholeGrid(snap.size(), n, ri)) {
    snap.resize(n);
    gather(0, n);
    return;
  }
  SnapshotRect q;
  q.x0 = rx0;
  q.y0 = ry0;
  q.x1 = rx1 + 1 < pw ? rx1 + 1 : pw;
  q.y1 = ry1 + 1 < ph ? ry1 + 1 : ph;
  ForEachSnapshotRow(pw, q, gather);
}

}  // namespace oni_sim
