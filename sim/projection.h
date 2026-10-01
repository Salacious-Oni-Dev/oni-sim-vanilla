// The projection contract: how world state becomes GameDataUpdate.
//
// This is the entire mod-compatibility surface. Mods reach sim state through managed `Grid.Element[]`, `Grid.Mass[]` and
// `Grid.IsSolidCell`, all of which come from here. Compatibility is decided by this file,
// not by whether the physics matches Klei's — which is why it is implemented and tested
// before any physics exists.
//
// Rules:
//   1. mass[cell] is the *total* mass of the cell over all phases.
//   2. elementIdx[cell] is the phase with the largest volume fraction, ties broken by
//      element index so it cannot flicker.
//   3. Solidity is thresholded with hysteresis, so a cell at the boundary does not
//      toggle every frame.
//   4. temperature[cell] is the mass-weighted mean, so mass * shc * temperature still
//      sums to the cell's real internal energy.
//   5. Element and solidity changes must be *announced*. Grid.Element[] is a managed
//      cache updated only from substanceChangeInfo, and Grid.SetSolid fires only from
//      solidInfo. A silent change is a permanently stale cache.

#pragma once

#include <cstdint>
#include <vector>

#include "world.h"

namespace oni_sim {

// Solid fraction thresholds. A cell becomes solid at or above kSolidEnter and stops
// being solid at or below kSolidExit; between the two it keeps whatever it was. The gap
// is the whole point: pathfinding, UnstableGroundManager and every IsSolidCell consumer
// re-evaluate on each solidInfo event, so a cell oscillating around a single threshold
// would thrash all of them once volume fractions make partial solidity possible.
inline constexpr float kSolidEnter = 0.55f;
inline constexpr float kSolidExit = 0.45f;

// One byte of last frame's answer per cell, kept so this frame can tell what actually
// changed and emit exactly those events.
//
// It is a byte and not a struct because the only other thing worth remembering about a
// cell — the element it was — is already in `ProjectionBuffers::element`, which holds last
// frame's value right up until this frame overwrites it. Reading it back costs nothing:
// the line has to be fetched for the store in any case.
inline constexpr uint8_t kPrevSolid = 1 << 0;
inline constexpr uint8_t kPrevHadMass = 1 << 1;
// Set by the pass-through copy for the cells whose `properties` byte was written this
// frame, and cleared by the main loop when it has re-derived that cell. Without it a cell
// whose element and mass are untouched but which just had a door built in it would keep
// last frame's solidity forever.
inline constexpr uint8_t kPrevRecheck = 1 << 2;

// The unpadded, game-indexed arrays the game holds pointers into. The game does not
// cache these addresses: `Game.StepTheSim` re-reads every pointer out of a fresh
// `GameDataUpdate*` each frame, returned by PrepareGameData rather than by Start. So resizing between frames is legal, and a growing phase list may
// well want to. What is *not* legal is resizing partway through a frame, after
// BuildUpdate has published the pointers and before the game has finished reading them.
struct ProjectionBuffers {
  std::vector<uint16_t> element;
  std::vector<float> temperature;
  std::vector<float> mass;
  std::vector<uint8_t> properties;
  std::vector<uint8_t> insulation;
  std::vector<uint8_t> strength;
  std::vector<float> radiation;
  std::vector<uint8_t> disease_idx;
  std::vector<int32_t> disease_count;
  std::vector<uint16_t> backwall_element;
  std::vector<float> backwall_mass;
  std::vector<float> backwall_temperature;
  std::vector<float> accumulated_flow;

  std::vector<uint8_t> previous;

