// ConduitTemperatureManager.
//
// The contents of a pipe are not cells. They live in the game's own `ConduitFlow` grid and
// the sim never sees them move; what the sim owns is one number per conduit — the
// temperature of whatever is inside it — and one job: exchange heat between that number and
// the *building* the conduit is, then tell the game when the contents froze or boiled.
//
// So this file has no world, no grid and no neighbours. It is a flat list of independent
// two-body problems, driven once per 200 ms from the game thread rather than from the sim
// frame, and it talks to the rest of the sim through exactly one message
// (`ModifyBuildingEnergy`) — the same message a duplicant warming a bed sends. That is the
// whole coupling: the conduit reads the building temperature the last frame published and
// pays back the energy it took as a message the next frame drains.
//
// Everything below follows the game's arithmetic exactly rather than a reasoned-out version,
// because the arithmetic is unusual in two ways that a plausible-looking rewrite gets wrong:
//
//   * The exchange is limited by *both* sides independently and then the smaller limit wins
//     (`min(limB, limC)`), rather than one clamp on the result.
//   * When the transfer would still cross the two temperatures over each other, the pair is
//     dropped onto its exact equilibrium instead. Not clamped to it — computed.
//
// Verification note: no `diffsim` scenario can reach any of this. The suite drives the cell
// grid, and conduit contents are a separate world that only the game populates. What can be
// checked offline is the two-body arithmetic against hand-worked cases and the handle
// bookkeeping against `CompactedVector`; what cannot be checked without the game is whether
// the game's own conduit flow agrees about which handle is which.

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "../abi/sim_abi.h"
#include "buildings.h"
#include "world.h"

namespace oni_sim {

// `MinSS`/`MaxSS` live in `world.h` now — this file was where they were first needed (conduit
// temperatures do go NaN in practice, a zero heat capacity divides); the definition and the
// explanation live in the one header everything includes.

// The exchange rate. Klei folds the pipe's contact area into a single constant
// rather than deriving it from the building's footprint the way cell-to-building exchange
// does, so a conduit is the same size to the thermal model no matter what building holds it.
inline constexpr float kConduitContactArea = 50.0f;
// Dt arrives in seconds and the conductivities are per-kilosecond.
inline constexpr float kConduitTransferRate = 0.001f;
// A conduit does not freeze at its contents' transition
// temperature, it freezes three degrees past it. The margin is what stops a pipe sitting
// exactly on the boundary from reporting a phase change every single update.
inline constexpr float kConduitTransitionMargin = 3.0f;
// A heat capacity at or below this is treated as "no body here"
// and the pair is skipped, and an energy transfer at or below the other is not worth a
// message.
inline constexpr float kMinConduitHeatCapacity = 1e-4f;
inline constexpr float kMinConduitEnergy = 1e-6f;
// Handles are `slot | (version << 24)`; a slot this large is a corrupt or
// released handle rather than a real one, and the entry is skipped without even writing an
// output temperature for it.
inline constexpr int32_t kMaxConduitSlot = 0x100000;

// `ConduitTemperatureManager::Data`, 36 bytes, laid out exactly as Klei's. Nothing outside
// this file depends on the layout — the game never sees it.
struct ConduitTemperatureData {
  float temperature = 0.0f;              // +0x00, the contents
  float contents_conductivity = 0.0f;    // +0x04, the element's, unscaled
  float contents_heat_capacity = 0.0f;   // +0x08, mass * the element's specific heat
  int32_t structure_handle = -1;         // +0x0c, into the building temperature array
  float conduit_heat_capacity = 0.0f;    // +0x10, the pipe's own, sent by the game
  float conduit_conductivity = 0.0f;     // +0x14, likewise
  uint8_t insulated = 0;                 // +0x18, `def.ThermalConductivity < 1`
  float low_temp = 0.0f;                 // +0x1c, 0 when the element has no low transition
  float high_temp = 0.0f;                // +0x20, FLT_MAX when it has no high transition
};

// What `ConduitTemperatureManager_Update` hands back. The game reads it through a pointer
// that stays valid until the next call, so the three arrays are members rather than locals.
#pragma pack(push, 4)
struct ConduitTemperatureUpdateData {
  int32_t numEntries = 0;
  float* temperatures = nullptr;
  int32_t numFrozenHandles = 0;
  int32_t* frozenHandles = nullptr;
  int32_t numMeltedHandles = 0;
  int32_t* meltedHandles = nullptr;
};
#pragma pack(pop)

// The energy the contents took from the building has to go back as a message rather than as
// a direct write, because `Update` runs on the game thread between sim frames while the
// building temperatures belong to the sim. Klei calls `SimFrameManager::HandleMessage`,
// which enqueues; so does this, through the caller's sink.
using ConduitEnergySink = void (*)(void* ctx, const ModifyBuildingEnergyMessage& msg);

class ConduitTemperatures {
 public:
  // The element is sent as a *hash*, not an index, and is resolved here once —
  // the conduit keeps the element's numbers, not its identity, so an element table reload
  // does not reach conduits already registered.
  int32_t Add(const ElementTable& table, float temperature, float mass, int32_t element_hash,
              int32_t structure_handle, float conduit_heat_capacity,
              float conduit_conductivity, bool insulated) {
    const Element& e = table.At(table.IndexOfHash(element_hash));
    ConduitTemperatureData d;
    // A temperature past the sim's ceiling is refused and replaced with the
    // element's default rather than clamped, so a corrupt save loads cold instead of
    // exploding. Klei logs it; there is nothing to log to here.
    d.temperature = temperature > kMaxTemperature ? e.defaultValues.temperature : temperature;
    d.contents_conductivity = e.thermalConductivity;
    d.contents_heat_capacity = mass * e.specificHeatCapacity;
    d.structure_handle = structure_handle;
    d.conduit_heat_capacity = conduit_heat_capacity;
    d.conduit_conductivity = conduit_conductivity;
    d.insulated = insulated ? 1 : 0;
    SetTransitions(&d, e);
    return vec_.Add(d);
  }

