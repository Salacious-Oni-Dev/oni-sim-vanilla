// Element consumers and element emitters: the sim's **first two** components, and the pair
// the game uses to move matter between a building and the grid.
//
// A consumer is a pump: a cell, a radius, a rate, and either one element it wants or a whole
// phase. Once a substep it floods outward from its cell, takes `dt * rate` kilograms of that
// element out of whatever it reaches, and hands the game a `ConsumedMassInfo` describing
// what it got. An emitter is a vent: a cell, an interval, a mass, a temperature and a
// pressure ceiling. Once every `emitInterval` seconds it floods outward, finds a cell under
// the ceiling, and puts the matter there — displacing whatever is in the way if it can.
//
// They sit at the front of `SimData`'s component list (element consumer, element emitter,
// radiation emitter, element chunk, building heat exchange, building-to-building, disease
// emitter), so within a substep they run **before** chunks and buildings.
//
// The game's functions this file reproduces:
//
//   * `ElementConsumer::Register`   `ElementConsumer::Modify`
//   * `ElementConsumer::Update`   `AnyInputCellHasState`
//   * `ElementEmitter::Register`   `ElementEmitter::Modify`
//   * `ElementEmitter::Update`   `ElementEmitter::TryEmit`
//   * `ElementEmitter::Emit`   `...::UpdateDataListOnly`
//   * `Flood<>`   `GetReachableCells`
//   * `FloodRemoved`   `RectangularRemoved`
//   * `do_remove`   `CalculateCombinedTemperature`
//
// `Flood` and `do_remove` are not the components' own: `ProcessMassConsumption`
// drives the same two functions from the `MassConsumption` message, which is
// why that handler moved in here as well.
//
// Five behaviours in here that a reasonable implementation would get wrong:
//
//   * **A solid emitter does not put anything in the grid.** `TryEmit`'s `state == Solid`
//     branch pushes a `SpawnOreInfo` onto the ore-spawn list and falls straight back into the
//     loop without calling `Emit` — so it can drop ore in *every* reachable cell, and the
//     emitter's own `emittedMassInfo` slot stays empty. The ore is gated on the cell being
//     visible unless the game is in debug editing.
//   * **A registered emitter never fires.** `Register` writes `emitInterval = FLT_MAX`
// and `emitMass = 0`, so the emitter is inert until a `ModifyElementEmitter`
//     arrives. `blockedState` starts at `0xff`, which is neither of its two values, so the
//     first update always fires one of the two blocked callbacks.
//   * **`AnyInputCellHasState`'s test is `(state & 3 & wanted) == wanted`**, which for
//     `Liquid` (2) also accepts `Solid` (3) and for `Gas` (1) also accepts `Solid`. It is
//     harmless only because `GetReachableCells` has already refused every solid cell.
//   * **`emittedMassInfo` is indexed by handle slot** and is *not* cleared per frame: the
//     export copies it and then writes `vacuumElementIdx`, `mass = 0` and `temperature = 0`
//     over each entry, leaving `diseaseIdx` and `diseaseCount` alone.
//   * **The flood's visited test is a linear scan** and its queue is a FIFO whose four
//     pushes are up, left, right, down. Order decides which cell an emitter picks when
//     several are equally good, so it is copied rather than tidied into a set.

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "../abi/sim_abi.h"
#include "buildings.h"
#include "cellmod.h"
#include "disease.h"
#include "physics.h"
#include "world.h"

