// Germs: how they spread between cells, and how they grow and die inside one.
//
// Klei's `Disease` class. Three entry points matter and this
// file is all three:
//
//   * `Disease::UpdateCells` — one pair of neighbouring cells. Called from
//     `SimBase::UpdateData`, once for the right neighbour
//     and once for the one above, over the whole region, after the liquid sweep.
//   * `Disease::PostProcess` — the growth and decay sweep, plus the pass that
//     ages every cell's infestation counter. Called once per region, the
//     last thing `UpdateData` does before the substep counter turns over.
//   * `Disease::AddDiseaseToCell` — the only writer. Both of the above go
//     through it, and so does every message that puts germs into a cell.
//
// What is *not* here: `DiseaseEmitter` and `DiseaseConsumer` are components, registered by handle the way buildings are, and they are what
// makes a slime tile or a duplicant produce germs in the first place. Nothing in this file
// creates disease; it only moves, grows and kills what a message put there. The radiation
// block — which looks up `Radioactive Contaminants` by hash and seeds it into
// irradiated cells — belongs to the radiation subsystem and is likewise absent.
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "world.h"

namespace oni_sim {

// A half-life of infinity means "never decays" and
// short-circuits to a factor of exactly 1 rather than going through `powf`, which would
// return 1 anyway but only for the finite inputs.
inline const float kDiseaseNeverDecays = std::numeric_limits<float>::infinity();

// One substep, negated, and the base. The game spells the decay `2^(-0.2/halfLife)` rather than the `0.5^(0.2/halfLife)` it is
// mathematically, and the same -0.2 is reused as the plain multiplier on the
// under-population death rate — so that rate is a positive kill and is *subtracted*.
inline constexpr float kDiseaseDecayBase = 2.0f;
inline constexpr float kDiseaseNegSubstep = -0.20000000298023224f;

// The only constant the pair sweep has. A neighbour pair moves an eighth of
// the count that separates them, scaled by the element's diffusion rate.
inline constexpr float kDiseaseDiffusionShare = 0.125f;

// Klei's own cap: the infestation counter saturates at 254 rather than wrapping
// through 255, which is the value `minDiffusionInfestationTickCount` is compared against.
inline constexpr uint8_t kMaxInfestationTicks = 0xFE;

// A half-life turned into the fraction that survives one substep. The two special cases are
// tested against 0 and against +inf, both before the call, so a
// zero half-life kills everything outright and an infinite one changes nothing.
inline float DiseaseDecayFactor(float half_life) {
  if (half_life == 0.0f) return 0.0f;
  if (half_life == kDiseaseNeverDecays) return 1.0f;
  return std::pow(kDiseaseDecayBase, kDiseaseNegSubstep / half_life);
}

// How much of a cell's germ count survives one substep at this temperature.
//
// `temperature_range` is `{minViable, minGrowth, maxGrowth, maxViable}` and the sweep walks
// it for the first entry the temperature does not exceed. The band between minGrowth and
// maxGrowth is the one case that returns 1 without looking at a half-life at all; below and
// above it the half-life is interpolated between the two entries that bracket the
// temperature, and outside the outermost entries it is held flat.
//
// The walk is spelled the way the game spells it because the ends matter. The loop breaks on
// an unordered compare, so a NaN temperature lands in the first band rather than falling off
// the end; and the `i - 1` that becomes -1 at the first entry is
// what makes the below-minViable case interpolate a range of zero width and so read a
// single half-life.
inline float DiseaseTemperatureFactor(const DiseaseInfo& d, float temperature) {
  int hit = 3;      // `r8`, the upper bracket
  int reached = 3;  // `r9d`, how many entries the temperature cleared
  int lower = 3;    // `rdx`, the lower bracket before the clamp
  for (int i = 0; i < 4; ++i) {
    if (!(temperature > d.temperature_range[i])) {
      lower = i - 1;
      reached = i;
      hit = i;
      break;
    }
  }
  const int lo_idx = lower >= 0 ? lower : hit;
  // The growth band. `lo_idx == 1 && reached == 2` is the temperature sitting between
  // minGrowth and maxGrowth, and it is the only exit that never reads a half-life.
  if (lo_idx == 1 && reached == 2) return 1.0f;

  const float hl_lo = d.temperature_half_lives[lo_idx];
  const float hl_hi = d.temperature_half_lives[hit];
  if (hl_lo == kDiseaseNeverDecays) return 1.0f;
  if (hl_hi == kDiseaseNeverDecays) return 1.0f;

  const float lo = d.temperature_range[lo_idx];
  const float span = d.temperature_range[hit] - lo;
  float t = 0.0f;
  if (span > 0.0f) t = (temperature - lo) / span;
  const float half_life = (1.0f - t) * hl_lo + t * hl_hi;
  return DiseaseDecayFactor(half_life);
}

// `Disease::CalculateFinalDiseaseCount`. The merge rule on its own, with no
// cell attached: given what a container already holds and what is arriving, which disease
// survives and how many germs of it there are.
//
// The rule is not symmetric. Each side's claim is its count times its `strength`, and:
//
//   * If **A**'s claim is the larger one, the survivor is `countB - (claimA / claimB) * countA`
//     taken as an absolute value, with the sign choosing which of the two diseases it is.
//   * Otherwise B is dropped on the floor and A is returned unchanged. Not "the stronger one
//     wins" — A wins the tie *and* every case where B is stronger.
//
// This is the same arithmetic the merge inside `AddDiseaseToCell` does, so the two share one
// function here; the standalone entry point
// exists so that `do_remove` can merge into a `ConsumedMassInfo`, which is not a cell.
struct DiseaseMerge {
  uint8_t idx;
  int32_t count;
  // False only when the table cannot resolve one of the two indices, which is what a world
  // loaded before the disease table arrives looks like. Klei has no such check — its
  // `operator[]` is unchecked — so this is a guard rather than a behaviour.
  bool valid;
};

inline DiseaseMerge MergeDiseaseCounts(const DiseaseTable& table, uint8_t idx_a,
                                       int32_t count_a, uint8_t idx_b, int32_t count_b) {
  if (idx_a == idx_b) return DiseaseMerge{idx_a, count_b + count_a, true};
  if (idx_a == 0xFF) return DiseaseMerge{idx_b, count_b, true};
  if (idx_b == 0xFF) return DiseaseMerge{idx_a, count_a, true};

  const DiseaseInfo* a = table.Get(idx_a);
  const DiseaseInfo* b = table.Get(idx_b);
  if (a == nullptr || b == nullptr) return DiseaseMerge{idx_a, count_a, false};

  const float claim_a = static_cast<float>(count_a) * a->strength;
  const float claim_b = static_cast<float>(count_b) * b->strength;
  if (claim_b < claim_a) {
    const float ratio = claim_a / claim_b;
    const float left = static_cast<float>(count_b) - ratio * static_cast<float>(count_a);
    const int32_t signed_left = static_cast<int32_t>(left);
    return DiseaseMerge{signed_left < 0 ? idx_a : idx_b,
                        signed_left < 0 ? -signed_left : signed_left, true};
  }
  if (count_a < 0) return DiseaseMerge{idx_b, -count_a, true};
  return DiseaseMerge{idx_a, count_a, true};
}

// `Disease::AddDiseaseToCell`. Merges `count` germs of `idx` into whatever the
// cell already holds and writes the result back.
//
// The merge is the interesting part, and it is not symmetric. When the two diseases differ,
// each side's claim on the cell is its count times its `strength`, and:
//
//   * If the **resident** claim is the larger one, the incoming germs are consumed and the
//     survivor is `residentCount * (residentClaim / incomingClaim) - incomingCount`, taken
//     as an absolute value with whichever disease the sign picks out.
//   * Otherwise the incoming germs are dropped on the floor and the cell is left exactly as
//     it was. Not "the stronger one wins" — the resident wins the tie *and* every case where
//     the newcomer is stronger. `CalculateFinalDiseaseCount` is the same
//     arithmetic in a standalone function and has the same asymmetry, so this is Klei's
//     rule rather than an artefact of one call site.
//
// A count that reaches zero or below clears the cell outright, and a cell whose disease
// *changes identity* has its infestation age reset while one that merely gains germs of the
// disease it already had does not.
inline void AddDiseaseToCell(World* w, const DiseaseTable& table, size_t cell, uint8_t idx,
                             int32_t count) {
  const uint8_t cur_idx = w->DiseaseIdx(cell);
  const int32_t cur_count = w->Disease()[cell].count;

  // The resident is A and the newcomer is B. The `idx == 0xFF` case falls out of the shared
  // rule as "keep what you have", and still falls through to the write and the zero test
  // below, as the game's sim does.
  const DiseaseMerge m = MergeDiseaseCounts(table, cur_idx, cur_count, idx, count);
  if (!m.valid) return;
  const uint8_t final_idx = m.idx;
  const int32_t final_count = m.count;

  w->MutableDiseaseIdx(cell) = final_idx;
  w->MutableDisease(cell).count = final_count;
  w->MutableDisease(cell).diseaseHash =
      final_idx == 0xFF ? 0 : table.HashOf(final_idx);

  if (final_count > 0) {
    if (final_idx != cur_idx) w->MutableDiseaseInfest(cell) = 0;
    return;
  }
  w->MutableDiseaseIdx(cell) = 0xFF;
  w->MutableDisease(cell).count = 0;
  w->MutableDisease(cell).diseaseHash = 0;
  w->MutableDiseaseInfest(cell) = 0;
  w->MutableDiseaseAccum(cell) = 0.0f;
}

// `Disease::GetDiffusionScale`. How fast this cell lets germs out, or zero if
// it is not infested enough to let any out at all.
//
// Both gates are per element and per disease: a cell needs at least `minDiffusionCount`
// germs, and it must have held them for at least `minDiffusionInfestationTickCount`
// substeps. The second is why the infestation counter exists — it is what stops a single
// germ landing in a tile and immediately smearing across the map.
inline float DiseaseDiffusionScale(const World& w, const ElementTable& elements,
                                   const DiseaseTable& table,
                                   const std::vector<DiseaseEntry>& start, size_t cell) {
  (void)elements;
  // **The snapshot, not the live grid.** `Disease::GetDiffusionScale` reads its
  // index, its count and its infestation age from `param_1->cells` — the copy taken before
  // the sweep — while every write the sweep makes goes to `updatedCells`. Reading live here
  // costs a cell its diffusion the moment one transfer drops it under `minDiffusionCount`:
  // granite's threshold is exactly 1,000,000, so a cell seeded with a round million moved
  // germs to the *first* of its four neighbours and then refused the other three, which is
  // `germthresh` and the fringe half of `germemit`. Klei prices all four transfers against
  // the same pre-sweep number.
  const uint8_t idx = start[cell].idx;
  const DiseaseInfo* d = table.Get(idx);
  if (d == nullptr) return 0.0f;
  const uint16_t elem = w.Phase(cell).element;
  if (elem >= d->growth.size()) return 0.0f;
  const ElemGrowthInfo& g = d->growth[elem];
  if (start[cell].count < g.minDiffusionCount) return 0.0f;
  if (start[cell].infest < g.minDiffusionInfestationTickCount) return 0.0f;
  return g.diffusionScale;
}

// One pair of neighbouring cells, `Disease::UpdateCells`.
//
// Four cases, and every one of them ends in the same two `AddDiseaseToCell` calls: a
// quantity leaves one cell and the same quantity arrives at the other, so the pair conserves
// germs exactly even when the two cells hold different diseases.
//
//   * Same disease on both sides: an eighth of the difference moves, scaled by the diffusion
//     rate **of the richer cell**, and the direction follows the sign.
//   * One side clean: an eighth of the infected side's whole count moves out.
//   * Both infected, different diseases: whichever side has the larger `count * strength`
//     exports an eighth of its own count into the other, where `AddDiseaseToCell` resolves
//     the collision.
//
// The phase gate comes first and is absolute: the two cells' elements must be in the same
// state, so germs never cross between a gas and the liquid it is bubbling through, or out of
// a solid into the air above it: the low two bits (the state) of the two elements' state
// bytes must be equal.
inline void DiseaseUpdatePair(World* w, const ElementTable& elements,
                              const DiseaseTable& table,
                              const std::vector<DiseaseEntry>& start, size_t a, size_t b) {
  const uint8_t state_a = elements.At(w->Phase(a).element).state & 3;
  const uint8_t state_b = elements.At(w->Phase(b).element).state & 3;
  if (state_a != state_b) return;

  const uint8_t idx_a = start[a].idx;
  const uint8_t idx_b = start[b].idx;

  int32_t to_a = 0;
  int32_t to_b = 0;
  uint8_t give_a = idx_a;
  uint8_t give_b = idx_b;

  if (idx_a == idx_b) {
    if (idx_a == 0xFF) return;
    const int32_t diff = start[b].count - start[a].count;
    // The richer cell's element is what sets the rate, so a germ-rich tile next to bare
    // rock diffuses at the tile's rate rather than the rock's.
    const size_t source = diff > 0 ? b : a;
    const float scale = DiseaseDiffusionScale(*w, elements, table, start, source);
    to_a = static_cast<int32_t>(scale * (static_cast<float>(diff) * kDiseaseDiffusionShare));
    to_b = -to_a;
    give_a = idx_a;
    give_b = idx_b;
  } else if (idx_a == 0xFF) {
    const float scale = DiseaseDiffusionScale(*w, elements, table, start, b);
    to_a = static_cast<int32_t>(static_cast<float>(start[b].count) *
                                kDiseaseDiffusionShare * scale);
    to_b = -to_a;
    give_a = idx_b;
    give_b = idx_b;
  } else if (idx_b == 0xFF) {
    const float scale = DiseaseDiffusionScale(*w, elements, table, start, a);
    const int32_t amount = static_cast<int32_t>(static_cast<float>(start[a].count) *
                                                kDiseaseDiffusionShare * scale);
    to_a = -amount;
    to_b = amount;
    give_a = idx_a;
    give_b = idx_a;
  } else {
    const DiseaseInfo* da = table.Get(idx_a);
    const DiseaseInfo* db = table.Get(idx_b);
    if (da == nullptr || db == nullptr) return;
    const float claim_a = static_cast<float>(start[a].count) * da->strength;
    const float claim_b = static_cast<float>(start[b].count) * db->strength;
    if (claim_a > claim_b) {
      const float scale = DiseaseDiffusionScale(*w, elements, table, start, a);
      const int32_t amount = static_cast<int32_t>(static_cast<float>(start[a].count) *
                                                  scale * kDiseaseDiffusionShare);
      to_a = -amount;
      to_b = amount;
      give_a = idx_a;
      give_b = idx_a;
    } else {
      const float scale = DiseaseDiffusionScale(*w, elements, table, start, b);
      to_a = static_cast<int32_t>(static_cast<float>(start[b].count) * scale *
                                  kDiseaseDiffusionShare);
      to_b = -to_a;
      give_a = idx_b;
      give_b = idx_b;
    }
  }

  if (to_a != 0) AddDiseaseToCell(w, table, a, give_a, to_a);
  if (to_b != 0) AddDiseaseToCell(w, table, b, give_b, to_b);
}

// The pair sweep. `SimBase::UpdateData`'s inner loop: every cell in the
// region against the one to its right and the one above it, skipping a pair only when both
// halves are clean.
//
// It prices against `SimData::cells`, and that is the copy `UpdateData` takes,
// after both liquid sweeps: the germs the liquid carried this substep are already in it. So
// the snapshot is retaken here, over the same rectangle `StepFlow` took its own.
inline void StepDiseaseDiffusion(World* w, const ElementTable& elements,
                                 const DiseaseTable& table, size_t ri) {
  const World::PaddedRect& r = w->PaddedRegion(ri);
  const int32_t pw = w->PaddedWidth();
  RefreshDiseaseSnapshot(DiseaseSweepStart(), *w, ri, pw, w->PaddedHeight(), r.x0, r.y0, r.x1,
                         r.y1);
  const std::vector<DiseaseEntry>& start = DiseaseSweepStart();
  if (start.size() != w->PaddedCount()) return;
  for (int32_t y = r.y0; y < r.y1; ++y) {
    for (int32_t x = r.x0; x < r.x1; ++x) {
      const size_t c = static_cast<size_t>(y) * pw + x;
      if (start[c].idx != 0xFF || start[c + 1].idx != 0xFF) {
        DiseaseUpdatePair(w, elements, table, start, c, c + 1);
      }
      if (start[c].idx != 0xFF || start[c + pw].idx != 0xFF) {
        DiseaseUpdatePair(w, elements, table, start, c, c + pw);
      }
    }
  }
}

// `Disease::PostProcess`. Two passes over the region: growth and decay, then
// the ageing sweep that clears anything the first pass took to zero.
//
// The first pass computes one signed delta per cell and holds the fraction back:
//
//     delta  = count * temperatureFactor - count          // temperature
//            + (populationFactor - 1) * count             // crowding, one of three rules
//            - radiation * radiationKillRate              // only when radiation is on
//            + whatever the last substep could not spend
//
// then truncates it towards zero, adds the whole part to the count and keeps the remainder
// in the cell's accumulator. That accumulator is the reason a disease with a half-life
// measured in minutes does anything at all at 0.2 s a substep: without it every delta would
// truncate to zero and nothing would ever decay.
//
// The crowding rule is a three-way branch on the cell's *mass*, not on its count alone:
// below `mass * minCountPerKG` the germs die at a flat rate per second; above
// `mass * maxCountPerKG` they decay on `overPopulationHalfLife`; in between they decay on
// `populationHalfLife`. So the same germ count is thriving in a wisp of gas and dying in a
// dense one.
inline void StepDiseasePostProcess(World* w, const ElementTable& elements,
                                   const DiseaseTable& table, bool radiation_enabled,
                                   size_t ri) {
  (void)elements;
  const World::PaddedRect& r = w->PaddedRegion(ri);
  const int32_t pw = w->PaddedWidth();

  for (int32_t y = r.y0; y < r.y1; ++y) {
    for (int32_t x = r.x0; x < r.x1; ++x) {
      const size_t c = static_cast<size_t>(y) * pw + x;
      const uint8_t idx = w->DiseaseIdx(c);
      if (idx == 0xFF) continue;
      const DiseaseInfo* d = table.Get(idx);
      if (d == nullptr) continue;

      const PhaseEntry& cell = w->Phase(c);
      if (cell.element >= d->growth.size()) continue;
      const ElemGrowthInfo& g = d->growth[cell.element];

      const float temp_factor = DiseaseTemperatureFactor(*d, cell.temperature);
      const float count = static_cast<float>(w->Disease()[c].count);
      float delta = count * temp_factor + w->DiseaseAccum()[c] - count;

      const float min_count = cell.mass * g.minCountPerKG;
      bool crowded = true;
      if (!(min_count > count)) {
        if (!(count > cell.mass * g.maxCountPerKG)) {
          delta += (DiseaseDecayFactor(g.populationHalfLife) - 1.0f) * count;
          crowded = false;
        }
      }
      if (crowded) {
        // Re-read rather than reuse `count`: the same value here, but read back from the grid
        // where the two branches rejoin, as the game's sim does.
        const float again = static_cast<float>(w->Disease()[c].count);
        if (again < min_count) {
          delta += g.underPopulationDeathRate * kDiseaseNegSubstep;
        } else {
          delta += (DiseaseDecayFactor(g.overPopulationHalfLife) - 1.0f) * count;
        }
      }

      if (radiation_enabled) {
        delta -= w->Radiation()[c] * d->radiation_kill_rate;
      }

      const int32_t whole = static_cast<int32_t>(delta);
      w->MutableDiseaseAccum(c) = delta - static_cast<float>(whole);
      w->MutableDisease(c).count += whole;
    }
  }

  // The ageing pass. Separate from the first because it also has to catch the
  // cells the first pass emptied — and it runs over every cell in the region, infected or
  // not, so a cell that was cleaned out keeps counting from zero.
  for (int32_t y = r.y0; y < r.y1; ++y) {
    for (int32_t x = r.x0; x < r.x1; ++x) {
      const size_t c = static_cast<size_t>(y) * pw + x;
      if (w->Disease()[c].count <= 0) {
        w->MutableDiseaseIdx(c) = 0xFF;
        w->MutableDisease(c).count = 0;
        w->MutableDisease(c).diseaseHash = 0;
        w->MutableDiseaseInfest(c) = 0;
        w->MutableDiseaseAccum(c) = 0.0f;
      }
      const int32_t next = w->DiseaseInfest()[c] + 1;
      w->MutableDiseaseInfest(c) =
          next < kMaxInfestationTicks ? static_cast<uint8_t>(next) : kMaxInfestationTicks;
    }
  }
}

}  // namespace oni_sim
