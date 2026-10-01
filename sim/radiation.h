// The radiation emitter: the sim's **third** component, and the only one that writes a
// field no kernel of ours had ever written.
//
// `radiation` was a pass-through array — the game set a cell's rads with a
// `CellRadiationModification` and read the same number back. It is not: a registered
// emitter paints rads into the grid every substep, and the shape it paints is a rasterised
// ellipse, attenuated along each ray by everything the ray passes through.
//
// Six behaviours here are easy to get wrong:
//
//   * **`tickConstant`'s scan box is square in `radiusX`.** Its rows run from `cy - radiusY`
//     for `2 * radiusX + 1` of them, so an emitter that is wider than it is tall scans past
//     the bottom of its own ellipse, and one that is taller than it is wide never reaches
//     the top of it. The ellipse test inside then refuses whatever the box got wrong, which
//     is why a tall emitter emits a *clipped* ellipse rather than a wrong one.
//   * **`Register` clamps `emitSpeed` to `emitRads`; `Modify` clamps it to `emitRate`.**
//     They are against different fields because the two messages have different layouts.
//   * **The attractor reports only the *last* cell it drained.** `setAttractor` writes
//     `*out = taken` rather than accumulating, and `Update` then adds that one number back
//     into the emitter's own cell.
//   * **`inRadialRange` treats an angle of exactly 360 as "no cone at all"** and short-
//     circuits before any trigonometry; the emitter's own cell is always in range.
//   * **The noise is only drawn for the outer 3/4 of a constant emitter.** `tickConstant`
//     advances the shared LCG only when the falloff is below 0.25, so the emitter's random
//     draws depend on its geometry, and the gas shuffle downstream of it moves when they do.
//   * **`RadialBeams` (type 4) does nothing.** It has no case in the switch, so the emitter
//     still steps its phase and its timers and paints nothing.
//
// The whole subsystem works in **padded** coordinates: `Register` converts the game cell on
// the way in, every guard is against the padded width and height, and the region test is a
// point test on the emitter's own cell.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../abi/sim_abi.h"
#include "buildings.h"
#include "disease.h"
#include "world.h"

namespace oni_sim {

// `RadiationEmitterType`, the game's emitter kinds.
enum : int32_t {
  kRadiationConstant = 0,
  kRadiationPulsing = 1,
  kRadiationPulsingAveraged = 2,
  kRadiationSimplePulse = 3,
  kRadiationRadialBeams = 4,
  kRadiationAttractor = 5,
};

// `RadiationEmitterData`, 44 bytes. Every field is either sent or zero: unlike the element
// emitter there are no surprising registration constants, and an emitter with `emitRads`
// zero or both radii zero is skipped whole before its timers move.
struct RadiationEmitterData {
  int32_t cell = 0;  // padded
  uint16_t radius_x = 0;
  uint16_t radius_y = 0;
  float emit_rads = 0.0f;
  float emit_rate = 0.0f;
  float emit_speed = 0.0f;
  float emit_direction = 0.0f;
  float emit_angle = 0.0f;
  float emit_timer = 0.0f;
  float emit_step_timer = 0.0f;
  int32_t emit_type = kRadiationConstant;
  int32_t emit_step = 0;
};

// The 5x5 stencil a radioactive element sprays into, the game's
// `adjacentOffsets`. It is a weight
// map, not a normalised kernel: the weights sum to 8.6 (1 + 4 x 0.75 + 4 x 0.5 + 4 x 0.25 +
// 8 x 0.15 + 4 x 0.1), so a lump of uranium puts 8.6 times its own `radiationPer1000Mass`
// into the world every substep.
struct RadiationOffset {
  float dx;
  float dy;
  float weight;
};
inline const RadiationOffset* RadiationOffsets() {
  static const RadiationOffset kOffsets[25] = {
      {-2.0f, -2.0f, 0.10f}, {-1.0f, -2.0f, 0.15f}, {0.0f, -2.0f, 0.25f},
      {1.0f, -2.0f, 0.15f},  {2.0f, -2.0f, 0.10f},  {-2.0f, -1.0f, 0.15f},
      {-1.0f, -1.0f, 0.50f}, {0.0f, -1.0f, 0.75f},  {1.0f, -1.0f, 0.50f},
      {2.0f, -1.0f, 0.15f},  {-2.0f, 0.0f, 0.25f},  {-1.0f, 0.0f, 0.75f},
      {0.0f, 0.0f, 1.00f},   {1.0f, 0.0f, 0.75f},   {2.0f, 0.0f, 0.25f},
      {-2.0f, 1.0f, 0.15f},  {-1.0f, 1.0f, 0.50f},  {0.0f, 1.0f, 0.75f},
      {1.0f, 1.0f, 0.50f},   {2.0f, 1.0f, 0.15f},   {-2.0f, 2.0f, 0.10f},
      {-1.0f, 2.0f, 0.15f},  {0.0f, 2.0f, 0.25f},   {1.0f, 2.0f, 0.15f},
      {2.0f, 2.0f, 0.10f}};
  return kOffsets;
}

// `SIM_MIN_RADIATION` and `SIM_MAX_RADIATION`.
inline constexpr float kMinRadiation = 0.0f;
inline constexpr float kMaxRadiation = 9000000.0f;
// The one epsilon the field pass uses, for both the occlusion cutoff and the final floor.
inline constexpr float kRadiationEpsilon = 0.01f;
// `Disease::GetDiseaseIndex(gDisease, 0xd49f77d6)` — radiation sickness, the one disease that
// makes the cell it sits in radioactive.
inline constexpr int32_t kRadiationSicknessHash = -727746602;

struct RadiationState {
  CompactedVector<RadiationEmitterData> emitters;
  // `SimData::cosmicRadiationOcclusion`, one float per padded cell. Klei `resize`s it with a
  // generator that returns 1.0, which fills **new** elements only — so it is allocated once
  // and carries its values from substep to substep for as long as the world lives.
  std::vector<float> occlusion;
  // The consumed-radiation callbacks. Filled only by `ProcessCellRadiationChanges`, never by the
  // emitters, and handed to the game whole once a frame.
  std::vector<ConsumedRadiationCallback> consumed;

