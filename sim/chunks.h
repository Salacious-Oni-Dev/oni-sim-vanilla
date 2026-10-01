// Element chunks: the sim's fifth component, and the busiest one the game talks to.
//
// A chunk is a lump of matter that is *not* in the grid — a dropped rock, a stored ore, the
// contents of a storage locker, a duplicant's suit. It has a temperature, a heat capacity
// and a cell it is sitting in, and once a substep it trades heat with that cell and, if the
// tile underneath is solid, with the tile underneath as well. Nothing else: it has no mass
// in the grid, it never moves on its own, and it never triggers a state transition in the
// cell it warms.
//
// Chunks carry more message traffic than most of the sim: a 38-second capture of a real game
// sent 12,194 chunk messages across five message types.
//
// The pieces, by the game's names:
//
//   * `ElementChunk::Register`   message -> ElementChunkData
//   * `ElementChunk::Modify`   SetElementChunkData
//   * `ElementChunk::ModifyEnergy`   ModifyElementChunkEnergy
//   * `ElementChunk::ModifyAdjuster`   ModifyChunkTemperatureAdjuster
//   * `ElementChunk::Unregister`   RemoveElementChunk
//   * `ElementChunk::Update`   the substep
//   * `ElementChunk::UpdateDataListOnly`   the substep-free frame
//   * `ExchangeHeatEnergyWithWorld`   chunk <-> one cell
//   * `CalculateTemperatureExchange`   the shared two-body solver
//   * `ProcessElementChunkMessages`   move/modify/energy/adjuster drain
//
// The component sits **fourth** in `SimData`'s component list (element consumer, element
// emitter, radiation emitter, element chunk, building heat exchange, building-to-building,
// disease emitter), so within a substep chunks exchange after the two element components
// (`sim/emitters.h`) and before buildings.
//
// Two things here are surprising enough to be worth stating before the code says them:
//
//   * The two-body solver is done in **double** and rounded back to float at the end, which
//     is the only place in the sim that does. Everything else Klei writes is float32
//     throughout, so this is not a stylistic difference — reproducing it in float misses.
//   * `ElementChunkInfo::deltaKJ` is accumulated by the substep and zeroed by the *export*,
//     not by the frame: `CopySimDataToGame` (0x180045..) copies the whole list into the
//     game's own vector and then walks the sim's, writing zero over the second float of each
//     entry. So the game sees one frame's energy however many substeps that frame ran, and
//     the sim-side list is never the thing the game reads. Reading the substep alone says
//     "running total", and a probe that only ran one substep per frame cannot tell the two
//     apart — `chunkenergy` needed five ticks past a divergence to show 0.07141 holding
//     steady on Klei's side against 0.07141, 0.14282, 0.21423 on ours.

#pragma once

#include <cstdint>
#include <vector>

#include "../abi/sim_abi.h"
#include "buildings.h"
#include "physics.h"
#include "world.h"

