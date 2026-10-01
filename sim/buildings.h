// Buildings, as far as the sim is concerned.
//
// A building is not a cell. It is a rectangle of cells plus a lump of heat that lives
// outside the grid: one temperature, one heat capacity, one conductivity. Every substep it
// trades heat with each cell it covers, adds whatever its machinery is producing, and hands
// the game back a single number. That is the whole of `AddBuildingHeatExchange`, and it is
// the busiest message the game sends after the per-frame ones — 11,671 calls in four
// minutes of play.
//
// Two components:
//
//   * `BuildingHeatExchange`           building <-> the cells under it
//   * `BuildingToBuildingHeatExchange` building <-> building in contact
//
// Both run inside `SimData::UpdateComponents`, which `SimBase::UpdateData`
// calls once per substep after the fluid and disease kernels and before `PostProcessCell`.
// The components are updated in the order they sit in `SimData`, which puts cell exchange
// before building-to-building exchange.

#pragma once

#include <cstdint>
#include <vector>

#include "../abi/sim_abi.h"
#include "physics.h"
#include "world.h"

namespace oni_sim {

// The rate at which a building trades heat with anything, folded into the transfer as a
// bare multiplier, used by both components.
inline constexpr float kBuildingTransferRate = 0.005f;

// `CellSOA::GetInsulationValue` is `insulation * insulation * 1/255^2`, on the
// byte the game sends for the cell. So a normal cell (255) passes heat at full rate and an
// insulated tile attenuates by the *square* of its value. Only the cell exchange applies it;
// building-to-building exchange has no cell to read it from.
//
// The cell exchange never applies the two separately: `BuildingHeatExchange::Update` inlines
// `GetInsulationValue` and folds `kBuildingTransferRate` into its constant, so the literal in
// the DLL is the product, and the raw `insulation * insulation` is what gets
// multiplied by it. Splitting them back apart would change the rounding, so it is kept as
// the one constant Klei actually stores.
// (`kInsulationScale`, the 1/255^2 on its own, lives in `physics.h` — cell-to-cell conduction
// needs it unfolded, and this file has no use for it once the product below exists.)
inline constexpr float kCellTransfer = 7.689349956763181e-08f;

// `SimData::constructor` writes 0.001f to both scales, and `SetDebugProperties`
// overwrites them every frame with whatever the game's debug panel holds. They are not debug
// values in any useful sense: they are the constants that set the absolute rate, and the
// game sends 3,819 of those messages a session.
inline constexpr float kDefaultBuildingTemperatureScale = 0.001f;

// Every temperature the sim writes is checked against this and 0. Klei logs and refuses
// rather than clamping, which matters: a building that goes out of range keeps its old
// temperature rather than being pinned to the bound.
inline constexpr float kMaxTemperature = 10000.0f;

// A handle is `slot | (version << 24)`, and everything that indexes by handle masks first.
inline constexpr int32_t kHandleIndexMask = 0x00ffffff;

// Every clamp in this file has SSE min-then-max semantics, not a comparison — see
// `ClampSS` in world.h for why that is a different function. It matters at three of the sites
// below, all of them a 0/0 that Klei lands on `hi` and a comparison clamp would keep as a NaN:
// `ApplyBuildingEnergy` on a building whose `CellCount() * per_cell_heat_capacity` is zero
// (applied at the start of each frame), the building's own average at the
// end of the cell sweep when its extents are empty, and
// `BuildingToBuildingHeatExchange`'s `1/self_hc`, which — unlike the `other_hc`
// on the far side of the same loop — has no positive guard in front of it.
inline float Clamp(float v, float lo, float hi) { return ClampSS(v, lo, hi); }
inline float Abs(float v) { return v < 0.0f ? -v : v; }

// Klei's `BuildingHeatExchangeData`, 44 bytes, built by `InitializeFromMessageData`.
// Three of its eleven fields are derived rather than sent, and the derivation
// is the interesting part:
//
//   * `heat_capacity` is `mass * element.specificHeatCapacity`, the whole building's.
//   * `per_cell_heat_capacity` is that divided by the cell count, and it is the one the
//     per-cell exchange actually uses — each cell trades against a building of one n'th the
//     size, and the n results are averaged back at the end. Building-to-building exchange
//     uses the undivided one instead.
//   * `thermal_conductivity` is the message's value *times* the element's, so a building's
//     conductivity is a multiplier on its material rather than a replacement for it.
//
// The extents are stored **padded** — the constructor adds one to each of minX/minY/maxX/maxY
// — so they index our storage directly, and the region test compares padded to padded.
struct BuildingHeatExchangeData {
  float temperature = 0.0f;
  float overheat_temperature = 0.0f;
  float operating_kilowatts = 0.0f;
  float heat_capacity = 0.0f;
  float per_cell_heat_capacity = 0.0f;
  float thermal_conductivity = 0.0f;
  // The element's high transition temperature. A building
  // does not melt at its overheat temperature, it melts at its material's.
  float melt_temperature = 0.0f;
  int32_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;