namespace oni_sim {

// `Element::defaultValues.temperature`, the fallback an emitter uses when its own
// `emitTemperature` is negative.
inline float ElementDefaultTemperature(const Element& e) { return e.defaultValues.temperature; }

// `CalculateCombinedTemperature`. A mass-weighted mix clamped to the two
// inputs, and **not** the same function as `FastCalculateCombinedTemperature`,
// which has no zero guard and clamps in a different order.
//
// The multiply order is Klei's, left to right: `(mass_b * temp_b + mass_a * temp_a)`.
// Reassociating it moves the last bits.
inline float CalculateCombinedTemperature(float mass_a, float temp_a, float mass_b,
                                          float temp_b) {
  const float total = mass_a + mass_b;
  if (total <= 0.0f) return 0.0f;
  const float mix = (mass_b * temp_b + mass_a * temp_a) / total;
  const float lo = temp_a < temp_b ? temp_a : temp_b;
  const float hi = temp_a < temp_b ? temp_b : temp_a;
  return mix > hi ? hi : (mix < lo ? lo : mix);
}

// `FastCalculateCombinedTemperature`. The same mass-weighted mix with no zero
// guard, and the clamp written the other way round: it assumes `temp_a <= temp_b` and its
// callers arrange that before calling. The ore export is the only caller here.
//
// The first test is `!(temp_b >= mix)` and not `temp_b < mix` because there is no zero guard:
// two massless inputs divide 0 by 0, and in the game an unordered compare takes the
// fall-through path, so a NaN mix comes back as `temp_b`. `temp_b < mix` is false for a
// NaN and would return `temp_a` instead. See `ClampRunningMix` below for the same hazard in
// `do_remove`, where it reached the game.
inline float FastCalculateCombinedTemperature(float mass_a, float temp_a, float mass_b,
                                              float temp_b) {
  const float mix = (mass_a * temp_a + mass_b * temp_b) / (mass_a + mass_b);
  if (!(temp_b >= mix)) return temp_b;
  return temp_a <= mix ? mix : temp_a;
}

// The clamp `do_remove` puts on its running mix. It is NOT the `if (mix > hi)`
// it reads as, and the difference is load-bearing.
//
// The game clamps with SSE min then max, and both return their **second** operand whenever
// either operand is a NaN. `do_remove`'s mix divides by `info->mass + take`,
// which is exactly 0 on a cell that holds the right element and no mass — a hole in solid
// rock, a gas cell a kernel emptied earlier in the same frame — so the quotient is a 0/0 NaN
// on that cell and Klei's clamp quietly replaces it with the bound.
//
// Written as comparisons the NaN passes every test untouched, and it does not stay local: the
// next cell's mix multiplies the running mass by it, `0 * NaN` is NaN, and a drain that took
// real mass hands the game a NaN temperature: in the game that shows as
// `GameUtil.GetFinalTemperature: t2=NaN` under `ElementConsumer.AddMass`, with
// `PrimaryElement.SetTemperature` refusing the value behind it.
inline float ClampRunningMix(float mix, float lo, float hi) {
  mix = mix < hi ? mix : hi;   // SSE min: NaN -> hi
  return mix > lo ? mix : lo;  // SSE max: NaN -> lo
}

// ------------------------------------------------------------------------------- the flood

// `FloodFillInfo`, 6 bytes. The coordinates are `uint16` in Klei and that is load-bearing:
// a node pushed from `x == 0` wraps to 65535 and is refused by the width test rather than
// by an underflow check that does not exist.
struct FloodNode {
  uint16_t x;
  uint16_t y;
  uint16_t depth;
};

// The scratch three of the four flood entry points share. Klei keeps these as members of
// the component (`ElementConsumer` and `ElementEmitter` each own a visited vector, a
// reachable vector and a queue) and as locals in `ProcessMassConsumption`; they are pooled
// here because nothing observes which buffer a flood ran out of.
struct FloodScratch {
  std::vector<int32_t> visited;
  std::vector<int32_t> reachable;
  std::vector<FloodNode> queue;
};

// `Flood<Fn>`.
//
// Breadth-first from `(x0, y0)`, at most `max_depth` **steps**, refusing whichever phases
// the three flags name. `fn` returning true stops the whole flood, which is how
// `FloodRemoved` gets out the moment it has taken everything it wanted.
//
// The order of the guards is Klei's, and the depth test comes *first*: a node at the depth
// limit is dropped before its coordinates are even looked at.
template <typename Fn>
inline void Flood(World* w, const ElementTable& t, int32_t x0, int32_t y0, int32_t max_depth,
                  bool skip_solid, bool skip_liquid, bool skip_gas,
                  std::vector<int32_t>* visited, std::vector<FloodNode>* queue, Fn fn) {
  const std::vector<PhaseEntry>& cells = w->Phases();
  const int32_t pw = w->PaddedWidth();
  const int32_t ph = w->PaddedHeight();

  visited->clear();
  queue->clear();
  queue->push_back(FloodNode{static_cast<uint16_t>(x0), static_cast<uint16_t>(y0), 0});

  for (size_t head = 0; head < queue->size(); ++head) {
    const FloodNode n = (*queue)[head];
    if (static_cast<int32_t>(n.depth) >= max_depth) continue;
    if (n.x == 0 || static_cast<int32_t>(n.x) >= pw) continue;
    if (n.y == 0 || static_cast<int32_t>(n.y) >= ph) continue;

    const int32_t cell = static_cast<int32_t>(n.y) * pw + static_cast<int32_t>(n.x);
    const uint8_t phase = t.Phase(cells[static_cast<size_t>(cell)].element);
    if (skip_solid && phase == kStateSolid) continue;
    if (skip_liquid && phase == kStateLiquid) continue;
    if (skip_gas && phase == kStateGas) continue;
    if (std::find(visited->begin(), visited->end(), cell) != visited->end()) continue;

    visited->push_back(cell);
    if (fn(cell)) return;

    const uint16_t d = static_cast<uint16_t>(n.depth + 1);
    queue->push_back(FloodNode{n.x, static_cast<uint16_t>(n.y - 1), d});
    queue->push_back(FloodNode{static_cast<uint16_t>(n.x - 1), n.y, d});
    queue->push_back(FloodNode{static_cast<uint16_t>(n.x + 1), n.y, d});
    queue->push_back(FloodNode{n.x, static_cast<uint16_t>(n.y + 1), d});
  }
}

// `GetReachableCells`. Every non-solid cell within `max_depth` steps, in the
// order the flood reached them. Both consumers and emitters use it to decide *where* they
// are allowed to work; the caller then picks one of the cells out of the list.
inline void GetReachableCells(World* w, const ElementTable& t, int32_t x, int32_t y,
                              int32_t max_depth, FloodScratch* s) {
  s->reachable.clear();
  Flood(w, t, x, y, max_depth, /*skip_solid=*/true, /*skip_liquid=*/false,
        /*skip_gas=*/false, &s->visited, &s->queue, [s](int32_t c) {
          s->reachable.push_back(c);
          return false;
        });
}

// `do_remove`, the body both flood shapes share. Takes up to `*remaining`
// kilograms of `element` out of one cell and folds what it got into `info`.
//
// Three details that a rewrite loses:
//
//   * The germs removed are `(int)(count * (take / mass_before))` — truncated, and computed
//     against the mass *before* the subtraction. A cell drained to nothing still keeps the
//     rounding remainder of its germ count until the `< 1` test clears it.
//   * `info->mass` is written **before** the temperature it is the denominator of.
//   * The disease merge is `CalculateFinalDiseaseCount`, the standalone twin
//     of the merge inside `AddDiseaseToCell`, and it runs on the cell's disease index even
//     when the cell had no germs left to give.
inline void RemoveMassFromCell(World* w, const ElementTable& t, const DiseaseTable& diseases,
                               float* remaining, uint16_t element, ConsumedMassInfo* info,
                               size_t cell) {
  std::vector<PhaseEntry>& cells = w->Phases();
  PhaseEntry& c = cells[cell];
  if (c.element != element) return;

  const float mass_before = c.mass;
  const float take = *remaining <= mass_before ? *remaining : mass_before;
  const int32_t germs_before = w->Disease()[cell].count;
  const uint8_t cell_disease = w->DiseaseIdx(cell);

  c.mass = mass_before - take;
  *remaining -= take;
  w->MarkProjectDirty(cell);

  const int32_t germs_taken =
      static_cast<int32_t>(static_cast<float>(germs_before) * (take / mass_before));
  w->MutableDisease(cell).count = germs_before - germs_taken;
  if (w->Disease()[cell].count < 1) ClearCellDisease(w, cell);

  const float t_cell = c.temperature;
  const float t_info = info->temperature;
  const float lo = t_info < t_cell ? t_info : t_cell;
  const float hi = t_info < t_cell ? t_cell : t_info;
  const float mass_was = info->mass;
  const float total = mass_was + take;
  info->mass = total;
  info->temperature = ClampRunningMix((mass_was * t_info + t_cell * take) / total, lo, hi);

  if (cell_disease != 0xFF) {
    const DiseaseMerge m = MergeDiseaseCounts(diseases, info->diseaseIdx, info->diseaseCount,
                                              cell_disease, germs_taken);
    if (m.valid) {
      info->diseaseIdx = m.idx;
      info->diseaseCount = m.count;
    }
  }

  if (!(c.mass > kCellModEmptyMass)) {
    ClearCell(w, t, cell);
    w->TouchSubstance(cell);
  }
}

// `FloodRemoved`. Floods from the cell and drains as it goes, stopping the
// moment the request is satisfied.
//
// The three phase flags are derived from the element being removed rather than passed:
// removing a **solid** floods through solids only, and removing anything else floods
// through everything except solids. There is no configuration for this.
inline void FloodRemoved(World* w, const ElementTable& t, const DiseaseTable& diseases,
                         float* remaining, uint16_t element, ConsumedMassInfo* info,
                         int32_t x, int32_t y, int32_t max_depth, FloodScratch* s) {
  const bool solid = t.Phase(element) == kStateSolid;
  Flood(w, t, x, y, max_depth, /*skip_solid=*/!solid, /*skip_liquid=*/solid,
        /*skip_gas=*/solid, &s->visited, &s->queue, [&](int32_t c) {
          RemoveMassFromCell(w, t, diseases, remaining, element, info,
                             static_cast<size_t>(c));
          return *remaining <= 0.0f;
        });
}

// `RectangularRemoved`. The same drain over a `width x height` rectangle whose
// top-left corner is the message's own cell — no flood, no phase test, and no bound beyond
// the array's own length. The `*remaining <= 0` test is at the **end** of each cell's turn,
// so it is checked even for cells whose element did not match.
inline void RectangularRemoved(World* w, const ElementTable& t, const DiseaseTable& diseases,
                               float* remaining, uint16_t element, ConsumedMassInfo* info,
                               int32_t x, int32_t y, int32_t width, int32_t height) {
  const int32_t pw = w->PaddedWidth();
  const int32_t ph = w->PaddedHeight();
  for (int32_t row = y; row < y + height; ++row) {
    for (int32_t col = x; col < x + width; ++col) {
      if (row < 0 || row >= ph || col < 0 || col >= pw) continue;
      RemoveMassFromCell(w, t, diseases, remaining, element, info,
                         static_cast<size_t>(row) * static_cast<size_t>(pw) +
                             static_cast<size_t>(col));
      if (*remaining <= 0.0f) return;
    }
  }
}

// ---------------------------------------------------------------------------- the consumer

// `ElementConsumerData`, 16 bytes. The game keeps the cell as a pair of 16-bit coordinates;
// the padded index is the same information and is what every reader wants.
struct ElementConsumerData {
  float consumption_rate = 0.0f;
  int32_t cell = 0;  // padded
  uint16_t element = 0;
  uint8_t max_depth = 0;
  uint8_t configuration = 0;
  // Where in the reachable list this consumer starts looking. Bumped by one every substep
  // in which it actually consumed, so a pump reading a mixed room does not always sample
  // the same cell first. It is a `uint8` and it wraps.
  uint8_t offset_idx = 0;
};

// `ElementConsumer::Update`'s `configuration` byte. Anything else is a silent no-op — the
// consumer runs, finds nothing to take, and does not even bump its offset.
enum : uint8_t {
  kConsumeSpecificElement = 0,
  kConsumeAllLiquid = 1,
  kConsumeAllGas = 2,
};

// `ElementEmitterData`, 44 bytes. The defaults are `Register`'s, not zero: an emitter that
// has been registered but never modified has `emitInterval = FLT_MAX` and so never fires,
// and `blocked_state = 0xff` so that its first update always reports one way or the other.
struct ElementEmitterData {
  float elapsed_time = 0.0f;
  float emit_interval = 3.4028234663852886e+38f;  // FLT_MAX
  float emit_mass = 0.0f;
  float emit_temperature = -1.0f;  // negative means "the element's default"
  float max_pressure = 0.0f;
  int32_t emit_disease_count = 0;
  int32_t cell = 0;  // padded
  uint16_t element = 0;
  uint8_t max_depth = 0;
  uint8_t offset_idx = 0;
  uint8_t blocked_state = 0xFF;
  uint8_t disease_idx = 0xFF;
  int32_t blocked_cb = -1;
  int32_t unblocked_cb = -1;
};

// `DiseaseEmitterData`, from `DiseaseEmitter::Register` — 20 bytes, and the
// defaults are the ones `Register` writes into a fresh slot: no cell, no disease, nothing to
// emit. The germ half of the emitter family, and the only thing in the sim that *creates*
// disease rather than moving what a message already put there.
struct DiseaseEmitterData {
  int32_t cell = -1;  // padded
  uint8_t disease_idx = 0xFF;
  uint8_t range = 0;
  int32_t emit_count = 0;
  float emit_interval = 0.0f;
  float elapsed_time = 0.0f;
};

// Everything the two components own, plus the scratch their floods share.
//
// `consumed` is appended to and handed over whole once a frame; `emitted` is indexed by
// **handle slot** and survives across frames, with only three of its five fields reset by
// the export. The two lists are shaped differently because Klei shapes them differently:
// `CopySimDataToGame` *swaps* the consumed list into the game's buffer and clears the sim's,
// and *copies* the emitted list and then partially blanks the sim's.
struct ElementFlowState {
  CompactedVector<ElementConsumerData> consumers;
  CompactedVector<ElementEmitterData> emitters;
  CompactedVector<DiseaseEmitterData> disease_emitters;
  std::vector<ConsumedMassInfo> consumed;
  std::vector<EmittedMassInfo> emitted;
  // Indexed by **handle slot**, like `emitted`, and resized to the emitter count at the top
  // of every `DiseaseEmitter::Update`.
  std::vector<DiseaseEmittedInfo> disease_emitted;
  std::vector<DiseaseEmittedInfo> published_disease_emitted;
  // The copies handed to the game, so the reset below cannot walk over what it just gave.
  std::vector<ConsumedMassInfo> published_consumed;
  std::vector<EmittedMassInfo> published_emitted;
  FloodScratch scratch;