namespace oni_sim {

// The literal inside `ExchangeHeatEnergyWithWorld`. It is the same 0.001 the
// building components take from `SetDebugProperties`, but here it is a constant compiled
// into the function: the game cannot scale chunk transfer the way it scales building
// transfer.
inline constexpr float kChunkTransferRate = 0.001f;

// The margin on the melt test in `ElementChunk::Update`. A chunk announces
// itself as melting three kelvin past its element's transition point rather than at it.
inline constexpr float kChunkTransitionMargin = 3.0f;

// `Element::State` bit 16. Klei's `ExchangeHeatEnergyWithWorld` refuses outright to trade
// heat with a cell whose element carries it — the state byte lives at offset 0 of
// `ElementTemperatureData`, confirmed against `ElementTemperatureData::IsSolid`,
// which reads the same byte for its `& 3`.
//
// Nothing else in this sim reads the bit; cell-to-cell conduction has its own gate.
inline constexpr uint8_t kStateTemperatureInsulated = 16;

// `ModifyChunkTemperatureAdjuster`: a fictitious second body a chunk exchanges against
// *instead of* the world. It is how the game models something actively driving a chunk's
// temperature — a duplicant's suit in a dock, food in a fridge. A chunk with a live
// adjuster does not touch the grid at all, and reports no energy.
struct ElementChunkAdjuster {
  float temperature = 0.0f;
  float heat_capacity = 0.0f;
  float thermal_conductivity = 0.0f;
};

// Klei's `ElementChunkData`, 44 bytes — the same size as `BuildingHeatExchangeData` and for
// the same reason: one lump of heat plus where it is.
//
// Four of the fields are derived at registration rather than sent, and the derivations are
// the part worth reading:
//
//   * `heat_capacity` is `element.specificHeatCapacity * mass`, in that order.
//   * `thermal_conductivity`, `low_temp` and `high_temp` come from the element table and
//     the message has no say in them. A building's conductivity is a multiplier on its
//     material's; a chunk's *is* its material's.
//   * `max_energy_transfer_scale_factor` is `surfaceArea / thickness` — the message sends
//     the two separately and the sim only ever wants the ratio.
struct ElementChunkData {
  // Padded cell index, converted at registration. Klei stores the padded index too, which
  // is why `MoveElementChunk` reconverts rather than storing what the game sent.
  int32_t cell = 0;
  float temperature = 0.0f;
  float heat_capacity = 0.0f;
  float thermal_conductivity = 0.0f;
  float max_energy_transfer_scale_factor = 0.0f;
  float ground_transfer_scale = 0.0f;
  float high_temp = 0.0f;
  float low_temp = 0.0f;
  ElementChunkAdjuster adjuster;
};

// Everything the component hands the game in one frame.
//
// `info` is indexed by **handle slot**, not appended to, the same shape the building
// temperature list has: `Update` resizes it to the slot count and writes each live chunk
// into its own slot, so a chunk outside the active region keeps whatever it last reported.
struct ElementChunkState {
  CompactedVector<ElementChunkData> chunks;
  // The accumulator the substep writes into.
  std::vector<ElementChunkInfo> info;
  // The copy the game is handed. Klei keeps the same two: `GameData` owns a vector of its
  // own and `CopySimDataToGame` copies into it, which is the only reason the sim can zero
  // `deltaKJ` immediately afterwards without the game losing it.
  std::vector<ElementChunkInfo> published;
  std::vector<int32_t> melted;