  int32_t CellCount() const { return (max_x - min_x) * (max_y - min_y); }
};

// A building this one is touching, and how many cells they share. `cells_in_contact` is
// stored by `BuildingToBuildingHeatExchange::Add` and then **never read** — the exchange
// rate does not depend on how much of the two buildings touch. Kept because the game sends
// it and a future build may want it.
struct InContactBuilding {
  int32_t handle = -1;
  int32_t cells_in_contact = 0;
};

struct BuildingToBuildingData {
  int32_t self = -1;
  std::vector<InContactBuilding> contacts;
};

// Klei's `CompactedVector`: a dense array plus a handle indirection, so iteration is linear
// and handles survive removals. A handle is `index | (version << 24)`; the version byte is
// bumped on release so a stale handle is detected rather than silently aliasing a new object.
//
// Removal is a swap with the last element, which is what "compacted" means and which decides
// **iteration order** — after a building is removed the last one takes its place, so update
// order is not registration order. That is observable, so it is copied rather than tidied.
template <typename T>
class CompactedVector {
 public:
  int32_t Add(const T& value) {
    int32_t handle;
    if (!free_.empty()) {
      handle = free_.back();
      free_.pop_back();
      index_[static_cast<size_t>(handle & kHandleIndexMask)] =
          static_cast<int32_t>(data_.size());
    } else {
      handle = static_cast<int32_t>(index_.size()) & kHandleIndexMask;
      index_.push_back(static_cast<int32_t>(data_.size()));
      version_.push_back(0);
    }
    data_.push_back(value);
    handles_.push_back(handle);
    return handle;
  }

  bool Valid(int32_t handle) const {
    if (handle < 0) return false;
    const size_t slot = static_cast<size_t>(handle & kHandleIndexMask);
    if (slot >= index_.size()) return false;
    if (version_[slot] != static_cast<uint8_t>(handle >> 24)) return false;
    return index_[slot] >= 0;
  }

  T* Get(int32_t handle) {
    if (!Valid(handle)) return nullptr;
    return &data_[static_cast<size_t>(index_[static_cast<size_t>(handle & kHandleIndexMask)])];
  }
  const T* Get(int32_t handle) const {
    return const_cast<CompactedVector*>(this)->Get(handle);
  }

  void Remove(int32_t handle) {
    if (!Valid(handle)) return;
    const size_t slot = static_cast<size_t>(handle & kHandleIndexMask);
    const size_t at = static_cast<size_t>(index_[slot]);
    const size_t last = data_.size() - 1;
    if (at != last) {
      data_[at] = data_[last];
      handles_[at] = handles_[last];
      index_[static_cast<size_t>(handles_[at] & kHandleIndexMask)] = static_cast<int32_t>(at);
    }
    data_.pop_back();
    handles_.pop_back();
    index_[slot] = -1;
    version_[slot] = static_cast<uint8_t>(version_[slot] + 1);
    free_.push_back(static_cast<int32_t>(slot) |
                    (static_cast<int32_t>(version_[slot]) << 24));
  }

  void Clear() {
    data_.clear();
    handles_.clear();
    index_.clear();
    version_.clear();
    free_.clear();
  }

  std::vector<T>& Data() { return data_; }
  const std::vector<T>& Data() const { return data_; }
  const std::vector<int32_t>& Handles() const { return handles_; }
  size_t SlotCount() const { return index_.size(); }