  void Allocate(size_t n) {
    element.assign(n, 0);
    temperature.assign(n, 0.0f);
    mass.assign(n, 0.0f);
    properties.assign(n, 0);
    insulation.assign(n, 0);
    strength.assign(n, 0);
    radiation.assign(n, 0.0f);
    disease_idx.assign(n, 0xFF);
    disease_count.assign(n, 0);
    backwall_element.assign(n, 0);
    backwall_mass.assign(n, 0.0f);
    backwall_temperature.assign(n, 0.0f);
    accumulated_flow.assign(n, 0.0f);
    previous.assign(n, 0);
  }
};

// Rule 1: total mass over every phase present.
inline float TotalMass(const PhaseEntry* phases, int count) {
  float m = 0.0f;
  for (int i = 0; i < count; ++i) m += phases[i].mass;
  return m;
}

// Rule 2: the phase occupying the largest share of the cell's volume. Volume, not mass —
// a kilogram of granite and a kilogram of hydrogen do not occupy remotely the same
// space, and picking by mass would report a cell as "granite" when it is one grain of
// sand in a room of gas. Ties go to the lower element index, which is stable across
// frames because the element table's ordering is.
// An empty cell still has an element — Vacuum is an element, with an index that is not
// zero — so the fallback is the cell's own stored phase, never index 0. Returning 0 here
// reported a vacuum shaft as element 0 (an unstable solid in the real table) and was
// caught by diffsim's `sunlit` scenario.
inline uint16_t DominantElement(const PhaseEntry* phases, int count,
                                const ElementTable& table) {
  // One phase is the common case, so it is
  // spelled out rather than reached through the general loop — which for one phase costs a
  // division and a scattered lookup into the element table to rank a list of one. The two
  // agree by inspection: with `count == 1` the loop either skips its body (massless) or
  // runs it once, and both leave `best` at `phases[0].element`.
  if (count == 1) return phases[0].element;
  uint16_t best = count > 0 ? phases[0].element : 0;
  float best_volume = -1.0f;
  for (int i = 0; i < count; ++i) {
    if (phases[i].mass <= 0.0f) continue;
    const float density = table.At(phases[i].element).molarMass;
    // molarMass is the closest thing the element table has to a density; a zero or
    // missing value would divide by zero, so such a phase is ranked by mass alone.
    const float volume = density > 0.0f ? phases[i].mass / density : phases[i].mass;
    if (volume > best_volume ||
        (volume == best_volume && phases[i].element < best)) {
      best_volume = volume;
      best = phases[i].element;
    }
  }
  return best;
}

// Rule 3: the share of the cell's volume held by solid phases.
inline float SolidFraction(const PhaseEntry* phases, int count,
                           const ElementTable& table) {
  // As above: one phase fills all of the cell it is in or none of it, so the general
  // `solid / total` is `volume / volume` and the density it divides by cancels.
  // MASS IS NOT A TERM, and this cost a divergence to learn. Klei's whole
  // solidity rule, at the top of `CopySimDataToGame`:
  // if the cell's element index changed, compare
  // `ElementTemperatureData::IsSolid` of the old index against the new one and announce a
  // `SolidInfo(cell, IsSolid(new))` when they differ. There is no mass test, no fraction and
  // no remembered bitmap anywhere in it — a cell of a solid element is solid whether it holds
  // 2000 kg or nothing at all.
  //
  // Mass is deliberately not tested. A cell whose element is solid and whose mass is exactly
  // zero (an ElementConsumer drains one to nothing) is solid to the game, so when it is later
  // refilled or replaced the transition is announced; testing `mass > 0` would call it hollow,
  // announce nothing, and leave `Grid.IsSolidCell` stale. `diffsim --scenario econsumeempty`
  // covers it.
  //
  // The hysteresis above it stays: it is what rule 3 in this file's header is for, and on a
  // single-phase cell the fraction is still 0 or 1 so the dead band cannot bite and
  // `ProjectSolid` reduces to exactly Klei's `IsSolid(element)`.
  if (count == 1) return table.IsSolid(phases[0].element) ? 1.0f : 0.0f;
  float solid = 0.0f, total = 0.0f;
  for (int i = 0; i < count; ++i) {
    if (phases[i].mass <= 0.0f) continue;
    const float density = table.At(phases[i].element).molarMass;
    const float volume = density > 0.0f ? phases[i].mass / density : phases[i].mass;
    total += volume;
    if (table.IsSolid(phases[i].element)) solid += volume;
  }
  // An empty phase list has no volume to weigh, so it falls back to the same rule the single
  // phase case uses: the element the cell reports is the one that decides, mass or no mass.
  if (total <= 0.0f) {
    return table.IsSolid(DominantElement(phases, count, table)) ? 1.0f : 0.0f;
  }
  return solid / total;
}

// Rule 3, with the dead band applied. `was_solid` is last frame's answer.
inline bool ProjectSolid(float solid_fraction, uint8_t properties, bool was_solid) {
  // An explicitly impermeable cell is solid regardless of what is in it — that is how
  // the game models doors and tempshift plates, and IsSolidCell reads the flag.
  if (properties & kSolidImpermeable) return true;
  if (was_solid) return solid_fraction > kSolidExit;
  return solid_fraction >= kSolidEnter;
}

// Rule 4: mass-weighted mean temperature. Weighting by mass alone rather than by heat
// capacity is what Klei's single-element cells effectively do, and it is what keeps
// mass * shc * temperature summing to the cell's energy when there is only one phase.
// With several phases of different specific heats this is an approximation, and the
// volume-fraction model will have to weight by heat capacity instead. Flagged here
// rather than silently left as a bug for later.
//
// The single-phase case has to be **exact**, not merely correct to a rounding: `m*T/m` is
// not `T` in float, and Klei — which has one phase per cell and simply reports its
// temperature — is not doing that division at all. One last bit is enough to matter,
// because `GasShuffle` keeps the coolest candidate and so a last-bit disagreement anywhere
// in a gas room eventually swaps a different pair of cells. Every scenario with gas in it
// was one ulp adrift within about five ticks purely because of this line, with the kernel
// underneath it already bit-exact.
inline float MeanTemperature(const PhaseEntry* phases, int count) {
  // One phase reports its own temperature whether or not it has any mass — which is what
  // both of the loop's exits below already do for `count == 1`.
  if (count == 1) return phases[0].temperature;
  float m = 0.0f, mt = 0.0f;
  int contributors = 0;
  int only = 0;
  for (int i = 0; i < count; ++i) {
    if (phases[i].mass <= 0.0f) continue;
    ++contributors;
    only = i;
    m += phases[i].mass;
    mt += phases[i].mass * phases[i].temperature;
  }
  if (contributors == 1) return phases[only].temperature;
  if (m > 0.0f) return mt / m;
  // A massless cell keeps the temperature it was given rather than dropping to 0 K.
  // Klei does the same, and the difference is visible: a vacuum shaft seeded at 293.15 K
  // reports 293.15 K, not absolute zero.
  return count > 0 ? phases[0].temperature : 0.0f;
}

// Rule 5: fill the game-indexed buffers and record what changed. The caller turns
// `changed_element` and `changed_solid` into substanceChangeInfo and solidInfo entries;
// nothing else in the sim is allowed to write those buffers, so there is exactly one
// place where a change can go unannounced.
// Klei publishes four separate lists for what is really one event, and the game subscribes
// to different ones from different places. Which lists a change lands in was measured by
// forcing one transition of each phase pairing and reading the counts back:
//
//   liquid -> gas     substance,                 liquid
//   liquid -> solid   substance, solidSubstance, liquid, solid
//   solid  -> liquid  substance, solidSubstance, liquid, solid
//   solid  -> solid   substance, solidSubstance
//   gas    -> liquid  substance,                 liquid
//
// which is exactly: `solidSubstance` when either side is solid, `liquid` when either side
// is liquid, and `solid` only when the cell's solidity actually flipped. A solid turning
// into a different solid produces no solidInfo, because nothing about its solidity changed.
struct ProjectionEvents {
  std::vector<SubstanceChangeInfo> substance;
  std::vector<SolidInfo> solid;
  std::vector<SolidSubstanceChangeInfo> solid_substance;
  std::vector<LiquidChangeInfo> liquid;