  // Only the four contents fields move; the conduit's own heat capacity and
  // conductivity were fixed when the pipe was built and are deliberately not re-sent.
  void Set(const ElementTable& table, int32_t handle, float temperature, float mass,
           int32_t element_hash) {
    ConduitTemperatureData* d = vec_.Get(handle);
    if (!d) return;
    const Element& e = table.At(table.IndexOfHash(element_hash));
    d->temperature = temperature > kMaxTemperature ? e.defaultValues.temperature : temperature;
    d->contents_heat_capacity = mass * e.specificHeatCapacity;
    d->contents_conductivity = e.thermalConductivity;
    SetTransitions(d, e);
  }

  // Removal does not free the slot. It neuters the entry — a negative conduit
  // heat capacity fails the `kMinConduitHeatCapacity` test in `Update`, so the pair is
  // skipped from the next update on — and queues the handle for release a frame later.
  //
  // The delay is not caution about threads. The game holds handles across a frame boundary
  // and would otherwise be able to see a slot recycled underneath a handle it is still
  // carrying; the version byte would catch that, but the temperature array it indexes by
  // slot would already have the new conduit's value in it.
  void Remove(int32_t handle) {
    if (ConduitTemperatureData* d = vec_.Get(handle)) {
      d->conduit_heat_capacity = -1.0f;
      d->conduit_conductivity = -1.0f;
      d->structure_handle = -1;
    }
    release_queue_[release_parity_].push_back(handle);
  }

  void Clear() {
    vec_.Clear();
    out_temperatures_.clear();
    frozen_.clear();
    melted_.clear();
    release_queue_[0].clear();
    release_queue_[1].clear();
    release_parity_ = 0;
    update_ = ConduitTemperatureUpdateData{};
  }

  // Called once per sim frame from `Sim::Main`. The parity flips
  // first and the buffer that comes up is the one filled *before* the previous flip, which
  // is where the one frame of delay comes from.
  void ReleaseQueuedHandles() {
    release_parity_ = (release_parity_ + 1) & 1;
    std::vector<int32_t>& due = release_queue_[release_parity_];
    for (int32_t handle : due) vec_.Remove(handle);
    due.clear();
  }