 private:
  std::vector<T> data_;
  std::vector<int32_t> handles_;   // parallel to data_
  std::vector<int32_t> index_;     // handle slot -> index into data_, -1 if free
  std::vector<uint8_t> version_;   // handle slot -> current version byte
  std::vector<int32_t> free_;      // released handles, already version-bumped, LIFO
};

// Everything the two components hand back to the game in one frame.
//
// The three int lists are `MeltedInfo`, which is a bare handle. `temperatures` is indexed by
// handle slot rather than appended to, because Klei resizes it to the slot count at the top
// of every update and writes each building into its own slot — a building that was skipped
// this substep keeps the value it had, which is what makes a skipped building read as
// "unchanged" rather than as absent.
struct BuildingEvents {
  std::vector<BuildingTemperatureInfo> temperatures;
  std::vector<int32_t> overheated;
  std::vector<int32_t> no_longer_overheated;
  std::vector<int32_t> melted;

  void ClearFrame() {
    overheated.clear();
    no_longer_overheated.clear();
    melted.clear();
  }
};

// The whole of a building's state outside the grid.
struct BuildingState {
  CompactedVector<BuildingHeatExchangeData> exchange;
  CompactedVector<BuildingToBuildingData> contact;
  BuildingEvents events;
  // `SetDebugProperties`, which despite the name arrives every single frame.
  float temperature_scale = kDefaultBuildingTemperatureScale;
  float to_building_temperature_scale = kDefaultBuildingTemperatureScale;