  void Clear() {
    substance.clear();
    solid.clear();
    solid_substance.clear();
    liquid.clear();
  }
};

inline void Project(const World& w, const ElementTable& table, ProjectionBuffers* out,
                    ProjectionEvents* events, bool first_frame) {
  // Seven of the thirteen arrays this fills are pass-throughs: properties, insulation,
  // strength, radiation, the two disease fields and the three backwall fields all arrive
  // from the game and leave again unchanged. Copying them for every cell cost 1.5 ms of a
  // 3.4 ms projection on a 512x768 grid, every frame, to reproduce bytes that were already
  // there — the buffers persist between frames and the game re-reads the same pointers, so
  // an unwritten array is not a stale array, it is the same array.
  //
  // `World` records every write to one of the seven by cell (see its Mutable* accessors),
  // so what is copied here is exactly the cells that were written. Not an approximation:
  // each copy is either performed or provably redundant.
  auto copy_static = [&](size_t i, size_t p) {
    out->properties[i] = w.Properties()[p];
    out->insulation[i] = w.Insulation()[p];
    out->strength[i] = w.Strength()[p];
    out->radiation[i] = w.Radiation()[p];
    out->disease_idx[i] = w.DiseaseIdx(p);
    out->disease_count[i] = w.Disease()[p].count;
    const int32_t bw_hash = w.Backwall()[p].elementHash;
    // Hash 0 is a backwall with no element (`ElementTable::BackwallHash`); Klei publishes 0xFFFF.
    out->backwall_element[i] = table.HasHash(bw_hash) ? table.IndexOfHash(bw_hash) : 0xFFFF;
    out->backwall_mass[i] = w.Backwall()[p].mass;
    out->backwall_temperature[i] = w.Backwall()[p].temperature;
    out->previous[i] |= kPrevRecheck;
  };
  // A property write changes what `ProjectSolid` answers for that cell, so the main loop
  // below has to reach it this frame and not merely whenever it is next in a dirty
  // rectangle. `kPrevRecheck` would survive the wait; the `solidInfo` announcement would be
  // a frame or more late, which is a building placing a tile and the game not hearing.
  if (first_frame || w.StaticDirtyAll()) {
    // Row-walked rather than `Padded(i)` per cell, for the reason the main loop below is:
    // `Padded` is a 64-bit division by a runtime width, and the game grid is one contiguous
    // row-major sweep in which the padded index is just the game index plus two per row.
    for (const CellWalk c : w.GameCells()) copy_static(c.game, c.padded);
    w.MarkProjectDirtyAll();
  } else {
    // The recorded cells are padded indices and may be border cells, which have no game
    // index at all — a building writing a property into the ring is not a cell the game
    // can see.
    for (const uint32_t p : w.StaticDirty()) {
      const int64_t i = w.GameIndex(p);
      if (!w.ValidGameCell(i)) continue;
      copy_static(static_cast<size_t>(i), p);
      w.MarkProjectDirty(p);
    }
  }
  w.ClearStaticDirty();

  // `Padded(i)` is a 64-bit division by a width that is only known at run time, and this
  // loop ran it once per cell: 0.44 ms of a 1.25 ms projection on a 512x768 grid, a third of
  // the kernel, to recompute an index that advances by one per cell and by two per row.
  auto project_cell = [&](size_t i, size_t p) {
    // Single-phase today. The volume-fraction model replaces this span with the cell's
    // real phase list and nothing below changes.
    const PhaseEntry& only = w.Phase(p);
    const PhaseEntry* phases = &only;
    const int count = 1;

    const uint16_t element = DominantElement(phases, count, table);
    const float total = TotalMass(phases, count);
    const uint8_t prev = out->previous[i];
    // Last frame's element, read out of the buffer this frame is about to overwrite.
    const uint16_t old_element = out->element[i];
    const bool was_solid = (prev & kPrevSolid) != 0;
    const bool has_mass = total > 0.0f;

    // Solidity was half of this kernel — 0.52 ms of 1.00 on a 512x768 grid — and almost
    // none of it was needed. With one phase in the cell `SolidFraction` is `has_mass &&
    // IsSolid(element)`, so the whole of `ProjectSolid` is a function of three things: the
    // element, whether the cell has any mass, and the cell's impermeable bit. Two of them
    // are already in hand, and the third is recorded for us: every write to `properties`
    // goes through `World::MutableProperties`, which is what the pass-through copy above
    // walks, so it sets `kPrevRecheck` on exactly the cells whose third input moved.
    //
    // So an unchanged cell keeps last frame's answer and does not read `properties` at
    // all — which is most of what makes this worth doing, since that read is a fourth
    // stream over the whole grid for a byte that almost never differs.
    //
    // The `count == 1` guard is not caution, it is the condition the argument rests on:
    // once a cell can be part solid the fraction moves continuously, the dead band in
    // `ProjectSolid` starts doing work, and "the element did not change" stops implying
    // "the answer did not change". It is a compile-time constant today, so the general
    // path costs nothing until something makes it reachable.
    //
    // `!first_frame` is not redundant with the recheck bit. It is what keeps a full
    // recompute an *independent* answer: a caller passing `first_frame` re-derives every
    // cell's solidity from the world rather than from anything this function remembered,
    // which is exactly what `bench --verify` compares the incremental result against.
    const bool solid_unchanged = count == 1 && !first_frame && element == old_element &&
                                 has_mass == ((prev & kPrevHadMass) != 0) &&
                                 (prev & kPrevRecheck) == 0;
    const bool solid =
        solid_unchanged
            ? was_solid
            : ProjectSolid(SolidFraction(phases, count, table), w.Properties()[p],
                           was_solid);

    out->element[i] = element;
    out->mass[i] = total;
    out->temperature[i] = MeanTemperature(phases, count);

    // On the first frame the managed caches are seeded from these arrays directly by
    // Sim.Start, so announcing every cell as changed would be a few hundred thousand
    // redundant events. After that, silence means "unchanged" and must be earned.
    if (!first_frame) {
      // `substanceChangeInfo` is the set of cells a kernel *wrote a substance into*, not the
      // set whose element differs, and the two are not the same list in either direction: a
      // gas shuffle swapping two cells of oxygen announces both of them as `183 -> 183`,
      // while the convection swap in `DoDensityDisplacement` moves mass and announces
      // nothing at all. Scanning the touch bitmap in game-cell order reproduces Klei's sort
      // and `std::unique` (see `World::TouchSubstance`) without doing either.
      if (w.SubstanceTouched(p)) {
        SubstanceChangeInfo s{};
        s.cellIdx = static_cast<int32_t>(i);
        s.oldElemIdx = old_element;
        s.newElemIdx = element;
        events->substance.push_back(s);
      }
      // The other three lists are gated on the element having actually changed —
      // `CopySimDataToGame` tests `oldElemIdx != newElemIdx` and skips all of
      // them when it does not hold — so they stay a diff even though substance no longer is.
      if (element != old_element) {
        const uint8_t old_phase = table.Phase(old_element);
        const uint8_t new_phase = table.Phase(element);
        if (old_phase == kStateSolid || new_phase == kStateSolid) {
          events->solid_substance.push_back(
              SolidSubstanceChangeInfo{static_cast<int32_t>(i)});
        }
        if (old_phase == kStateLiquid || new_phase == kStateLiquid) {
          events->liquid.push_back(LiquidChangeInfo{static_cast<int32_t>(i)});
        }
      }
      if (solid != was_solid) {
        SolidInfo s{};
        s.cellIdx = static_cast<int32_t>(i);
        s.isSolid = solid ? 1 : 0;
        events->solid.push_back(s);
      }
    }
    // kPrevRecheck is cleared here, having just been used.
    out->previous[i] = static_cast<uint8_t>((solid ? kPrevSolid : 0) |
                                            (has_mass ? kPrevHadMass : 0));
  };

  // The region bound. On the cluster shape — 512x768 with a quarter-sized region — the
  // rectangles something could have written cover about 37 % of the grid rather than the
  // 6.25 % the region fraction suggests, because `StepGasDisplacement` clamps its first row
  // with `min(min_y, 2)` and so drives every row from the world's third up to the region's
  // top. That is the honest bound and it is still most of the kernel.
  //
  // Row spans rather than rectangles, so the walk stays in increasing game index and
  // `substanceChangeInfo` stays sorted for free. See `World::MarkProjectDirtyRect`.
  if (first_frame || w.ProjectDirtyAll()) {
    for (const CellWalk cw : w.GameCells()) project_cell(cw.game, cw.padded);
  } else {
    const int32_t pw = w.PaddedWidth();
    const int32_t gw = w.GameWidth();
    const int32_t y1 = w.ProjectDirtyY1();
    for (int32_t y = w.ProjectDirtyY0(); y <= y1; ++y) {
      const int32_t x0 = w.ProjectRowX0(y);
      const int32_t x1 = w.ProjectRowX1(y);
      if (x1 < x0) continue;
      size_t p = static_cast<size_t>(y) * static_cast<size_t>(pw) + static_cast<size_t>(x0);
      size_t i = static_cast<size_t>(y - 1) * static_cast<size_t>(gw) +
                 static_cast<size_t>(x0 - 1);
      for (int32_t x = x0; x <= x1; ++x, ++p, ++i) project_cell(i, p);
    }
  }
  w.ClearProjectDirty();
}

}  // namespace oni_sim