  void Clear() {
    consumers.Clear();
    emitters.Clear();
    disease_emitters.Clear();
    consumed.clear();
    emitted.clear();
    disease_emitted.clear();
    published_disease_emitted.clear();
    published_consumed.clear();
    published_emitted.clear();
  }
};

// `AnyInputCellHasState`. Walks the reachable list from `offset`, wrapping,
// and returns the first element whose phase satisfies the wanted one.
//
// The test is `(state & 3 & wanted) == wanted`, so asking for Liquid also accepts Solid and
// asking for Gas also accepts Solid. That is Klei's, and it is only harmless because the
// reachable list has already had every solid cell filtered out of it.
inline bool AnyInputCellHasState(const World& w, const ElementTable& t,
                                 const std::vector<int32_t>& reachable, uint16_t* out,
                                 uint8_t wanted, uint8_t offset) {
  const size_t n = reachable.size();
  if (n == 0) return false;
  for (size_t i = 0; i < n; ++i) {
    const size_t at = (static_cast<size_t>(offset) + i) % n;
    const uint16_t e = w.Phase(static_cast<size_t>(reachable[at])).element;
    if (((t.At(e).state & kStateMask) & wanted) == wanted) {
      *out = e;
      return true;
    }
  }
  return false;
}

// `ElementConsumer::Update` — one substep of the whole component.
//
// The region test is a **point** test on the consumer's own cell, inclusive at both ends,
// the same shape the chunk component uses. A consumer in no region does nothing at all and
// reports nothing.
inline void StepElementConsumers(World* w, const ElementTable& t, const DiseaseTable& diseases,
                                 ElementFlowState* state, float dt, size_t ri) {
  std::vector<ElementConsumerData>& all = state->consumers.Data();
  const std::vector<int32_t>& handles = state->consumers.Handles();
  const World::PaddedRect& r = w->PaddedRegion(ri);
  const int32_t pw = w->PaddedWidth();

  for (size_t i = 0; i < all.size(); ++i) {
    ElementConsumerData& d = all[i];
    const int32_t x = d.cell % pw;
    const int32_t y = d.cell / pw;
    if (x < r.x0 || y < r.y0 || x > r.x1 || y > r.y1) continue;

    float amount = dt * d.consumption_rate;
    uint16_t element = 0;
    bool found = false;
    if (d.configuration == kConsumeSpecificElement) {
      element = d.element;
      found = true;
    } else if (d.configuration == kConsumeAllLiquid || d.configuration == kConsumeAllGas) {
      GetReachableCells(w, t, x, y, d.max_depth, &state->scratch);
      const uint8_t wanted =
          d.configuration == kConsumeAllLiquid ? kStateLiquid : kStateGas;
      found = AnyInputCellHasState(*w, t, state->scratch.reachable, &element, wanted,
                                   d.offset_idx);
    }
    if (!found) continue;

    ConsumedMassInfo info{};
    info.simHandle = handles[i];
    info.removedElemIdx = element;
    info.diseaseIdx = 0xFF;
    info.mass = 0.0f;
    info.temperature = 0.0f;
    info.diseaseCount = 0;
    FloodRemoved(w, t, diseases, &amount, element, &info, x, y, d.max_depth,
                 &state->scratch);
    if (info.mass > 0.0f) {
      w->NoteComponentConsumed(info.mass);
      state->consumed.push_back(info);
    }
    // Bumped only on a substep that actually looked for something, which is why a consumer
    // whose configuration byte is neither 0, 1 nor 2 never advances.
    d.offset_idx = static_cast<uint8_t>(d.offset_idx + 1);
  }
}

// ---------------------------------------------------------------------------- the emitter

// `ElementEmitter::Emit`. Puts one emission into one cell that `TryEmit` has
// already cleared for it, and reports what it put there.
//
// The report is the *message's* mass and temperature rather than what the cell gained, so a
// cell that rounds the addition away still reports the full emission. And the substance
// announcement fires only when the cell was **vacuum** — an emitter topping up a cell that
// already holds its own gas is invisible to the game's substance list.
inline EmittedMassInfo EmitInto(const CellModContext& cm, const ElementEmitterData& d,
                                size_t cell, float temperature) {
  World* w = cm.w;
  const ElementTable& t = *cm.elements;
  std::vector<PhaseEntry>& cells = w->Phases();
  const uint16_t vacuum = t.VacuumIndex();
  const uint16_t was = cells[cell].element;

  EmittedMassInfo out{};
  out.elemIdx = d.element;
  out.diseaseIdx = 0xFF;
  out.mass = d.emit_mass;
  out.temperature = temperature;
  out.diseaseCount = 0;

  // Klei asserts here and then returns the same report without touching the cell.
  if (was != d.element && was != vacuum) return out;

  PhaseEntry& c = cells[cell];
  c.element = d.element;
  c.temperature =
      CalculateCombinedTemperature(c.mass, c.temperature, d.emit_mass, temperature);
  const float before = c.mass;
  c.mass = before + d.emit_mass;
  w->NoteComponentEmitted(c.mass - before);
  w->MarkProjectDirty(cell);
  AddDiseaseToCell(w, *cm.diseases, cell, d.disease_idx, d.emit_disease_count);
  if (was == vacuum) w->TouchSubstance(cell);
  return out;
}

// `ElementEmitter::TryEmit`. Walks the reachable list from the emitter's own
// offset and puts the emission into the first cell that will take it.
//
// The four phases behave completely differently:
//
//   * **Vacuum** is refused outright — `(state & 3) != 0` gates the whole body.
//   * **Gas** displaces whatever gas is in the way, and gives up on that cell if it cannot.
//   * **Liquid** tries `DisplaceLiquid` first and `DisplaceGas` second, on the element the
//     cell holds *after* the failed liquid attempt rather than the one it held before.
//   * **Solid** never touches the grid at all. It hands the game a `SpawnOreInfo` — gated on
//     the cell being visible, or on the game being in debug editing — and carries on round
//     the loop, so one solid emitter can drop ore in every reachable cell in one substep.
//
// The pressure test is `mass < maxPressure` written as two comparisons, so a cell exactly at
// the ceiling is refused.
inline EmittedMassInfo TryEmit(const CellModContext& cm, const ElementEmitterData& d,
                               const std::vector<int32_t>& reachable,
                               std::vector<SpawnOreInfo>* ore, const uint8_t* visible,
                               bool debug_editing) {
  World* w = cm.w;
  const ElementTable& t = *cm.elements;
  std::vector<PhaseEntry>& cells = w->Phases();
  const uint16_t vacuum = t.VacuumIndex();

  EmittedMassInfo out{};
  out.elemIdx = 0xFFFF;
  out.diseaseIdx = 0xFF;
  out.mass = 0.0f;
  out.temperature = 0.0f;
  out.diseaseCount = 0;

  const Element& e = t.At(d.element);
  float temperature = d.emit_temperature;
  if (temperature < 0.0f) temperature = ElementDefaultTemperature(e);
  const uint8_t phase = e.state & kStateMask;

  const size_t n = reachable.size();
  for (size_t i = 0; i < n; ++i) {
    const size_t at = (static_cast<size_t>(d.offset_idx) + i) % n;
    const size_t cell = static_cast<size_t>(reachable[at]);
    if (!(cells[cell].mass < d.max_pressure)) continue;
    if (phase == kStateVacuum) continue;

    if (phase == kStateGas) {
      const uint16_t here = cells[cell].element;
      if (here != d.element && here != vacuum) {
        if (!DisplaceGasFromDrain(cm, cell, here)) continue;
      }
    } else if (phase == kStateLiquid) {
      const uint16_t here = cells[cell].element;
      if (here != d.element && here != vacuum) {
        if (!DisplaceLiquid(cm, cell, here)) {
          // Re-read: `DisplaceLiquid` may have moved a gas neighbour and left this cell
          // holding something else. Klei reloads the element.
          if (!DisplaceGasFromDrain(cm, cell, cells[cell].element)) continue;
        }
      }
    } else {
      // Solid. Nothing enters the grid; the game is handed a lump of ore instead.
      if (d.emit_mass > 0.0f) {
        const int64_t game = w->GameIndex(cell);
        if (game >= 0 && static_cast<size_t>(game) < w->GameCount() &&
            (debug_editing || visible == nullptr ||
             visible[static_cast<size_t>(game)] != 0)) {
          SpawnOreInfo o{};
          o.cellIdx = static_cast<int32_t>(game);
          o.elemIdx = d.element;
          o.diseaseIdx = 0xFF;
          o.mass = d.emit_mass;
          o.temperature = temperature;
          o.diseaseCount = 0;
          w->NoteEmitterOre(d.emit_mass);
          ore->push_back(o);
        }
      }
      continue;
    }

    return EmitInto(cm, d, cell, temperature);
  }
  return out;
}

// `ElementEmitter::Update` — one substep of the whole component.
//
// The interval is a carry rather than a countdown: a frame that goes past several intervals
// subtracts **one** of them and emits **once**, so an emitter can never catch up with itself.
// `elapsedTime` is advanced whether or not anything was emitted, and whether or not the
// emitter was even allowed to look — but only if it was inside the region.
//
// The blocked callbacks are edge-triggered on a three-valued flag, and the third value is
// what a freshly registered emitter carries, so the game always hears once at the start.
inline void StepElementEmitters(World* w, const ElementTable& t, const DiseaseTable& diseases,
                                ElementFlowState* state, uint16_t rotation, float dt,
                                std::vector<SpawnOreInfo>* ore,
                                std::vector<CallbackInfo>* callbacks, const uint8_t* visible,
                                bool debug_editing, size_t ri) {
  std::vector<ElementEmitterData>& all = state->emitters.Data();
  const std::vector<int32_t>& handles = state->emitters.Handles();
  // Sized by the **slot** count and indexed by handle slot. Not by the number of live
  // emitters: removing one compacts `Data()` and `Handles()` but never `index_`, so a list
  // sized by the live count would drop the highest slot the moment anything is removed —
  // which is exactly what `germemit` measured against Klei at tick 31 (5 slots, 4 live,
  // Klei still publishing 5).
  state->emitted.resize(state->emitters.SlotCount());

  const World::PaddedRect& r = w->PaddedRegion(ri);
  const int32_t pw = w->PaddedWidth();
  const uint16_t vacuum = t.VacuumIndex();
  const CellModContext cm{w, &t, &diseases, rotation};

  for (size_t i = 0; i < all.size(); ++i) {
    ElementEmitterData& d = all[i];
    const int32_t x = d.cell % pw;
    const int32_t y = d.cell / pw;
    if (x < r.x0 || y < r.y0 || x > r.x1 || y > r.y1) continue;

    float elapsed = d.elapsed_time;
    const float interval = d.emit_interval;
    if (interval <= elapsed) {
      const size_t slot = static_cast<size_t>(handles[i] & kHandleIndexMask);
      if (slot < state->emitted.size()) {
        EmittedMassInfo& acc = state->emitted[slot];
        // One slot can only ever report one element in a frame; a second emission of a
        // different element is refused rather than overwriting the first.
        if (acc.elemIdx == 0xFFFF || acc.elemIdx == vacuum || acc.elemIdx == d.element) {
          GetReachableCells(w, t, x, y, d.max_depth, &state->scratch);

          bool room = false;
          for (int32_t c : state->scratch.reachable) {
            if (w->Phase(static_cast<size_t>(c)).mass < d.max_pressure) {
              room = true;
              break;
            }
          }
          int32_t fire = -1;
          if (room) {
            if (d.blocked_state != 0) {
              fire = d.unblocked_cb;
              d.blocked_state = 0;
            }
          } else if (d.blocked_state != 1) {
            fire = d.blocked_cb;
            d.blocked_state = 1;
          }
          if (fire != -1) callbacks->push_back(CallbackInfo{fire});

          const EmittedMassInfo got =
              TryEmit(cm, d, state->scratch.reachable, ore, visible, debug_editing);
          if (got.mass > 0.0f) {
            const float was = acc.mass;
            acc.temperature =
                CalculateCombinedTemperature(was, acc.temperature, got.mass, got.temperature);
            acc.elemIdx = got.elemIdx;
            acc.mass = was + got.mass;
          }
          // Klei re-reads both fields here because `TryEmit` can reach a `Modify` — it
          // cannot in ours, but the read order is kept so the two stay comparable.
          elapsed = d.elapsed_time;
        }
      }
      elapsed = elapsed - interval;
    }
    d.elapsed_time = elapsed + dt;
  }
}

// `ElementEmitter::UpdateDataListOnly` — the frame with no substep in it.
// Every live emitter's slot is blanked back to "reported nothing", which is *not* what the
// export does: the export leaves the disease fields alone and writes the vacuum element,
// this writes `0xffff` and zeroes all four numbers.
inline void UpdateEmitterDataList(ElementFlowState* state) {
  const std::vector<int32_t>& handles = state->emitters.Handles();
  state->emitted.resize(state->emitters.SlotCount());
  for (int32_t h : handles) {
    const size_t slot = static_cast<size_t>(h & kHandleIndexMask);
    if (slot >= state->emitted.size()) continue;
    EmittedMassInfo& e = state->emitted[slot];
    e.elemIdx = 0xFFFF;
    e.diseaseIdx = 0xFF;
    e.mass = 0.0f;
    e.temperature = 0.0f;
    e.diseaseCount = 0;
  }
}

// The export half of `CopySimDataToGame` for both components.
//
// The consumed list is handed over whole and the sim's is emptied. The emitted list is
// *copied* and the sim's is then blanked in place — but only three of its five fields:
// `diseaseIdx` and `diseaseCount` survive into the next frame, and the element becomes
// Vacuum rather than `0xffff`.
inline void PublishElementFlow(ElementFlowState* state, uint16_t vacuum) {
  state->published_consumed.swap(state->consumed);
  state->consumed.clear();
  state->published_emitted = state->emitted;
  for (EmittedMassInfo& e : state->emitted) {
    e.elemIdx = vacuum;
    e.mass = 0.0f;
    e.temperature = 0.0f;
  }
  // The disease emitter's list is published the same way, and it is always `{0xFF, 0}`:
  // `DiseaseEmitter::Update` resizes it and never writes it, so the list only ever carries
  // what registration and `UpdateDataListOnly` put there. Confirmed against Klei on
  // `germemit`, where four emitters fire for fifty ticks and every slot stays `d255/0`.
  state->published_disease_emitted = state->disease_emitted;
  for (DiseaseEmittedInfo& e : state->disease_emitted) {
    e.diseaseIdx = 0xFF;
    e.count = 0;
  }
}

// `DiseaseEmitter::Update`. The germ emitter, and the only thing in the sim
// that creates disease.
//
// Four details, none of them the shape you would guess:
//
//   * the region test is **inclusive on both ends**, and the elapsed-time update lives
//     *inside* it — an emitter outside the active region does not accumulate time at all,
//     so it does not fire a backlog when the region reaches it;
//   * it emits **once** per update however far behind it is: `elapsed -= interval`, never a
//     modulo, and then `elapsed += dt` whether it fired or not;
//   * `emitCount < 0` is not an emission at all. It is a *removal*, and it only touches
//     reachable cells whose existing `diseaseIdx` already equals the emitter's — the branch
// tests the cell's index against the emitter's and skips the cell
//     otherwise. A positive count is added to every reachable cell without that test;
//   * `diseaseIdx == 0xFF` disables the emitter outright, which is what `Register` leaves in
//     a fresh slot.
//
// The flood is `GetReachableCells`, the same one the element emitter uses, so a disease
// emitter cannot push germs through a solid.
inline void StepDiseaseEmitters(World* w, const ElementTable& t, const DiseaseTable& diseases,
                                ElementFlowState* state, float dt, size_t ri) {
  std::vector<DiseaseEmitterData>& all = state->disease_emitters.Data();
  // Resized at the top of `Update`, before any region test, so a handle that never fires
  // still owns a slot in the export — and resized to the **slot** count, not the live
  // count, so a removal does not shorten the list. `germemit` removes an emitter at tick
  // 30 and Klei goes on publishing five entries for the four that are left.
  state->disease_emitted.resize(state->disease_emitters.SlotCount());
  const World::PaddedRect& r = w->PaddedRegion(ri);
  const int32_t pw = w->PaddedWidth();

  for (size_t i = 0; i < all.size(); ++i) {
    DiseaseEmitterData& d = all[i];
    if (d.disease_idx == 0xFF) continue;
    const int32_t x = d.cell % pw;
    const int32_t y = d.cell / pw;
    if (x < r.x0 || y < r.y0 || x > r.x1 || y > r.y1) continue;

    float elapsed = d.elapsed_time;
    if (elapsed >= d.emit_interval) {
      GetReachableCells(w, t, x, y, d.range, &state->scratch);
      for (int32_t c : state->scratch.reachable) {
        const size_t cell = static_cast<size_t>(c);
        // The removal branch: only cells already carrying this disease.
        if (d.emit_count < 0 && w->DiseaseIdx(cell) != d.disease_idx) continue;
        AddDiseaseToCell(w, diseases, cell, d.disease_idx, d.emit_count);
      }
      // **Nothing is reported here.** `Update` resizes the list and never writes it: the
      // only writer is `UpdateDataListOnly`, the paused-game path, which puts
      // `{0xFF, 0}` in every slot. Measured — `germemit` first shipped with the obvious
      // accumulation in this spot and Klei answered `d255/0` for every emitter that had just
      // fired thousands of germs. A component that emits does not say so.
      elapsed -= d.emit_interval;
    }
    d.elapsed_time = elapsed + dt;
  }
}

}  // namespace oni_sim