  void Clear() {
    exchange.Clear();
    contact.Clear();
    events = BuildingEvents{};
  }
};

// --------------------------------------------------------------------- registration

// `BuildingHeatExchange::Register` / `InitializeFromMessageData`. The message is the same 44
// bytes for Add and Modify; only the first field differs in meaning (a callback index on the
// way in, the handle to modify on the way back).
inline BuildingHeatExchangeData BuildingDataFromMessage(
    const ElementTable& table, uint16_t elem_idx, float mass, float temperature,
    float thermal_conductivity, float overheat_temperature, float operating_kilowatts,
    int32_t min_x, int32_t min_y, int32_t max_x, int32_t max_y) {
  const Element& e = table.At(elem_idx);
  BuildingHeatExchangeData d;
  d.temperature = temperature;
  d.overheat_temperature = overheat_temperature;
  d.operating_kilowatts = operating_kilowatts;
  d.heat_capacity = e.specificHeatCapacity * mass;
  const int32_t cells = (max_x - min_x) * (max_y - min_y);
  d.per_cell_heat_capacity =
      d.heat_capacity / static_cast<float>(cells > 0 ? cells : 1);
  d.thermal_conductivity = e.thermalConductivity * thermal_conductivity;
  d.melt_temperature = e.highTemp;
  // Padded, so the cell loop indexes storage directly.
  d.min_x = min_x + 1;
  d.min_y = min_y + 1;
  d.max_x = max_x + 1;
  d.max_y = max_y + 1;
  return d;
}

// `BuildingHeatExchange::Modify`. The replacement is wholesale — the message
// carries a fresh temperature too, so the game can teleport a building's heat — and the only
// thing carried over from the old record is one event: a building that *was* over its
// overheat temperature and is not any more says so.
inline void ModifyBuilding(BuildingState* state, int32_t handle,
                           const BuildingHeatExchangeData& next) {
  BuildingHeatExchangeData* d = state->exchange.Get(handle);
  if (!d) return;
  if (d->overheat_temperature <= d->temperature &&
      next.temperature < d->overheat_temperature) {
    state->events.no_longer_overheated.push_back(handle);
  }
  *d = next;
}

// `ModifyBuildingEnergy`, applied where the frame's messages are drained
// rather than inside either component's update. It is how the game injects a discrete lump
// of heat with its own bounds — a duplicant warming a bed, a dock charging a suit.
//
// The bounds widen to include the building's current temperature rather than clamping it,
// and a result outside 0..10000 K is **refused rather than clamped**: Klei logs it and
// leaves the building where it was.
inline void ApplyBuildingEnergy(BuildingHeatExchangeData* d, float delta_kj,
                                float min_temperature, float max_temperature) {
  const float lo = min_temperature < d->temperature ? min_temperature : d->temperature;
  const float hi = max_temperature > d->temperature ? max_temperature : d->temperature;
  if (lo < 0.0f || hi > kMaxTemperature) return;
  const float total_hc = static_cast<float>(d->CellCount()) * d->per_cell_heat_capacity;
  const float next = Clamp(d->temperature + delta_kj / total_hc, lo, hi);
  if (next > 0.0f && next < kMaxTemperature) d->temperature = next;
}

// --------------------------------------------------------------- the cell exchange

// `BuildingHeatExchange::Update`, step for step.
//
// Per cell under the building:
//
//   Q = srcHC * (T_cell - T_building) * (k_elem * k_building) * insulation
//       * kBuildingTransferRate * dt * scale
//
// where `srcHC` is the heat capacity of whichever side is *hotter* — the cell's mass times
// its specific heat if the cell is warmer, the building's per-cell capacity if the building
// is. That asymmetry is not a symmetry bug on Klei's part: it makes a hot building shed heat
// into a thin gas at the gas's rate and a hot gas heat the building at the building's, which
// is why machines in vacuum barely cool.
//
// Both sides then move by Q over their own heat capacity, each clamped into the interval the
// two temperatures already spanned. If the clamps would leave them crossed over — the
// building ending up hotter than the cell it was heating — the pair is put at their
// mass-weighted equilibrium instead and Q is recomputed from where the cell actually landed.
// So a single substep can equalise a pair outright but never overshoot it.
//
// The building's own temperature moves by the *sum* of those energies over `n * per-cell
// capacity`, clamped to the min and max temperature seen anywhere in the sweep, and only
// then does `operating_kilowatts * dt` go in — outside the clamp, which is what lets a
// machine heat itself above everything around it.
inline void StepBuildingHeatExchange(World* w, const ElementTable& table,
                                     BuildingState* state, float dt,
                                     std::vector<StateChangeOre>* ores, size_t ri,
                                     const uint8_t* visible = nullptr,
                                     bool debug_editing = false,
                                     std::vector<SpawnFallingLiquidInfo>* falling = nullptr) {
  BuildingEvents& ev = state->events;
  // `SimData::UpdateComponents` sits inside `UpdateData`'s region loop, so
  // this runs once per region and a building inside two of them exchanges twice. The
  // temperature report is indexed by slot rather than appended, so it is cleared on the
  // first region only and a building reached twice reports the second result.
  if (ri == 0) ev.temperatures.assign(state->exchange.SlotCount(), BuildingTemperatureInfo{});

  const int32_t pw = w->PaddedWidth();
  std::vector<PhaseEntry>& cells = w->Phases();
  const std::vector<uint8_t>& insulation = w->Insulation();
  std::vector<BuildingHeatExchangeData>& all = state->exchange.Data();
  const std::vector<int32_t>& handles = state->exchange.Handles();

  for (size_t b = 0; b < all.size(); ++b) {
    BuildingHeatExchangeData& d = all[b];
    const int32_t handle = handles[b];

    // Extents against the active region, not cells against it. A building hanging over the
    // edge is skipped whole and does not even report a temperature.
    if (!w->ExtentsInRegion(ri, d.min_x - 1, d.min_y - 1, d.max_x - 1, d.max_y - 1)) {
      continue;
    }

    const float t_building = d.temperature;
    if (d.per_cell_heat_capacity > 0.0f && t_building >= 0.0f) {
      const float building_hc = d.per_cell_heat_capacity;
      float accumulated = 0.0f;
      float seen_min = t_building, seen_max = t_building;

      for (int32_t y = d.min_y; y < d.max_y; ++y) {
        for (int32_t x = d.min_x; x < d.max_x; ++x) {
          const size_t i = static_cast<size_t>(y) * pw + x;
          PhaseEntry& c = cells[i];
          if (c.mass <= 0.0f) continue;
          const Element& e = table.At(c.element);

          const float ins = static_cast<float>(insulation[i]);
          const float insulation_factor = ins * ins;
          const float cell_hc = c.mass * e.specificHeatCapacity;
          // NEUTRONIUM. `Unobtanium` ships `specificHeatCapacity: 0`, so a 2000 kg tile of it
          // passes the mass guard above with a heat capacity of exactly zero, and the
          // `t_cell - q / cell_hc` below is a 0/0. That poisons the CELL with NaN; on the next
          // sweep its temperature is NaN, `delta` is NaN, `source_hc` takes the building's
          // branch, `accumulated` is NaN, and the BUILDING follows. Worse than the unreadable
          // number: `NaN > overheat_temperature` is false, so every temperature gate on that
          // building then fails silently and nothing reports it.
          //
          // Skipping is the physical answer as well as the safe one. A cell that can hold no
          // heat can neither give nor take any, so the correct energy transfer is exactly the
          // zero this `continue` produces -- and neutronium's thermal conductivity is 0 as
          // well, so `q` was already going to be zero on the branch that does not divide.
          //
          // This is NOT a general float-safety guard bolted onto Klei's arithmetic, and it is
          // deliberately written as the narrow one. Every element in the game except this one
          // has a nonzero specific heat, so for every cell a player can ever mine, build or
          // create this test cannot fire and the sweep below is byte-identical to Klei's --
          // which is why it is allowed into this repo at all.
          //
          // It has to exist because neutronium does. The player cannot mine or make it, but
          // the world is full of it: it closes off the sides and the bottom of the asteroid
          // and it is the floor geysers sit on. Anything with a structure temperature that
          // ends up against one of those tiles -- directly, or through an `OverrideExtents`
          // that reaches a row below itself, as the Steam Turbine does -- hits
          // this. The material is left alone and this clause carries it.
          if (!(cell_hc > 0.0f)) continue;
          const float t_cell = c.temperature;
          const float delta = t_cell - t_building;

          // The multiply order is Klei's, not algebra's. Every one of these is float32 and
          // reassociating them moves the last bits, which is the difference between a
          // scenario that scores zero and one that scores 1e-5 and has to be argued about.
          const float source_hc = delta >= 0.0f ? cell_hc : building_hc;
          float q = source_hc * delta;
          q *= e.thermalConductivity * d.thermal_conductivity;
          q *= insulation_factor;
          q *= kCellTransfer;
          q *= dt * state->temperature_scale;

          const float hi = t_building > t_cell ? t_building : t_cell;
          const float lo = t_building < t_cell ? t_building : t_cell;
          if (hi > seen_max) seen_max = hi;
          if (lo < seen_min) seen_min = lo;

          float new_cell = Clamp(t_cell - q / cell_hc, lo, hi);
          const float new_building = Clamp(t_building + q / building_hc, lo, hi);
          // NOT `< 0.0f`: in the game an unordered compare takes the equilibrium branch. Spelled as
          // the negation of `>= 0` so a NaN product lands on that branch the way it does in
          // the DLL, instead of falling through and leaving the NaN in the cell.
          if (!((new_building - new_cell) * (t_building - t_cell) >= 0.0f)) {
            // The two would have crossed. Put them where they were always going to end up.
            // Klei divides once and scales each side, rather than dividing the sum.
            const float inv_sum = 1.0f / (building_hc + cell_hc);
            const float equilibrium =
                Clamp(building_hc * inv_sum * t_building + inv_sum * cell_hc * t_cell, lo,
                      hi);
            new_cell = equilibrium;
            q = (equilibrium - t_building) * building_hc;
          }

          c.temperature = new_cell;
          // Klei runs the transition test on the cell here, inside the sweep, so a cell a
          // building has just boiled is already the new element before the next building
          // reads it.
          TransitionCell(w, table, x, y, ores, visible, debug_editing, falling);
          accumulated += q;
        }
      }

      const float n = static_cast<float>(d.CellCount());
      const float total_hc = n * building_hc;
      float next = Clamp(t_building + accumulated / total_hc, seen_min, seen_max);
      // Operating heat is added *after* the clamp, so a machine can drive itself past
      // everything it touches. This is where an overheating aquatuner comes from.
      next += (dt * d.operating_kilowatts) / total_hc;

      if (next >= d.overheat_temperature) ev.overheated.push_back(handle);
      if (t_building >= d.overheat_temperature && next < d.overheat_temperature) {
        ev.no_longer_overheated.push_back(handle);
      }
      if (next >= d.melt_temperature) ev.melted.push_back(handle);
      d.temperature = next;
    }

    const size_t slot = static_cast<size_t>(handle & kHandleIndexMask);
    if (slot < ev.temperatures.size()) {
      ev.temperatures[slot].handle = handle;
      ev.temperatures[slot].temperature = d.temperature;
    }
  }
}

// --------------------------------------------------- the building-to-building exchange

// `BuildingToBuildingHeatExchange::Update`.
//
// The same shape as the cell exchange with three differences, all of them load-bearing:
//
//   * it uses the **whole** building's heat capacity on both sides, not the per-cell one;
//   * there is no insulation term, because there is no cell to read one from;
//   * the clamp interval is computed once from *every* building in the group, in a first
//     pass, rather than per pair — so a chain of touching buildings cannot ping-pong heat
//     past the coldest and hottest members of the chain.
//
// The energy is also clamped by how far *both* sides could actually move, `min` of the two,
// with the sign put back afterwards. `cells_in_contact` plays no part.
inline void StepBuildingToBuilding(World* w, BuildingState* state, float dt, size_t ri) {
  BuildingEvents& ev = state->events;
  std::vector<BuildingToBuildingData>& groups = state->contact.Data();

  for (BuildingToBuildingData& g : groups) {
    BuildingHeatExchangeData* self = state->exchange.Get(g.self);
    if (!self) continue;
    if (!w->ExtentsInRegion(ri, self->min_x - 1, self->min_y - 1, self->max_x - 1,
                            self->max_y - 1)) {
      continue;
    }

    float seen_min = self->temperature, seen_max = self->temperature;
    for (const InContactBuilding& c : g.contacts) {
      const BuildingHeatExchangeData* other = state->exchange.Get(c.handle);
      if (!other) continue;
      if (other->temperature < seen_min) seen_min = other->temperature;
      if (other->temperature > seen_max) seen_max = other->temperature;
    }

    const float self_hc = self->heat_capacity;
    for (const InContactBuilding& c : g.contacts) {
      BuildingHeatExchangeData* other = state->exchange.Get(c.handle);
      if (!other) continue;
      const float other_hc = other->heat_capacity;
      if (other_hc <= 0.0f) continue;

      const float t_other = other->temperature;
      const float t_self = self->temperature;
      const float delta = t_other - t_self;
      const float source_hc = delta >= 0.0f ? other_hc : self_hc;
      float q = other->thermal_conductivity * self->thermal_conductivity;
      q *= delta * source_hc;
      q = (dt * state->to_building_temperature_scale) * q;
      q *= kBuildingTransferRate;

      const float inv_self = 1.0f / self_hc;
      const float inv_other = 1.0f / other_hc;
      const float self_room =
          Abs(Clamp(t_self + inv_self * q, seen_min, seen_max) - t_self) * self_hc;
      const float other_room =
          Abs(Clamp(t_other - inv_other * q, seen_min, seen_max) - t_other) * other_hc;
      // SSE min with the OTHER side's room first, so a NaN yields `self_room`. The operands
      // are in the game's order rather than the readable one for exactly that reason.
      float energy = MinSS(other_room, self_room);
      if (q < 0.0f) energy = -energy;

      float new_self = t_self + inv_self * energy;
      float new_other = t_other - inv_other * energy;
      // The unordered-compare case again, and this is the branch that decides what a
      // group owner with no heat capacity does. `1 / self_hc` is an infinity, `q` is zero
      // because `source_hc` is that same zero, and `inf * 0` makes `new_self` a NaN; the
      // unordered compare then sends the pair here and both ends come out at the weighted
      // equilibrium, which is the other building's temperature. Written `< 0.0f` the branch
      // is skipped and the NaN is published — `diffsim --scenario b2bzerohc`, klei 300.00000
      // against mine `nan`.
      if (!((new_self - new_other) * (t_self - t_other) >= 0.0f)) {
        const float equilibrium =
            Clamp((t_other * other_hc + t_self * self_hc) / (other_hc + self_hc), seen_min,
                  seen_max);
        new_self = equilibrium;
        new_other = equilibrium;
      }

      if (new_other >= other->overheat_temperature) ev.overheated.push_back(c.handle);
      if (other->overheat_temperature <= t_other &&
          new_other < other->overheat_temperature) {
        ev.no_longer_overheated.push_back(c.handle);
      }
      other->temperature = new_other;
      self->temperature = new_self;

      // Both sides publish immediately rather than at the end of the sweep, because a
      // building may appear in several contact groups in the same substep.
      auto publish = [&](int32_t handle, float temperature) {
        const size_t slot =
            static_cast<size_t>(handle & kHandleIndexMask);
        if (slot < ev.temperatures.size()) {
          ev.temperatures[slot].handle = handle;
          ev.temperatures[slot].temperature = temperature;
        }
      };
      publish(c.handle, new_other);
      publish(g.self, new_self);
    }
  }
}

}  // namespace oni_sim
