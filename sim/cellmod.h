// `ModifyCell`: the message the game sends when something outside the sim puts matter into
// a cell or takes it out. Digging fills, a pump's output, a duplicant exhaling, a building
// spawning its product, the debug tools, a solid melting into a cell the game owns — all of
// it arrives here, and `ProcessCellModifications` is the whole of it.
//
// It is in its own file because it is not one function. The message carries a `replaceType`
// and the three values pick three different subsystems:
//
//   * **1, Replace** — `ReplaceElement`. Overwrite the cell outright.
//   * **2, ReplaceAndDisplace** — `ReplaceAndDisplaceElement`. Push whatever
//     fluid is there into a neighbour first, then overwrite.
//   * **0, None** — *add* to the cell, dispatched on the phase of the element being added to
//     `AddGas`, `AddLiquid` or `AddSolid`. These
//     three are the displacement subsystem: each one has its own idea of where the matter
//     goes when the target cell will not take it, and none of the three agrees with the
//     other two.
//
// Everything here is pinned by tests against the shipped library rather than guessed, because
// almost none of it is guessable. The three that a reasonable implementation gets wrong:
//
//   * `ReplaceElement` announces the substance change **unconditionally** — even when the
//     element it writes is the element that was already there.
//   * The add path can put the matter in a **different cell** from the one the message
//     named, and `AddGas` and `AddLiquid` scan different neighbours in different orders.
//   * When there is nowhere at all for it to go, both of them **destroy mass**: either the
//     incoming matter is dropped and the blocking cell is thinned by that much, or the
//     blocking cell is emptied and only the remainder is added. That is a real sink, and
//     the ledger's `modified` bucket is charged across every one of those writes.

#ifndef ONI_SIM_CELLMOD_H
#define ONI_SIM_CELLMOD_H

#include <cstdint>
#include <vector>

#include "disease.h"
#include "physics.h"
#include "world.h"