  const ConduitTemperatureUpdateData* Update(float dt,
                                             const BuildingTemperatureInfo* building_temperatures,
                                             ConduitEnergySink sink, void* sink_ctx) {
    // Indexed by handle slot, not by dense position, and only ever grown: a conduit that is
    // skipped this update keeps the temperature it had rather than reading as absent.
    out_temperatures_.resize(vec_.SlotCount(), 0.0f);
    frozen_.clear();
    melted_.clear();

    std::vector<ConduitTemperatureData>& data = vec_.Data();
    const std::vector<int32_t>& handles = vec_.Handles();

    for (size_t i = 0; i < data.size(); ++i) {
      ConduitTemperatureData& d = data[i];
      const size_t out = static_cast<size_t>(handles[i] & kHandleIndexMask);
      if (out >= out_temperatures_.size()) continue;

      // No building temperatures at all means the game has not published a frame yet. Klei
      // still walks the list and copies every temperature straight through, so the array the
      // game reads is populated on the very first update rather than left at zero.
      if (!building_temperatures) {
        out_temperatures_[out] = d.temperature;
        continue;
      }

      const int32_t slot = d.structure_handle & kHandleIndexMask;
      // A dead or corrupt structure handle writes *nothing*, not even the unchanged
      // temperature — the one path in this loop that leaves the output slot alone.
      if (slot >= kMaxConduitSlot) continue;

      // Either body missing, or a building the sim has never warmed, and the pair is skipped
      // with its temperature copied through unchanged.
      //
      // These are spelled as `constant >= field` rather than `field <= constant` on purpose:
      // that is the direction the game compares in, and it decides what a NaN heat capacity
      // does. An unordered compare does *not* skip the pair —
      // the reverse of what the natural spelling would do.
      if (kMinConduitHeatCapacity >= d.contents_heat_capacity) {
        out_temperatures_[out] = d.temperature;
        continue;
      }
      if (kMinConduitHeatCapacity >= d.conduit_heat_capacity) {
        out_temperatures_[out] = d.temperature;
        continue;
      }
      const float building_t = building_temperatures[slot].temperature;
      // An unordered compare skips, so a NaN building temperature skips. Written as the
      // negation of `>` to keep that.
      if (!(building_t > 0.0f)) {
        out_temperatures_[out] = d.temperature;
        continue;
      }

      const float contents_t = d.temperature;
      const float hc_contents = d.contents_heat_capacity;
      const float hc_conduit = d.conduit_heat_capacity;

      // Insulated pipes take the *minimum* of the two conductivities, plain
      // ones take the mean. Same shape as cell insulation in `physics.h`: below the
      // threshold the formula changes shape rather than scale, so an insulated pipe is held
      // to its weaker side instead of being slowed by a factor.
      const float k = d.insulated
                          ? MinSS(d.contents_conductivity, d.conduit_conductivity)
                          : (d.contents_conductivity + d.conduit_conductivity) * 0.5f;
      const float rate = k * (contents_t - building_t) * kConduitContactArea;
      const float energy = rate * dt * kConduitTransferRate;

      const float lo = MinSS(contents_t, building_t);
      const float hi = MaxSS(contents_t, building_t);
      const float inv_contents = 1.0f / hc_contents;
      const float inv_conduit = 1.0f / hc_conduit;

      // How much energy each side could give up or take before it would pass the other one.
      // Both are computed from the *same* proposed transfer and the smaller wins, which is
      // what makes a small pipe against a huge building move by the pipe's limit and not by
      // some average of the two.
      float reach_contents = contents_t - inv_contents * energy;
      reach_contents = MinSS(reach_contents, hi);
      reach_contents = MaxSS(reach_contents, lo);
      const float limit_contents = std::fabs(reach_contents - contents_t) * hc_contents;

      float reach_conduit = building_t + inv_conduit * energy;
      reach_conduit = MinSS(reach_conduit, hi);
      reach_conduit = MaxSS(reach_conduit, lo);
      const float limit_conduit = std::fabs(reach_conduit - building_t) * hc_conduit;

      // The sign comes from the *unlimited* rate, not from either limit, because both limits
      // went through `fabs`. NaN takes the negative branch, as in the game.
      const float sign = rate >= 0.0f ? 1.0f : -1.0f;
      float transferred = MinSS(limit_conduit, limit_contents) * sign;
      const float delta = -transferred;

      float next = MaxSS(contents_t + inv_contents * delta, 0.0f);
      const float next_building = MaxSS(building_t - inv_conduit * delta, 0.0f);

      // If the two ends came out on opposite sides of where they started, the
      // limits above did not hold and the honest answer is the temperature the pair would
      // reach with all the energy shared. Klei computes it rather than clamping to one side,
      // so an overshooting pair lands on its equilibrium in one update instead of ringing.
      // An unordered compare runs the equilibrium branch, so a NaN product runs it rather
      // than falling through it.
      if (!((next - next_building) * (contents_t - building_t) >= 0.0f)) {
        const float inv_total = 1.0f / (hc_contents + hc_conduit);
        next = inv_total * hc_contents * contents_t + inv_total * hc_conduit * building_t;
      }

      // `_fdtest`, testing for NaN only — an infinity is allowed through. A
      // NaN result is refused rather than corrected: the conduit keeps the temperature it
      // had and, because the transfer is zeroed too, no energy is billed to the building.
      if (next != next) {
        next = d.temperature;
        transferred = 0.0f;
      } else {
        d.temperature = next;
      }
      out_temperatures_[out] = next;

      // The building is paid in energy, not in temperature, and the bounds ride along so the
      // building's own handler can refuse a result outside them. `min`/`max` are the two
      // starting temperatures, so the building can never be pushed past where the contents
      // began — the same envelope the limits above enforced on this side.
      if (sink && std::fabs(transferred) > kMinConduitEnergy) {
        ModifyBuildingEnergyMessage msg{};
        msg.handle = d.structure_handle;
        msg.deltaKJ = (contents_t - next) * hc_contents;
        msg.minTemperature = lo;
        msg.maxTemperature = hi;
        sink(sink_ctx, msg);
      }

      // Reported as handles, with the version byte still on, because the game looks the
      // conduit up in its own table and wants a stale handle to fail rather than alias.
      if (next < d.low_temp - kConduitTransitionMargin) {
        frozen_.push_back(handles[i]);
      } else if (next > d.high_temp + kConduitTransitionMargin) {
        melted_.push_back(handles[i]);
      }
    }

    update_.numEntries = static_cast<int32_t>(out_temperatures_.size());
    update_.temperatures = out_temperatures_.empty() ? nullptr : out_temperatures_.data();
    update_.numFrozenHandles = static_cast<int32_t>(frozen_.size());
    update_.frozenHandles = frozen_.empty() ? nullptr : frozen_.data();
    update_.numMeltedHandles = static_cast<int32_t>(melted_.size());
    update_.meltedHandles = melted_.empty() ? nullptr : melted_.data();
    return &update_;
  }

 private:
  // An element with no transition on a side cannot leave on
  // that side, and the sentinels say so in the units the test uses: 0 K, which nothing
  // reaches, and FLT_MAX, which nothing exceeds. Storing the transition temperatures rather
  // than the element index is what makes the update loop free of table lookups.
  static void SetTransitions(ConduitTemperatureData* d, const Element& e) {
    d->low_temp = e.lowTempTransitionIdx == 0xFFFF ? 0.0f : e.lowTemp;
    d->high_temp = e.highTempTransitionIdx == 0xFFFF ? 3.4028234663852886e+38f : e.highTemp;
  }

  CompactedVector<ConduitTemperatureData> vec_;
  std::vector<float> out_temperatures_;
  std::vector<int32_t> frozen_;
  std::vector<int32_t> melted_;
  std::vector<int32_t> release_queue_[2];
  int32_t release_parity_ = 0;
  ConduitTemperatureUpdateData update_{};
};

}  // namespace oni_sim