  void ClearFrame() { melted.clear(); }
  void Clear() {
    chunks.Clear();
    info.clear();
    published.clear();
    melted.clear();
  }
};

// ------------------------------------------------------------------ the two-body solver

// What `CalculateTemperatureExchange` hands back. Klei returns a `tuple<float,float,float>`
// and the memory order is (b, a, energy), which is the order the callers unpack it in.
//
// `energy` is the energy the **b** body gained, in kJ. Both branches of the wrapper below
// agree on that, which is the only reason the swap is safe to fold away.
struct ExchangeResult {
  float t_b = 0.0f;
  float t_a = 0.0f;
  float energy = 0.0f;
};

// `details::CalculateTemperatureExchange_precise`.
//
// Everything here is double. The arguments arrive as floats, are widened by the ABI, and
// only the three results are rounded back — including `energy`, which is computed from the
// *already rounded* `t_a` rather than from the double. That last detail is not a rounding
// nicety: it is a kJ figure the game accumulates, so computing it from the double drifts.
//
// The transfer is the smallest of three quantities, which is what stops a substep
// overshooting: the conduction the pair can carry in `dt`, and the energy it takes to move
// each body to the other's temperature. Both results are then clamped at the pair's
// mass-weighted equilibrium, so a substep can equalise them outright and never past.
inline ExchangeResult CalculateTemperatureExchangePrecise(double scale, double conductivity,
                                                          double dt, double t_a, double hc_a,
                                                          double t_b, double hc_b) {
  const double equilibrium = (t_a * hc_a + t_b * hc_b) / (hc_a + hc_b);
  const double reach = (t_b - t_a) * scale;
  // Klei builds the three candidates into a stack array and walks it front to back, so the
  // conduction term wins ties. Written out rather than looped, same order.
  double q = (t_b - t_a) * conductivity * dt;
  const double cap_a = reach * hc_a;
  const double cap_b = reach * hc_b;
  if (cap_a < q) q = cap_a;
  if (cap_b < q) q = cap_b;

  // `MINSD` and `MAXSD`, not comparisons — see `ClampSS` in
  // world.h. Klei guards neither heat capacity here, so `q / hc_a` is a 0/0 the moment one
  // body has none, and Klei's pair returns the equilibrium where a comparison keeps the NaN
  // and writes it straight into a cell.
  //
  // WHETHER ANYTHING CAN REACH IT, checked over the game's element table: four of the 212
  // elements ship
  // `specificHeatCapacity: 0` — neutronium at index 119 and the three vacuum-like entries at
  // 209..211 — and **all four also carry `kStateTemperatureInsulated`**, which
  // `ExchangeChunkHeatWithCell` refuses above before it ever computes `cell_hc`. The adjuster
  // and the chunk's own capacity are both guarded by the caller. So on the shipped element
  // table this divide cannot be driven to 0/0, and the change here is fidelity, not a repair:
  // it costs nothing, and it stops the next element that arrives with no specific heat — a
  // modded one, or one this project adds for Mod 2 — from poisoning a cell silently.
  double next_a = q / hc_a + t_a;
  next_a = MinSD(next_a, equilibrium);
  double next_b = (-1.0 / hc_b) * q + t_b;
  next_b = MaxSD(next_b, equilibrium);

  ExchangeResult r;
  r.t_b = static_cast<float>(next_b);
  r.t_a = static_cast<float>(next_a);
  r.energy = static_cast<float>((t_a - static_cast<double>(r.t_a)) * hc_a);
  return r;
}

// `CalculateTemperatureExchange`. A wrapper that guarantees the solver always
// sees the colder body first, by swapping the pair and mirroring the answer when it does
// not. The swap exists for reproducibility rather than for physics — the arithmetic is not
// symmetric in the last bits — so it has to be copied even though the algebra says it is a
// no-op.
inline ExchangeResult CalculateTemperatureExchange(float scale, float conductivity, float dt,
                                                   float t_a, float hc_a, float t_b,
                                                   float hc_b) {
  if (t_a <= t_b) {
    return CalculateTemperatureExchangePrecise(scale, conductivity, dt, t_a, hc_a, t_b, hc_b);
  }
  const ExchangeResult s =
      CalculateTemperatureExchangePrecise(scale, conductivity, dt, t_b, hc_b, t_a, hc_a);
  ExchangeResult r;
  r.t_b = s.t_a;
  r.t_a = s.t_b;
  r.energy = -s.energy;
  return r;
}

// ------------------------------------------------------------------------ registration

// `ElementChunk::Register` / the `AddElementChunkMessage` half of it.
inline ElementChunkData ChunkDataFromMessage(const ElementTable& table, const World& w,
                                             const AddElementChunkMessage& m) {
  const Element& e = table.At(m.elementIdx);
  ElementChunkData d;
  d.temperature = m.temperature;
  d.thermal_conductivity = e.thermalConductivity;
  d.low_temp = e.lowTemp;
  d.high_temp = e.highTemp;
  d.heat_capacity = e.specificHeatCapacity * m.mass;
  d.max_energy_transfer_scale_factor = m.surfaceArea / m.thickness;
  d.cell = static_cast<int32_t>(w.Padded(static_cast<size_t>(m.gameCell)));
  d.ground_transfer_scale = m.groundTransferScale;
  return d;
}

// `ElementChunk::Modify` — the handler for `SetElementChunkData`. Two fields and nothing else: the chunk keeps
// its cell, its element's conductivity and its transition points. That is how a storage
// locker gaining a rock changes the lump's heat capacity without re-registering it.
inline void ModifyChunk(ElementChunkState* state, const SetElementChunkDataMessage& m) {
  ElementChunkData* d = state->chunks.Get(m.handle);
  if (!d) return;
  d->temperature = m.temperature;
  d->heat_capacity = m.heatCapacity;
}

// `ElementChunk::ModifyEnergy`. A discrete lump of heat, floored at zero and
// **not** ceilinged — unlike the building version, which refuses anything out of range
// rather than clamping. A chunk with no heat capacity absorbs the energy silently.
inline void ModifyChunkEnergy(ElementChunkState* state,
                              const ModifyElementChunkEnergyMessage& m) {
  ElementChunkData* d = state->chunks.Get(m.handle);
  if (!d || d->heat_capacity <= 0.0f) return;
  float next = m.deltaKJ / d->heat_capacity + d->temperature;
  if (next <= 0.0f) next = 0.0f;
  d->temperature = next;
}

// `ElementChunk::ModifyAdjuster`. Three floats copied straight in; a
// `heat_capacity` of zero is how the game switches the adjuster back off.
inline void ModifyChunkAdjuster(ElementChunkState* state,
                                const ModifyElementChunkAdjusterMessage& m) {
  ElementChunkData* d = state->chunks.Get(m.handle);
  if (!d) return;
  d->adjuster.temperature = m.temperature;
  d->adjuster.heat_capacity = m.heatCapacity;
  d->adjuster.thermal_conductivity = m.thermalConductivity;
}

// The `MoveElementChunk` arm of `ProcessElementChunkMessages`. Inline in the
// drain rather than a method on the component, which is why there is no `ElementChunk::Move`
// in the symbols.
inline void MoveChunk(ElementChunkState* state, const World& w,
                      const MoveElementChunkMessage& m) {
  ElementChunkData* d = state->chunks.Get(m.handle);
  if (!d) return;
  d->cell = static_cast<int32_t>(w.Padded(static_cast<size_t>(m.gameCell)));
}

// ----------------------------------------------------------------------- the exchange

// `ExchangeHeatEnergyWithWorld`. One chunk against one cell, and the return is
// the energy the *chunk* gained.
//
// Four gates before any arithmetic, and each of them is a real behaviour:
//
//   * a massless cell trades nothing, and reports nothing;
//   * a chunk outside 0..10000 K is refused rather than clamped, and Klei logs it through
//     `KCrashReporterReportMessage` — the two assert strings in the DLL are this function's;
//   * a cell outside the same range is refused the same way;
//   * an element carrying `State::TemperatureInsulated` refuses outright. This is the gate
//     that keeps a rock in an insulated tile from heating it.
//
// The conductivity that goes into the solver is the *smaller* of the chunk's and the cell's
// insulation-attenuated one, not their product — which is the same shape cell-to-cell
// conduction uses when either factor drops below one, and the opposite of what buildings do.
//
// No transition test: Klei writes the cell's temperature and walks away. A chunk can boil
// the water it is sitting in and the cell does not become steam until some other kernel
// looks at it.
inline float ExchangeChunkHeatWithCell(World* w, const ElementTable& table, float dt,
                                       size_t padded, float transfer_scale,
                                       ElementChunkData* d) {
  PhaseEntry& c = w->Phases()[padded];
  const float mass = c.mass;
  if (mass <= 0.0f) return 0.0f;
  if (d->temperature < 0.0f || d->temperature > kMaxTemperature) return 0.0f;
  const Element& e = table.At(c.element);
  const float t_cell = c.temperature;
  if (t_cell < 0.0f || t_cell > kMaxTemperature) return 0.0f;
  if ((e.state & kStateTemperatureInsulated) != 0) return 0.0f;

  const float ins = static_cast<float>(w->Insulation()[padded]);
  // Klei's multiply order, left to right, in float32. Reassociating moves the last bits.
  float k = ins * ins * kInsulationScale * e.thermalConductivity;
  if (d->thermal_conductivity <= k) k = d->thermal_conductivity;

  const ExchangeResult r = CalculateTemperatureExchange(
      1.0f, k * transfer_scale * d->max_energy_transfer_scale_factor * kChunkTransferRate, dt,
      t_cell, mass * e.specificHeatCapacity, d->temperature, d->heat_capacity);
  c.temperature = r.t_a;
  d->temperature = r.t_b;
  return r.energy;
}

// `ElementChunk::Update` — one substep of the whole component.
//
// The region test is a **point** test on the chunk's own cell, in padded coordinates and
// inclusive at both ends, unlike the building components which test their extents. So a
// chunk in two overlapping regions exchanges twice, and one in none exchanges not at all —
// but it still reports, and it still announces melting.
//
// The melt announcement is deliberately outside the region gate and outside the heat-capacity
// gate: it is level-triggered, re-announced every substep for as long as the chunk stays out
// of range, in the same shape as the backwall transition list.
inline void StepElementChunks(World* w, const ElementTable& table, ElementChunkState* state,
                              float dt, size_t ri) {
  std::vector<ElementChunkData>& all = state->chunks.Data();
  const std::vector<int32_t>& handles = state->chunks.Handles();
  // Grows and shrinks with the slot count; existing entries keep their values, which is what
  // makes a chunk that was skipped this substep read as unchanged rather than as absent.
  state->info.resize(state->chunks.SlotCount());

  const World::PaddedRect& r = w->PaddedRegion(ri);
  const int32_t pw = w->PaddedWidth();
  std::vector<PhaseEntry>& cells = w->Phases();

  for (size_t i = 0; i < all.size(); ++i) {
    ElementChunkData& d = all[i];
    const int32_t handle = handles[i];
    float accumulated = 0.0f;

    const int32_t px = d.cell % pw;
    const int32_t py = d.cell / pw;
    if (px >= r.x0 && py >= r.y0 && px <= r.x1 && py <= r.y1 && d.heat_capacity > 0.0f) {
      if (d.adjuster.heat_capacity > 0.0f) {
        // The adjuster replaces the world entirely, and its own new temperature is thrown
        // away — it is a source, not a body. No energy is reported for this path.
        const ExchangeResult x = CalculateTemperatureExchange(
            1.0f, d.adjuster.thermal_conductivity, dt, d.adjuster.temperature,
            d.adjuster.heat_capacity, d.temperature, d.heat_capacity);
        d.temperature = x.t_b;
      } else {
        accumulated = ExchangeChunkHeatWithCell(w, table, dt, static_cast<size_t>(d.cell),
                                                1.0f, &d);
        // And the tile underneath, at the chunk's own ground scale, but only if it is
        // solid. This is why a hot rock on a metal tile cools faster than one in mid-air.
        const size_t below = static_cast<size_t>(d.cell - pw);
        if (table.Phase(cells[below].element) == kStateSolid) {
          accumulated += ExchangeChunkHeatWithCell(w, table, dt, below,
                                                   d.ground_transfer_scale, &d);
        }
      }
    }

    const size_t slot = static_cast<size_t>(handle & kHandleIndexMask);
    if (slot < state->info.size()) {
      state->info[slot].temperature = d.temperature;
      // Accumulated, never assigned. See the header comment: nothing in a running frame
      // clears this.
      state->info[slot].deltaKJ = accumulated + state->info[slot].deltaKJ;
    }
    if (d.temperature >= d.high_temp + kChunkTransitionMargin ||
        d.temperature < d.low_temp - kChunkTransitionMargin) {
      state->melted.push_back(handle);
    }
  }
}

// `ElementChunk::UpdateDataListOnly`.
//
// `Sim::Main` calls this instead of the substep when a frame has
// no time in it — the game is paused, or the queued frame carried a non-positive dt. It
// republishes every chunk's current temperature and is the **only** thing that ever zeroes
// `deltaKJ`.
inline void UpdateChunkDataList(ElementChunkState* state) {
  state->info.resize(state->chunks.SlotCount());
  const std::vector<ElementChunkData>& all = state->chunks.Data();
  const std::vector<int32_t>& handles = state->chunks.Handles();
  for (size_t i = 0; i < all.size(); ++i) {
    const size_t slot = static_cast<size_t>(handles[i] & kHandleIndexMask);
    if (slot >= state->info.size()) continue;
    state->info[slot].temperature = all[i].temperature;
    state->info[slot].deltaKJ = 0.0f;
  }
}

// The export half of `CopySimDataToGame`: hand the game a copy of the whole list, then walk
// the sim's and zero the second float of every entry. The temperature is left alone, so a
// chunk that was skipped this frame keeps reporting the temperature it had; only the energy
// resets.
inline void PublishChunkInfo(ElementChunkState* state) {
  state->published = state->info;
  for (ElementChunkInfo& e : state->info) e.deltaKJ = 0.0f;
}

}  // namespace oni_sim