namespace oni_sim {

// `ModifyCellMessage::replaceType`.
enum CellModReplaceType : uint8_t {
  kReplaceNone = 0,
  kReplaceElement = 1,
  kReplaceAndDisplace = 2,
};

// `ModifyCellMessage::addSubType`, and only `AddSolid` reads it. Only the two values below
// are acted on;
// **any other value makes the whole call a no-op**, which is worth knowing because the
// field is a byte and the other 254 values are silently inert.
enum CellModAddSubType : uint8_t {
  kDoVerticalDisplacement = 0,
  kOnlyIfSameElement = 1,
};

// `SIM_MAX_TEMPERATURE`, 0x461c4000. The same 10000 K ceiling `ModifyCellEnergy` clamps to.
constexpr float kCellModMaxTemperature = 10000.0f;

// `__real_00800000` — the smallest normal float, 1.17549435e-38, not a rounded epsilon.
// Three call sites compare a cell's remaining mass against it to decide whether the cell is
// empty enough to blank, and using anything friendlier here (1e-6, `FLT_EPSILON`) would
// blank cells Klei keeps.
constexpr float kCellModEmptyMass = 1.17549435e-38f;

// Everything the cell-modification paths need, bundled so the eight-argument Klei signatures
// do not have to be repeated at every level. `rotation` is the game's substep counter — the drain runs before the frame's substeps, so it is the value the *previous*
// frame left behind.
struct CellModContext {
  World* w = nullptr;
  const ElementTable* elements = nullptr;
  const DiseaseTable* diseases = nullptr;
  uint16_t rotation = 0;
};

// `ClearCellDisease` (`CellAccessor::ClearDisease`) lives in `physics.h`, beside
// `ModifyCellDiseaseCount`, since the off-gas path needs it too.

// `CellAccessor::SetDisease`. Overwrites rather than merges, and resets the
// infestation age and the growth remainder with it.
inline void SetCellDisease(const CellModContext& cm, size_t cell, uint8_t idx, int32_t count) {
  World* w = cm.w;
  w->MutableDiseaseIdx(cell) = idx;
  w->MutableDisease(cell).count = count;
  w->MutableDisease(cell).diseaseHash = idx == 0xFF ? 0 : cm.diseases->HashOf(idx);
  w->MutableDiseaseInfest(cell) = 0;
  w->MutableDiseaseAccum(cell) = 0.0f;
}

// `AddMassAndUpdateTemperature` with its disease half attached.
//
// `physics.h` has the same function without the last two arguments; its liquid movers merge
// the germs they carry with a separate `AddDiseaseToCell` after it. This
// form spells the merge out inside the call, the way `ModifyCell`'s paths use it.
//
// The zero branch is Klei's and is the surprise: when the total comes out at or below zero
// the cell's temperature is zeroed and its disease cleared, and **its mass is left alone**.
// It is also unreachable — see the note on the same branch in `physics.h`. Every caller
// here pours a non-negative amount in, so the branch only ever fires on a cell that is
// already empty and already cold, where all three of its writes are no-ops. Ablating it
// scores nothing, and that is the honest reading rather than a gap in the scenario.
inline void AddMassTemperatureDisease(const CellModContext& cm, size_t cell, float amount,
                                      float src_temp, uint8_t disease_idx,
                                      int32_t disease_count) {
  World* w = cm.w;
  PhaseEntry& c = w->Phase(cell);
  const float before = c.mass;
  const float total = before + amount;
  if (total > 0.0f) {
    const float mix = (before * c.temperature + src_temp * amount) / total;
    const float lo = c.temperature < src_temp ? c.temperature : src_temp;
    const float hi = c.temperature < src_temp ? src_temp : c.temperature;
    c.temperature = mix < lo ? lo : (mix > hi ? hi : mix);
    c.mass = total;
    AddDiseaseToCell(w, *cm.diseases, cell, disease_idx, disease_count);
  } else {
    c.temperature = 0.0f;
    ClearCellDisease(w, cell);
  }
  w->NoteModified(c.mass - before);
  w->MarkProjectDirty(cell);
}

// `CellSOA::SwapCells` now lives in `physics.h`, where the swap sites that need it are.

// `DisplaceLiquidSimple`. Splits a liquid cell evenly between whichever of the
// candidate cells will take it, then empties the source.
//
// Three details that are not obvious:
//
//   * The **0.01 kg floor** (0x3c23d70a) is on the *source*: a cell holding less than that
//     cannot be displaced at all, and the caller gets `false`.
//   * A candidate qualifies if it holds the **same element or is vacuum** — an element-index
//     test, not a mass test — and its `LiquidImpermeable` bit must be clear.
//   * The germs are split with a **ceiling**, `(count - 1 + n) / n`, so a cell with one germ
//     splitting four ways gives one germ to each of the four. Germs are created here.
inline bool DisplaceLiquidSimple(const CellModContext& cm, size_t cell, uint16_t element,
                                 const size_t* candidates, int count) {
  World* w = cm.w;
  std::vector<PhaseEntry>& cells = w->Phases();
  const std::vector<uint8_t>& props = w->Properties();
  const uint16_t vacuum = cm.elements->VacuumIndex();

  if (!(cells[cell].mass >= 0.01f)) return false;

  size_t taking[4];
  int n = 0;
  for (int i = 0; i < count && n < 4; ++i) {
    const size_t c = candidates[i];
    const uint16_t e = cells[c].element;
    if (!(e == element || e == vacuum)) continue;
    if (props[c] & kLiquidImpermeable) continue;
    taking[n++] = c;
  }
  if (n == 0) return false;

  const float share = cells[cell].mass / static_cast<float>(n);
  const int32_t germs = (w->Disease()[cell].count - 1 + n) / n;
  const float temp = cells[cell].temperature;
  const uint8_t disease_idx = w->DiseaseIdx(cell);
  for (int i = 0; i < n; ++i) {
    const size_t c = taking[i];
    AddMassTemperatureDisease(cm, c, share, temp, disease_idx, germs);
    cells[c].element = element;
    // Klei pushes the substance entry by hand here rather than through
    // `SimEvents::ChangeSubstance`, with both element indices left at 0xffff
    // for `CopySimDataToGame` to fill in, and ORs the cell's unstable countdown separately.
    // `TouchSubstance` is both of those.
    w->TouchSubstance(c);
  }
  ClearCell(w, *cm.elements, cell);
  w->TouchSubstance(cell);
  w->MarkProjectDirty(cell);
  return true;
}

// `DisplaceLiquid`. The liquid counterpart of `DisplaceGas`, and it is built
// on top of it: when no neighbour will take the liquid directly, it tries to clear a gas
// neighbour out of the way and then tries again.
//
// The neighbour order is **right, left, up, down** and the second pass starts at the substep
// counter's offset into it, the same rotation `DisplaceGas` uses.
inline bool DisplaceLiquid(const CellModContext& cm, size_t cell, uint16_t element) {
  World* w = cm.w;
  std::vector<PhaseEntry>& cells = w->Phases();
  const ElementTable& t = *cm.elements;
  if (!(cells[cell].mass > 0.0f)) return false;
  if (t.Phase(cells[cell].element) != kStateLiquid) return false;

  const size_t pw = static_cast<size_t>(w->PaddedWidth());
  const size_t around[4] = {cell + 1, cell - 1, cell + pw, cell - pw};
  if (DisplaceLiquidSimple(cm, cell, element, around, 4)) return true;

  for (int i = 0; i < 4; ++i) {
    const size_t c = around[(cm.rotation + i) & 3];
    if (t.Phase(cells[c].element) != kStateGas) continue;
    if (!DisplaceGas(w, t, c, cm.rotation, nullptr, false, cm.diseases)) continue;
    w->MarkProjectDirtyRect(static_cast<int32_t>(c % pw) - 1,
                            static_cast<int32_t>(c / pw) - 1,
                            static_cast<int32_t>(c % pw) + 1,
                            static_cast<int32_t>(c / pw) + 1);
    return DisplaceLiquidSimple(cm, cell, element, around, 4);
  }
  return false;
}

// The over-full `DisplaceLiquid` in `PostProcessCell`'s liquid branch, reached when none of the four `DoPressureBreak` calls broke anything. A liquid
// cell holding more than 1.5 * maxMass under a lighter liquid pushes that liquid out of the
// way and moves 1/2.01 of itself up into the cell. Every test, in Klei's order:
//
//   * this cell holds more than `1.5 * maxMass` (its own element's);
//   * the cell above holds less mass than this one, and is liquid;
//   * this cell's mass is above `max(maxMass, 1.01 * above)`, the larger picked by
//     `1.01 * above > maxMass`;
//   * `DisplaceLiquid` clears the cell above (its liquid goes to the cells around it -- which
//     can include this one, so this cell's mass is read again afterwards).
//
// Then the cell above becomes `m / 2.01` of this cell (0x3efeb9f3), at its temperature and
// element, with `(int)(count * (share / m))` of its germs and a fresh infestation age and
// growth remainder, and is announced; this cell loses the same mass and germs, and a germ
// count left at or under zero is cleared. The cell's turn ends there, skipping the off-gas
// draw. A refusal anywhere falls through to the rest of the turn.
inline bool DoOverfullDisplace(const CellModContext& cm, size_t cell) {
  World* w = cm.w;
  const ElementTable& t = *cm.elements;
  std::vector<PhaseEntry>& cells = w->Phases();
  const Element& e = t.At(cells[cell].element);
  const float m = cells[cell].mass;
  if (!(m > e.maxMass * 1.5f)) return false;
  const size_t above = cell + static_cast<size_t>(w->PaddedWidth());
  const float above_mass = cells[above].mass;
  if (!(above_mass < m)) return false;
  const uint16_t above_element = cells[above].element;
  if (t.Phase(above_element) != kStateLiquid) return false;
  const float heavier = above_mass * 1.01f;
  const float limit = heavier > e.maxMass ? heavier : e.maxMass;
  if (!(m > limit)) return false;
  if (!DisplaceLiquid(cm, above, above_element)) return false;

  const uint32_t share_bits = 0x3efeb9f3u;  // 1 / 2.01
  float share_of;
  memcpy(&share_of, &share_bits, sizeof(share_of));
  const float source = cells[cell].mass;
  const float share = source * share_of;
  const int32_t germs = static_cast<int32_t>(
      static_cast<float>(w->Disease()[cell].count) * (share / source));
  cells[above].mass = share;
  cells[above].temperature = cells[cell].temperature;
  cells[above].element = cells[cell].element;
  SetCellDisease(cm, above, w->DiseaseIdx(cell), germs);
  w->TouchSubstance(above);
  cells[cell].mass -= share;
  w->MutableDisease(cell).count -= germs;
  if (w->Disease()[cell].count <= 0) ClearCellDisease(w, cell);
  return true;
}

// `DisplaceGas` lives in `physics.h` and does not mark the projection, because every kernel
// that calls it is already inside a marked region. The drain is not, so the one call the
// message path makes has to say where it wrote.
inline bool DisplaceGasFromDrain(const CellModContext& cm, size_t cell, uint16_t element) {
  const size_t pw = static_cast<size_t>(cm.w->PaddedWidth());
  const bool moved =
      DisplaceGas(cm.w, *cm.elements, cell, cm.rotation, nullptr, false, cm.diseases);
  if (moved) {
    cm.w->MarkProjectDirtyRect(static_cast<int32_t>(cell % pw) - 1,
                               static_cast<int32_t>(cell / pw) - 1,
                               static_cast<int32_t>(cell % pw) + 1,
                               static_cast<int32_t>(cell / pw) + 1);
  }
  (void)element;
  return moved;
}

// The tail both `AddGas` and `AddLiquid` fall into when there is nowhere for the matter to
// go. It is the only place in the sim that destroys mass on
// purpose without calling it a leak.
//
//   * If the incoming mass is **less** than what is standing in the way, the incoming matter
//     is dropped on the floor and the blocker is thinned by exactly that much. Nothing is
//     added and nothing is announced.
//   * Otherwise the blocker is emptied, the cell takes the new element, and only the
//     **remainder** is added — with the germs scaled by the same fraction.
//
// `blank_fully` is the one difference between the two callers, and it is not cosmetic:
// `AddGas` calls `SimData::ClearCell` on a cell left empty, which resets the
// disease fields too, while `AddLiquid` only writes the vacuum element over it
// and leaves whatever germs the add just put there.
inline void AddIntoBlockedCell(const CellModContext& cm, size_t cell, uint16_t element,
                               float mass, float temperature, uint8_t disease_idx,
                               int32_t disease_count, bool blank_fully) {
  World* w = cm.w;
  PhaseEntry& c = w->Phase(cell);
  if (mass < c.mass) {
    const float before = c.mass;
    c.mass = before - mass;
    w->NoteModified(c.mass - before);
    w->MarkProjectDirty(cell);
    return;
  }
  const float displaced = c.mass;
  c.element = element;
  c.mass = 0.0f;
  c.temperature = 0.0f;
  w->NoteModified(-displaced);
  const float remaining = mass - displaced;
  AddMassTemperatureDisease(
      cm, cell, remaining, temperature, disease_idx,
      static_cast<int32_t>((remaining / mass) * static_cast<float>(disease_count)));
  if (c.mass <= kCellModEmptyMass) {
    if (blank_fully) {
      ClearCell(w, *cm.elements, cell);
    } else {
      c.element = cm.elements->VacuumIndex();
    }
  }
  w->TouchSubstance(cell);
  w->MarkProjectDirty(cell);
}

// `AddGas`.
inline void AddGas(const CellModContext& cm, size_t cell, uint16_t element, float mass,
                   float temperature, uint8_t disease_idx, int32_t disease_count) {
  World* w = cm.w;
  std::vector<PhaseEntry>& cells = w->Phases();
  const ElementTable& t = *cm.elements;

  // Same gas already there: a plain merge, and no announcement — the substance did not
  // change. This is the path nearly every emitter takes every frame.
  if (cells[cell].element == element) {
    AddMassTemperatureDisease(cm, cell, mass, temperature, disease_idx, disease_count);
    return;
  }

  const uint16_t here = cells[cell].element;
  const uint8_t phase = t.Phase(here);
  bool room = phase == kStateVacuum;
  if (phase == kStateGas) {
    room = DisplaceGasFromDrain(cm, cell, here);
  } else if (phase == kStateLiquid) {
    room = DisplaceLiquid(cm, cell, here);
  }
  // A solid is never displaced by a gas — the flag stays clear, which is why a gas emitter buried in rock does not carve itself a hole.
  if (room) {
    cells[cell].element = element;
    AddMassTemperatureDisease(cm, cell, mass, temperature, disease_idx, disease_count);
    w->TouchSubstance(cell);
    return;
  }

  // Blocked. Three neighbours only — **left, right, up**, never down — and the two tests are
  // not the same: a vacuum neighbour is *overwritten* outright, while a neighbour already
  // holding this gas is merged into.
  const size_t pw = static_cast<size_t>(w->PaddedWidth());
  const size_t around[3] = {cell - 1, cell + 1, cell + pw};
  const uint16_t vacuum = t.VacuumIndex();
  for (int i = 0; i < 3; ++i) {
    const size_t c = around[i];
    if (cells[c].element == vacuum) {
      const float before = cells[c].mass;
      cells[c].element = element;
      cells[c].mass = mass;
      cells[c].temperature = temperature;
      // Only these two disease fields, — the infestation age
      // and the growth remainder are deliberately left standing.
      w->MutableDiseaseIdx(c) = disease_idx;
      w->MutableDisease(c).count = disease_count;
      w->MutableDisease(c).diseaseHash =
          disease_idx == 0xFF ? 0 : cm.diseases->HashOf(disease_idx);
      w->NoteModified(cells[c].mass - before);
      w->MarkProjectDirty(c);
      w->TouchSubstance(c);
      return;
    }
    if (cells[c].element == element) {
      AddMassTemperatureDisease(cm, c, mass, temperature, disease_idx, disease_count);
      return;
    }
  }
  AddIntoBlockedCell(cm, cell, element, mass, temperature, disease_idx, disease_count, true);
}

// `AddLiquid`. Longer than `AddGas` because of the step at the front: when the
// named cell is **solid**, the liquid does not stay there — it retargets to the first
// non-solid of the four neighbours and everything after that happens at the new cell.
inline void AddLiquid(const CellModContext& cm, size_t cell, uint16_t element, float mass,
                      float temperature, uint8_t disease_idx, int32_t disease_count) {
  World* w = cm.w;
  std::vector<PhaseEntry>& cells = w->Phases();
  const ElementTable& t = *cm.elements;
  const size_t pw = static_cast<size_t>(w->PaddedWidth());

  if (cells[cell].element == element) {
    AddMassTemperatureDisease(cm, cell, mass, temperature, disease_idx, disease_count);
    return;
  }

  size_t target = cell;
  if (t.Phase(cells[cell].element) == kStateSolid) {
    // **left, right, down, up** — and note this is a different order from the scan below.
    const size_t look[4] = {cell - 1, cell + 1, cell - pw, cell + pw};
    for (int i = 0; i < 4; ++i) {
      if (t.Phase(cells[look[i]].element) != kStateSolid) {
        target = look[i];
        break;
      }
    }
  }

  const uint16_t here = cells[target].element;
  const uint8_t phase = t.Phase(here);
  // Void is the element whose cells swallow everything; it is checked by element **id**
  // rather than by phase, because its phase is vacuum and a phase test would let liquid in.
  const bool blocked = phase == kStateSolid || here == t.VoidIndex();
  bool room = false;
  bool scan = false;
  if (blocked) {
    scan = false;
  } else if (phase == kStateVacuum) {
    room = true;
  } else if (phase == kStateGas) {
    room = DisplaceGasFromDrain(cm, target, here);
    if (!room) {
      // The gas would not move. If there is liquid directly above, the two cells trade
      // places first and then the neighbour scan runs anyway.
      //
      // **Transcribed and unscored.** This is the one branch in the file no scenario
      // reaches, and the reason is structural rather than an oversight in `cellmodliq`:
      // the branch needs a gas cell that `DisplaceGas` refuses standing directly under a
      // liquid, and liquid resting on gas is precisely the arrangement the liquid mover
      // undoes. It survives at most one frame, and the drain runs *after* that frame's
      // substeps on every frame except the first — where nothing has been queued yet.
      // Sending on tick 0 and sealing the pocket with `LiquidImpermeable` were both tried;
      // ablating the swap still scores 0.000000 K and 0.000000 kg either way. Reaching it
      // needs a message that creates the arrangement and consumes it in the same drain,
      // which is two `ModifyCell`s in one frame and is the next thing to try.
      const size_t above = target + pw;
      if (t.Phase(cells[above].element) == kStateLiquid) {
        SwapCells(w, above, target);
        w->TouchSubstance(above);
        w->TouchSubstance(target);
      }
      scan = true;
    }
  } else if (phase == kStateLiquid) {
    room = DisplaceLiquid(cm, target, here);
    scan = !room;
  }

  if (scan) {
    // **right, left, up, down**, and only an exact element match counts — vacuum does not.
    const size_t look[4] = {target + 1, target - 1, target + pw, target - pw};
    for (int i = 0; i < 4; ++i) {
      if (cells[look[i]].element == element) {
        AddMassTemperatureDisease(cm, look[i], mass, temperature, disease_idx, disease_count);
        return;
      }
    }
  }

  if (room) {
    cells[target].element = element;
    AddMassTemperatureDisease(cm, target, mass, temperature, disease_idx, disease_count);
    w->TouchSubstance(target);
    return;
  }
  AddIntoBlockedCell(cm, target, element, mass, temperature, disease_idx, disease_count, false);
}

// `AddSolid`. The only one of the three that reads `addSubType`, and the two
// modes share nothing.
inline void AddSolid(const CellModContext& cm, size_t cell, uint16_t element, float mass,
                     float temperature, uint8_t disease_idx, int32_t disease_count,
                     uint8_t sub_type) {
  World* w = cm.w;
  std::vector<PhaseEntry>& cells = w->Phases();
  const ElementTable& t = *cm.elements;
  const int32_t pw = w->PaddedWidth();
  const int32_t ph = w->PaddedHeight();

  if (sub_type == kOnlyIfSameElement) {
    const uint16_t here = cells[cell].element;
    const uint8_t phase = t.Phase(here);
    if (phase == kStateSolid) {
      // A different solid refuses the whole message. This is the mode a falling-sand landing
      // uses when it must not overwrite the rock it hit.
      if (here != element) return;
    } else if (phase == kStateGas) {
      DisplaceGasFromDrain(cm, cell, here);
    } else if (phase == kStateLiquid) {
      DisplaceLiquid(cm, cell, here);
    }
    // The game never writes the element on this path — it goes straight from the
    // displacement to `AddMassAndUpdateTemperature`. Into a solid that matched there is
    // nothing to write; into a gas or liquid cell the displacement has just left the vacuum
    // element behind, and the cell ends up as vacuum holding mass until a later frame's
    // state transition sorts it out. Reproduced rather than repaired: it is observable.
    AddMassTemperatureDisease(cm, cell, mass, temperature, disease_idx, disease_count);
    w->TouchSubstance(cell);
    return;
  }
  if (sub_type != kDoVerticalDisplacement) return;

  const int32_t x = static_cast<int32_t>(cell) % pw;
  const int32_t y = static_cast<int32_t>(cell) / pw;

  // First, top up the cell below and the cell itself if either already holds this element,
  // each only as far as the element's `maxMass`, which the game takes from its post-process
  // copy of the element table; nothing else in this project reads that table, and the assumption
  // that it is a copy of `Element::maxMass` is what `solidfall` in `diffsim` exists to test.
  const float cap = t.At(element).maxMass;
  for (int d = -1; d <= 0; ++d) {
    const int32_t row = y + d;
    const size_t c = static_cast<size_t>(row) * pw + x;
    if (cells[c].element != element) continue;
    const float room = cap - cells[c].mass;
    if (!(room > 0.0f)) continue;
    const float give = mass <= room ? mass : room;
    AddMassTemperatureDisease(
        cm, c, give, temperature, disease_idx,
        static_cast<int32_t>((give / mass) * static_cast<float>(disease_count)));
    mass -= give;
  }
  if (mass <= 0.0f) return;
  if (y >= ph - 1) return;

  // Whatever is left goes into the first cell at or above `y` that is not solid, pushing any
  // fluid there out of the way. The scan stops at the top of the world, and a Void cell ends
  // it outright with the matter destroyed.
  for (int32_t row = y; row < ph - 1; ++row) {
    const size_t c = static_cast<size_t>(row) * pw + x;
    const uint16_t here = cells[c].element;
    if (t.Phase(here) == kStateSolid) continue;
    if (here == t.VoidIndex()) return;
    if (t.Phase(here) == kStateGas) {
      DisplaceGasFromDrain(cm, c, here);
    } else if (t.Phase(here) == kStateLiquid) {
      DisplaceLiquid(cm, c, here);
    }
    // An overwrite, not an add: the mass is written straight over whatever the
    // displacement left behind.
    const float before = cells[c].mass;
    cells[c].element = element;
    cells[c].mass = mass;
    cells[c].temperature = temperature;
    w->NoteModified(cells[c].mass - before);
    w->MarkProjectDirty(c);
    w->TouchSubstance(c);
    SetCellDisease(cm, c, disease_idx, disease_count);
    return;
  }
}

// `ReplaceElement`, `replaceType` 1. The simple one, and the one whose
// announcement is unconditional: a cell replaced with the element it already held is still
// reported to the game as a substance change.
inline void ReplaceElementAt(const CellModContext& cm, size_t cell, uint16_t element,
                             float mass, float temperature, uint8_t disease_idx,
                             int32_t disease_count) {
  World* w = cm.w;
  PhaseEntry& c = w->Phase(cell);
  const float before = c.mass;
  c.element = element;
  // Vacuum takes neither. Both tests are against the element being *written*, and they are
  // two separate tests of the same condition.
  if (cm.elements->Phase(element) == kStateVacuum) {
    temperature = 0.0f;
    mass = 0.0f;
  }
  c.temperature = temperature;
  c.mass = mass;
  w->NoteModified(c.mass - before);
  w->MarkProjectDirty(cell);
  w->TouchSubstance(cell);
  SetCellDisease(cm, cell, disease_idx, disease_count);
}

// `ReplaceAndDisplaceElement`, `replaceType` 2. `ReplaceElement` with the
// existing fluid pushed into a neighbour first — and with a temperature clamp of its own,
// which `ReplaceElement` does not have.
inline void ReplaceAndDisplaceElementAt(const CellModContext& cm, size_t cell,
                                        uint16_t element, float mass, float temperature,
                                        uint8_t disease_idx, int32_t disease_count) {
  World* w = cm.w;
  const ElementTable& t = *cm.elements;
  const uint16_t here = w->Phase(cell).element;
  const uint8_t phase = t.Phase(here);
  // The return value is **ignored**, as in the game. The cell is
  // overwritten whether or not the fluid found anywhere to go.
  if (phase == kStateGas) {
    DisplaceGasFromDrain(cm, cell, here);
  } else if (phase == kStateLiquid) {
    DisplaceLiquid(cm, cell, here);
  }

  PhaseEntry& c = w->Phase(cell);
  const float before = c.mass;
  c.element = element;
  if (t.Phase(element) == kStateVacuum) mass = 0.0f;
  c.mass = mass;
  if (t.Phase(element) == kStateVacuum) temperature = 0.0f;
  // Unconditional here, unlike the caller's clamp, which only fires on the warning path.
  if (temperature >= kCellModMaxTemperature) temperature = kCellModMaxTemperature;
  if (temperature <= 0.0f) temperature = 0.0f;
  c.temperature = temperature;
  w->NoteModified(c.mass - before);
  w->MarkProjectDirty(cell);
  w->TouchSubstance(cell);
  SetCellDisease(cm, cell, disease_idx, disease_count);
}

// The `replaceType` 0 removal path. A negative mass takes
// matter *out* of a cell, and it only works when the cell's current phase matches the phase
// of the element named in the message.
//
// The gas branch enters on `mass <= 0` while the solid and liquid branches enter on
// `mass < 0`, so a zero-mass message is a no-op for solids and liquids and a "clear me if
// I am empty" for gases. That asymmetry is the game's own behaviour.
inline void RemoveFromCell(const CellModContext& cm, size_t cell, uint8_t phase, float mass) {
  World* w = cm.w;
  PhaseEntry& c = w->Phase(cell);
  if (cm.elements->Phase(c.element) != phase) return;
  const float before = c.mass;
  const float left = before + mass;
  c.mass = left > 0.0f ? left : 0.0f;
  w->NoteModified(c.mass - before);
  w->MarkProjectDirty(cell);
  if (c.mass <= kCellModEmptyMass) {
    ClearCell(w, *cm.elements, cell);
    w->TouchSubstance(cell);
  }
}

}  // namespace oni_sim

#endif  // ONI_SIM_CELLMOD_H