  void Clear() {
    emitters.Clear();
    consumed.clear();
    occlusion.clear();
  }
};

// Which of the four per-pixel writers `SetCircleAA` is carrying. Klei passes a
// `std::function`; the three implementations are small and closed, so an enum is the same
// dispatch without the indirection.
enum RadPixel : uint8_t {
  kPixelPulsing = 0,
  kPixelSimplePulse = 1,
  kPixelAttractor = 2,
};

// `inRadialRange`. Everything is in degrees, and the wrap is done by
// `fmodf` against 360 after adding 360, so a negative direction is legal.
inline bool InRadialRange(int32_t x0, int32_t y0, int32_t x1, int32_t y1, float angle,
                          float direction) {
  if (angle == 360.0f || (x0 == x1 && y0 == y1)) return true;
  const float half = angle * 0.5f;
  const float lo = std::fmod((direction - half) + 360.0f, 360.0f);
  const float hi = std::fmod((half + direction) + 360.0f, 360.0f);
  const float pi = 3.14159265358979323846f;
  const float bearing = std::atan2(static_cast<float>(y1 - y0), static_cast<float>(x1 - x0));
  const float a = std::fmod((bearing * 180.0f) / pi + 360.0f, 360.0f);
  // The cone wraps through zero exactly when its end has fallen below its start.
  if (hi <= lo) return a >= lo || a <= hi;
  return a >= lo && a <= hi;
}

// The per-cell absorption factor, shared by the ray walk and the occlusion pass: an
// element's `radiationAbsorptionFactor`, scaled by `RADIATION_CONSTRUCTED_FACTOR` if the
// cell is a built tile and mixed with the cell's mass if it is not, clamped to [0, 1].
inline float RadiationAbsorption(const World& w, const ElementTable& t, size_t cell) {
  float f = t.At(w.Phases()[cell].element).radiationAbsorptionFactor;
  if (static_cast<int8_t>(w.Properties()[cell]) < 0) {
    f = f * w.RadiationConstructedFactor();
  } else {
    f = (w.Phases()[cell].mass / w.RadiationMaxMass()) * f * w.RadiationDensityWeight() +
        f * w.RadiationBaseWeight();
  }
  // Clamped min-then-max with SSE semantics, not comparisons -- see `ClampSS` in world.h. The
  // emitter's region function repeats the same walk. The divide above is by `RadiationMaxMass()`, which is a tuning
  // value the game SETS over the ABI (`SetRadiationParams` case 5) and can therefore be zero;
  // on a cell whose element absorbs nothing that is `0 * inf`, a NaN, and Klei's pair returns
  // 1.0 where a comparison clamp would hand the ray walk a NaN transmission.
  return ClampSS(f, 0.0f, 1.0f);
}

// `RadiationAbsorptionAlongLine`. A Bresenham walk from the emitter to the
// target, multiplying a running transmission by `1 - absorbed` at every cell it steps on —
// **including both endpoints**, so an emitter buried in lead attenuates itself.
//
// The per-cell factor is the element's `radiationAbsorptionFactor`, then either scaled by
// `RADIATION_CONSTRUCTED_FACTOR` (a constructed tile) or mixed with the cell's mass
// (everything else), and clamped to [0, 1] before it is used.
inline float RadiationAbsorptionAlongLine(const World& w, const ElementTable& t, int32_t x0,
                                          int32_t y0, int32_t x1, int32_t y1) {
  const float fx0 = static_cast<float>(x0), fy0 = static_cast<float>(y0);
  const float fx1 = static_cast<float>(x1), fy1 = static_cast<float>(y1);
  // Which axis is the long one decides which of the two coordinates the loop counts.
  const bool along_x = std::fabs(fy1 - fy0) <= std::fabs(fx1 - fx0);
  const float major_a = along_x ? fx0 : fy0;
  const float major_b = along_x ? fx1 : fy1;
  const float minor_a = along_x ? fy0 : fx0;
  const float minor_b = along_x ? fy1 : fx1;

  float from = minor_a, to = minor_b;
  if (major_b < major_a) {
    from = minor_b;
    to = minor_a;
  }
  const float lo = major_b <= major_a ? major_b : major_a;
  const float hi = major_a <= major_b ? major_b : major_a;
  const float span = hi - lo;
  const float delta = std::fabs(to - from);
  float err = span * 0.5f;
  const int32_t step = from < to ? 1 : -1;

  const int32_t pw = w.PaddedWidth();

  float transmission = 1.0f;
  int32_t minor = static_cast<int32_t>(from);
  const int32_t last = static_cast<int32_t>(hi);
  for (int32_t major = static_cast<int32_t>(lo); major <= last; ++major) {
    const int32_t x = along_x ? major : minor;
    const int32_t y = along_x ? minor : major;
    const size_t cell = static_cast<size_t>(y) * static_cast<size_t>(pw) +
                        static_cast<size_t>(x);
    // Klei writes this out twice, once here and once in the occlusion sweep; they are the
    // same five lines and are shared here so that one ablation covers both readers.
    transmission = (1.0f - RadiationAbsorption(w, t, cell)) * transmission;

    const float next = err - delta;
    err = next < 0.0f ? next + span : next;
    if (next < 0.0f) minor += step;
  }
  if (transmission >= 1.0f) transmission = 1.0f;
  if (transmission <= 0.0f) transmission = 0.0f;
  return transmission;
}

// The radiation **field**, which is not the emitter's and is the reason `radiation` was never
// a pass-through array. Two whole-region sweeps, part of `SimBase::UpdateData` and run
// once per substep immediately before `SimData::UpdateComponents`:
//
//   * **Top down**: build `cosmicRadiationOcclusion`, a running product of
//     `1 - absorption` down each column, and decay every cell by `x -= x / LINGER_RATE`.
//   * **Bottom up**: add what a radioactive element sprays through the 5x5
//     stencil, what a radiation-sick cell emits, and what the sky delivers through the
//     occlusion; then clamp to [0, 9000000] and floor anything at or below 0.01 to zero.
//
// The decay is why an emitter's steady state is `emitRads` and not something unbounded:
// `tickConstant` writes `rads / LINGER_RATE` per substep and this takes `x / LINGER_RATE`
// away, so `x` converges on `rads`. Getting the emitter right and this wrong puts every
// figure in the grid out by a factor that grows with the tick count, which is exactly what
// the first `radiate` run showed.
//
// Four asymmetries are Klei's and are transcribed rather than tidied:
//
//   * The column loop is `[minX, maxX)` while the stencil's own bound test is `<= maxX`.
//   * The top-down pass covers rows `[minY, maxY]`; the bottom-up pass covers `[minY, maxY)`.
//   * The occlusion of a row reads the row **below it in index order** (`row + 1`), and the
//     top two rows of the region both read a hard 1.0 instead.
//   * The decay's zero case is not a no-op: when `x / LINGER_RATE` is exactly zero it
//     subtracts a whole 1.0 and clamps at zero, which is how a denormal is swept out.
inline void StepRadiationField(World* w, const ElementTable& t, const DiseaseTable& diseases,
                               RadiationState* state, size_t ri) {
  if (!w->RadiationEnabled()) return;
  const int32_t pw = w->PaddedWidth();
  const World::PaddedRect& r = w->PaddedRegion(ri);
  if (state->occlusion.size() != w->PaddedCount()) {
    state->occlusion.assign(w->PaddedCount(), 1.0f);
  }
  const float linger = w->RadiationLingerRate();

  // ---- top down: occlusion, then decay.
  if (r.y0 <= r.y1) {
    for (int32_t y = r.y1; y >= r.y0; --y) {
      for (int32_t x = r.x0; x < r.x1; ++x) {
        const size_t cell = static_cast<size_t>(y) * static_cast<size_t>(pw) +
                            static_cast<size_t>(x);
        float above = 1.0f;
        if (y + 1 < r.y1) {
          above = state->occlusion[static_cast<size_t>(y + 1) * static_cast<size_t>(pw) +
                                   static_cast<size_t>(x)];
        }
        float occ = (1.0f - RadiationAbsorption(*w, t, cell)) * above;
        if (occ < kRadiationEpsilon) occ = 0.0f;
        state->occlusion[cell] = occ > 0.0f ? occ : 0.0f;

        const float have = w->Radiation()[cell];
        const float lost = have / linger;
        float left;
        if (lost == 0.0f) {
          left = have - 1.0f;
          if (left <= 0.0f) left = 0.0f;
        } else {
          left = have - lost;
        }
        w->SetRadiation(cell, left);
      }
    }
  }

  // ---- bottom up: the three sources, then the clamp and the floor.
  if (r.y0 >= r.y1) return;
  const uint8_t sick = diseases.IndexOfHash(kRadiationSicknessHash);
  const float cosmic = w->RegionCosmic(ri);
  const RadiationOffset* offsets = RadiationOffsets();
  for (int32_t y = r.y0; y < r.y1; ++y) {
    for (int32_t x = r.x0; x < r.x1; ++x) {
      const size_t cell = static_cast<size_t>(y) * static_cast<size_t>(pw) +
                          static_cast<size_t>(x);
      const float per_1000 = t.At(w->Phases()[cell].element).radiationPer1000Mass;
      if (per_1000 > 0.0f) {
        const float mass = w->Phases()[cell].mass;
        for (int i = 0; i < 25; ++i) {
          const int32_t tx = static_cast<int32_t>(offsets[i].dx) + x;
          const int32_t ty = static_cast<int32_t>(offsets[i].dy) + y;
          if (tx < r.x0 || ty < r.y0 || tx > r.x1 || ty > r.y1) continue;
          const size_t at = static_cast<size_t>(ty) * static_cast<size_t>(pw) +
                            static_cast<size_t>(tx);
          w->SetRadiation(at, mass * 0.001f * per_1000 * offsets[i].weight +
                                  w->Radiation()[at]);
        }
      }
      // Klei compares the cell's disease index against whatever `GetDiseaseIndex` returned,
      // and a table with no radiation sickness in it returns the same 0xff a clean cell
      // carries — so on such a world every cell matches and contributes its zero germ count.
      if (w->DiseaseIdx(cell) == sick) {
        w->SetRadiation(cell, static_cast<float>(w->Disease()[cell].count) * 0.001f +
                                  w->Radiation()[cell]);
      }
      const float occ = state->occlusion[cell];
      if (occ > 0.0f) {
        w->SetRadiation(cell, (cosmic / linger) * occ + w->Radiation()[cell]);
      }
      const float have = w->Radiation()[cell];
      if (!(have >= 0.0f)) {
        w->SetRadiation(cell, kMinRadiation);
      } else if (have > kMaxRadiation) {
        w->SetRadiation(cell, kMaxRadiation);
      }
      if (w->Radiation()[cell] <= kRadiationEpsilon) w->SetRadiation(cell, 0.0f);
    }
  }
}

// `setPulsing`. The ring writer: attenuated, refuses the emitter's own cell,
// and refuses a non-positive value — which is what makes the second half of every
// anti-aliased pixel pair free when the first half took all of it.
inline void SetPulsingPixel(World* w, const ElementTable& t, const World::PaddedRect& r,
                            const RadiationEmitterData& d, int32_t ox, int32_t oy, int32_t x,
                            int32_t y, float value) {
  if (!(value > 0.0f)) return;
  if (x <= 0 || x >= w->PaddedWidth()) return;
  if (y <= 0 || y >= w->PaddedHeight()) return;
  if (x < r.x0 || y < r.y0 || x > r.x1 || y > r.y1) return;
  if (!InRadialRange(ox, oy, x, y, d.emit_angle, d.emit_direction)) return;
  if (x == ox && y == oy) return;
  const float a = RadiationAbsorptionAlongLine(*w, t, ox, oy, x, y);
  const size_t cell = static_cast<size_t>(y) * static_cast<size_t>(w->PaddedWidth()) +
                      static_cast<size_t>(x);
  w->SetRadiation(cell, a * value + w->Radiation()[cell]);
}

// `setSimplePulse`. No attenuation, no value test, and the emitter's own cell
// is written like any other.
inline void SetSimplePulsePixel(World* w, const World::PaddedRect& r,
                                const RadiationEmitterData& d, int32_t ox, int32_t oy,
                                int32_t x, int32_t y, float value) {
  if (x <= 0 || x >= w->PaddedWidth()) return;
  if (y <= 0 || y >= w->PaddedHeight()) return;
  if (x < r.x0 || y < r.y0 || x > r.x1 || y > r.y1) return;
  if (!InRadialRange(ox, oy, x, y, d.emit_angle, d.emit_direction)) return;
  const size_t cell = static_cast<size_t>(y) * static_cast<size_t>(w->PaddedWidth()) +
                      static_cast<size_t>(x);
  w->SetRadiation(cell, value + w->Radiation()[cell]);
}

// `setAttractor`. The one writer that *takes*: it drains up to `value` rads
// out of a cell that holds at least 0.01, and reports the amount through `out` by
// overwriting it, so what survives to the caller is whichever cell the raster reached last.
inline void SetAttractorPixel(World* w, const World::PaddedRect& r,
                              const RadiationEmitterData& d, int32_t ox, int32_t oy, int32_t x,
                              int32_t y, float value, float* out) {
  if (x <= 0 || x >= w->PaddedWidth()) return;
  if (y <= 0 || y >= w->PaddedHeight()) return;
  if (x < r.x0 || y < r.y0 || x > r.x1 || y > r.y1) return;
  if (!InRadialRange(ox, oy, x, y, d.emit_angle, d.emit_direction)) return;
  if (x == ox && y == oy) return;
  const size_t cell = static_cast<size_t>(y) * static_cast<size_t>(w->PaddedWidth()) +
                      static_cast<size_t>(x);
  const float have = w->Radiation()[cell];
  if (!(have >= 0.01f)) return;
  const float take = have <= value ? have : value;
  w->SetRadiation(cell, have - take);
  if (out != nullptr) *out = take;
}

inline void SetRadPixel(World* w, const ElementTable& t, const World::PaddedRect& r,
                        const RadiationEmitterData& d, int32_t ox, int32_t oy, int32_t x,
                        int32_t y, float value, float* out, RadPixel kind) {
  switch (kind) {
    case kPixelPulsing:
      SetPulsingPixel(w, t, r, d, ox, oy, x, y, value);
      break;
    case kPixelSimplePulse:
      SetSimplePulsePixel(w, r, d, ox, oy, x, y, value);
      break;
    case kPixelAttractor:
      SetAttractorPixel(w, r, d, ox, oy, x, y, value, out);
      break;
  }
}

// `SetPixel4`. The four-way mirror, in Klei's order: `+x+y`, `-x+y`, `+x-y`,
// `-x-y`. The order is not cosmetic — the attractor reports the last cell it drained.
inline void SetPixel4(World* w, const ElementTable& t, const World::PaddedRect& r,
                      const RadiationEmitterData& d, int32_t cx, int32_t cy, int32_t dx,
                      int32_t dy, float value, float* out, RadPixel kind) {
  SetRadPixel(w, t, r, d, cx, cy, cx + dx, cy + dy, value, out, kind);
  SetRadPixel(w, t, r, d, cx, cy, cx - dx, cy + dy, value, out, kind);
  SetRadPixel(w, t, r, d, cx, cy, cx + dx, cy - dy, value, out, kind);
  SetRadPixel(w, t, r, d, cx, cy, cx - dx, cy - dy, value, out, kind);
}

// `SetCircleAA`. Two rasters of the same ellipse — one stepping x, one
// stepping y — each stopping where the other takes over, at `r^2 / hypot`. Every step
// splits `value` between two adjacent pixels by the fractional part of the ellipse's
// crossing, rounded, which is the anti-aliasing.
//
// The loop counters are floats compared against a rounded float bound, so the number of
// steps is whatever that comparison says and not a derived integer.
inline void SetCircleAA(World* w, const ElementTable& t, const World::PaddedRect& r,
                        const RadiationEmitterData& d, int32_t cx, int32_t cy, int32_t rx,
                        int32_t ry, float value, float* out, RadPixel kind) {
  if (rx < 1 || ry < 1) return;
  const float inv_hyp = 1.0f / std::sqrt(static_cast<float>(ry * ry + rx * rx));

  float rx2 = static_cast<float>(rx * rx);
  float bound = std::round(inv_hyp * rx2);
  for (float x = 0.0f; x <= bound; x += 1.0f) {
    const float yf = std::sqrt(1.0f - x * x * (1.0f / rx2)) * static_cast<float>(ry);
    const float yi = std::floor(yf);
    const float share = std::round((yf - yi) * value);
    SetPixel4(w, t, r, d, cx, cy, static_cast<int32_t>(x), static_cast<int32_t>(yi), share,
              out, kind);
    SetPixel4(w, t, r, d, cx, cy, static_cast<int32_t>(x), static_cast<int32_t>(yi) - 1,
              value - share, out, kind);
  }

  const float ry2 = static_cast<float>(ry * ry);
  bound = std::round(ry2 * inv_hyp);
  for (float y = 0.0f; y <= bound; y += 1.0f) {
    const float xf = std::sqrt(1.0f - y * y * (1.0f / ry2)) * static_cast<float>(rx);
    const float xi = std::floor(xf);
    const float share = std::round((xf - xi) * value);
    SetPixel4(w, t, r, d, cx, cy, static_cast<int32_t>(xi), static_cast<int32_t>(y), share,
              out, kind);
    SetPixel4(w, t, r, d, cx, cy, static_cast<int32_t>(xi) - 1, static_cast<int32_t>(y),
              value - share, out, kind);
  }
}

// Klei's ceiling: truncate, and add one when the truncation lost something and the value was
// positive. `INT_MIN` is left alone because the truncation of an out-of-range float is
// already `INT_MIN` and the game does not add to it.
inline int32_t CeilFromTrunc(float f) {
  int32_t i = static_cast<int32_t>(f);
  if (i != INT32_MIN && static_cast<float>(i) != f) i += (f < 0.0f ? 0 : 1);
  return i;
}

// `tickConstant`. The steady-state emitter: one pass over its whole box every
// step, ellipse-clipped, attenuated per ray, with a linear falloff and a noise term.
//
// The falloff is not radial. It is `(1 - |dx|/rx) - (|dy| - |dy*dx|/rx) / ry`, which is a
// bilinear tent over the bounding box rather than a cone, and the noise is drawn only where
// that tent has fallen below a quarter.
inline void TickConstant(World* w, const ElementTable& t, const World::PaddedRect& r,
                         const RadiationEmitterData& d) {
  const int32_t pw = w->PaddedWidth(), ph = w->PaddedHeight();
  const int32_t cy = d.cell / pw, cx = d.cell % pw;
  const int32_t rx = d.radius_x, ry = d.radius_y;
  const float frx = static_cast<float>(rx), fry = static_cast<float>(ry);
  const float rx2 = static_cast<float>(rx * rx), ry2 = static_cast<float>(ry * ry);
  const float linger = w->RadiationLingerRate();
  // The scale is written as its bits: it is 2^-17 nudged by one ulp, so 0x7fff of them come
  // to a hair over a quarter, and the difference decides the low bit of a rads figure.
  const uint32_t noise_scale_bits = 0x37000100u;
  float noise_scale;
  memcpy(&noise_scale, &noise_scale_bits, sizeof(noise_scale));

  const int32_t x0 = cx - rx, y0 = cy - ry;
  // Both extents are `2 * radiusX`. See the header note: this is Klei's, not a slip.
  for (int32_t row = 0; row <= 2 * rx; ++row) {
    const int32_t y = y0 + row;
    for (int32_t x = x0; x <= x0 + 2 * rx; ++x) {
      if (x <= 0 || x >= pw) continue;
      if (y <= 0 || y >= ph) continue;
      if (x < r.x0 || y < r.y0 || x > r.x1 || y > r.y1) continue;
      if (!InRadialRange(cx, cy, x, y, d.emit_angle, d.emit_direction)) continue;
      const float dx = static_cast<float>(x - cx);
      const float dy = static_cast<float>(y - cy);
      if (!((dy * dy) / ry2 + (dx * dx) / rx2 <= 1.0f)) continue;
      const float inv_rx = 1.0f / frx;
      const float falloff = (1.0f - std::fabs(dx) * inv_rx) -
                            (std::fabs(dy) - std::fabs(dy * dx) * inv_rx) / fry;
      const float a = RadiationAbsorptionAlongLine(*w, t, cx, cy, x, y);
      const float v = a * ((falloff * d.emit_rads) / linger);
      float noise = 0.0f;
      if (falloff < 0.25f) {
        const uint32_t s = w->NextRandomState();
        noise = static_cast<float>((s >> 16) & 0x7FFFu) * v * noise_scale - v * 0.125f;
      }
      const size_t cell = static_cast<size_t>(y) * static_cast<size_t>(pw) +
                          static_cast<size_t>(x);
      w->SetRadiation(cell, noise + v + w->Radiation()[cell]);
    }
  }
}

// `tickPulsing`, which serves both `Pulsing` and `PulsingAveraged`. Step zero
// is a single write into the emitter's own cell — **unguarded**, neither by the region nor
// by the grid bounds — and every later step is one anti-aliased ring.
//
// `PulsingAveraged` divides the ring's rads by the area of the ring's bounding box, so a
// pulse spreads a fixed budget instead of repeating a fixed intensity.
inline void TickPulsing(World* w, const ElementTable& t, const World::PaddedRect& r,
                        const RadiationEmitterData& d, int32_t max_radius) {
  const int32_t pw = w->PaddedWidth();
  // A true division here, and a multiply by the reciprocal in `Update`'s own two cases.
  // They are not the same number and the raster can differ by a pixel because of it.
  const float frac = (static_cast<float>(d.emit_step) + 1.0f) / static_cast<float>(max_radius);
  const int32_t cy = d.cell / pw;
  const int32_t rx = CeilFromTrunc(static_cast<float>(d.radius_x) * frac);
  const int32_t ry = CeilFromTrunc(static_cast<float>(d.radius_y) * frac);
  float rads = d.emit_rads;
  if (d.emit_type == kRadiationPulsingAveraged) {
    int32_t area = (ry - 1) * (rx - 1);
    if (area < 1) area = 1;
    rads = rads / static_cast<float>(area);
  }
  if (d.emit_step == 0) {
    w->SetRadiation(static_cast<size_t>(d.cell),
                    rads + w->Radiation()[static_cast<size_t>(d.cell)]);
  } else if (rx > 1 || ry > 1) {
    SetCircleAA(w, t, r, d, d.cell % pw, cy, rx, ry, rads, nullptr, kPixelPulsing);
  }
}

// `RadiationEmitter::Update`. One substep of the whole component.
//
// The region test is a point test on the emitter's own cell and it comes *before* the
// timers move, so an emitter outside the active region is frozen rather than merely silent.
//
// The two timers do different jobs. `emitTimer` is the pulse clock: it resets, along with
// the phase, whenever the rate has elapsed. `emitStepTimer` is the sweep clock and is a
// carry — it is decremented by one step's worth per step taken and topped up by `dt`, so a
// sweep that cannot afford a step this substep banks the time towards the next one.
inline void StepRadiationEmitters(World* w, const ElementTable& t, RadiationState* state,
                                  float dt, size_t ri) {
  if (!w->RadiationEnabled()) return;
  std::vector<RadiationEmitterData>& all = state->emitters.Data();
  if (all.empty()) return;
  const World::PaddedRect& r = w->PaddedRegion(ri);
  const int32_t pw = w->PaddedWidth();

  for (size_t i = 0; i < all.size(); ++i) {
    RadiationEmitterData& d = all[i];
    const int32_t y = d.cell / pw;
    const int32_t x = d.cell % pw;
    if (x < r.x0 || y < r.y0 || x > r.x1 || y > r.y1) continue;
    if (!(d.emit_rads > 0.0f)) continue;
    if (d.radius_x == 0 && d.radius_y == 0) continue;

    const float timer = d.emit_timer + dt;
    d.emit_timer = timer;
    bool rate_zero = false;
    bool elapsed;
    if (d.emit_rate == 0.0f) {
      rate_zero = true;
      elapsed = true;
    } else {
      elapsed = d.emit_rate <= timer;
    }
    // Inside the sweep window the clock only resets when the rate has come round; past the
    // end of the window an emitter whose rate has *not* come round does nothing at all.
    if (timer <= d.emit_speed) {
      if (elapsed) {
        d.emit_timer = 0.0f;
        d.emit_step = 0;
      }
    } else {
      if (!elapsed) continue;
      d.emit_timer = 0.0f;
      d.emit_step = 0;
    }

    const int32_t max_radius =
        d.radius_y <= d.radius_x ? static_cast<int32_t>(d.radius_x)
                                 : static_cast<int32_t>(d.radius_y);
    const float inv_radius = 1.0f / static_cast<float>(max_radius);
    const float step_seconds = inv_radius * d.emit_speed;
    float steps = dt / step_seconds;
    if (rate_zero || (steps <= 1.0f && step_seconds <= d.emit_step_timer)) steps = 1.0f;

    const int32_t count = static_cast<int32_t>(steps);
    for (int32_t k = 0; k < count; ++k) {
      switch (d.emit_type) {
        case kRadiationConstant:
          TickConstant(w, t, r, d);
          break;
        case kRadiationPulsing:
        case kRadiationPulsingAveraged:
          TickPulsing(w, t, r, d, max_radius);
          break;
        case kRadiationSimplePulse: {
          const float frac = (static_cast<float>(d.emit_step) + 1.0f) * inv_radius;
          int32_t rx = CeilFromTrunc(static_cast<float>(d.radius_x) * frac);
          int32_t ry = CeilFromTrunc(static_cast<float>(d.radius_y) * frac);
          if (rx < 1) rx = 1;
          if (ry < 1) ry = 1;
          SetCircleAA(w, t, r, d, d.cell % pw, d.cell / pw, rx, ry, d.emit_rads, nullptr,
                      kPixelSimplePulse);
          break;
        }
        case kRadiationAttractor: {
          const float frac = (static_cast<float>(d.emit_step) + 1.0f) * inv_radius;
          int32_t rx = CeilFromTrunc(static_cast<float>(d.radius_x) * frac);
          int32_t ry = CeilFromTrunc(static_cast<float>(d.radius_y) * frac);
          if (rx < 1) rx = 1;
          if (ry < 1) ry = 1;
          float taken = 0.0f;
          SetCircleAA(w, t, r, d, d.cell % pw, d.cell / pw, rx, ry, d.emit_rads, &taken,
                      kPixelAttractor);
          // Always true as written — `taken` starts at zero and the writer only ever stores
          // a positive — so the emitter's own cell is touched on every attractor step.
          if (taken >= 0.0f) {
            w->SetRadiation(static_cast<size_t>(d.cell),
                            taken + w->Radiation()[static_cast<size_t>(d.cell)]);
          }
          break;
        }
        default:
          // `RadialBeams` and anything else: the phase and the timers still move.
          break;
      }
      ++d.emit_step;
      if (static_cast<uint32_t>(d.emit_step) == static_cast<uint32_t>(max_radius)) {
        d.emit_step = 0;
      }
      d.emit_step_timer -= step_seconds;
    }
    d.emit_step_timer += dt;
  }
}

}  // namespace oni_sim
