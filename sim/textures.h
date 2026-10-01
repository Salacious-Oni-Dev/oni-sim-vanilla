// The five property textures.
//
// GameDataUpdate ends with five pointers the game hands straight to
// Texture2D.LoadRawTextureData. They are raw buffers, not handles — an earlier version of
// the generated ABI had them as int32_t because `IntPtr` was missing from the type map,
// which left GameDataUpdate 20 bytes short on x64. They are the last five fields, which
// is why nothing crashed and why it went unnoticed until now.
//
// Sizes and formats come from PropertyTextures.textureProperties and the
// LoadRawTextureData calls that read them:
//
//   Flow               RGFloat  8 bytes/cell   two floats
//   Liquid             RGBA32   4 bytes/cell
//   LiquidData         RGBA32   4 bytes/cell
//   MaterialData       RGBA32   4 bytes/cell
//   ExposedToSunlight  Alpha8   1 byte/cell
//
// Contents are consumed by shaders, so what is implemented here was read out of the game's
// own buffers for worlds whose contents were known
// (driver/src/diffsim.cpp --scenario textures). Where that reading is solid it is stated
// as fact; where it is not, it is marked, because a plausible-looking guess in a
// rendering buffer produces a subtly wrong picture rather than an error.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "projection.h"
#include "world.h"

namespace oni_sim {


struct PropertyTextureBuffers {
  std::vector<float> flow;              // 2 per cell
  std::vector<uint8_t> liquid;          // 4 per cell
  std::vector<uint8_t> liquid_data;     // 4 per cell
  std::vector<uint8_t> material_data;   // 4 per cell
  std::vector<uint8_t> exposed_to_sun;  // 1 per cell

  // What each cell's textures were built from last time. Three of the four buffers depend
  // only on the cell's element unless the cell holds liquid: `material_data` is the
  // element's `materialProperties` and nothing else, and a cell with no liquid in it writes
  // zeros into the other two. So a cell whose element has not changed and which held no
  // liquid has nothing to write, which on a 512x768 asteroid is 99.7% of the grid.
  std::vector<uint16_t> last_element;
  std::vector<uint8_t> last_liquid;
  // Which game cells the flow texture was written into last frame, so the next frame can
  // put them back to zero without walking the grid. The flow pass is the one part of this
  // file that cannot skip an unchanged cell — the value is a transfer, so it is zero again
  // the moment the transfer stops — and it is also the one part whose live set is a
  // fraction of the grid rather than a fraction of the *changes*: on the bench asteroid it
  // is the moving gas, about a third of the cells, and on a settled world far less.
  std::vector<uint32_t> flow_dirty;
  // Element 0 is a real element, so "never filled" cannot be spelled as a value in
  // `last_element`; and a reloaded table can change `materialProperties` under an element
  // index that did not move.
  bool primed = false;
  uint32_t table_generation = 0;

  // These are handed to the game as bare pointers and read every frame, so like the
  // projection buffers they must be allocated once and never resized after Start.
  void Allocate(size_t cells) {
    flow.assign(cells * 2, 0.0f);
    liquid.assign(cells * 4, 0);
    liquid_data.assign(cells * 4, 0);
    material_data.assign(cells * 4, 0);
    exposed_to_sun.assign(cells, 0);
    last_element.assign(cells, 0);
    last_liquid.assign(cells, 0);
    flow_dirty.clear();
    primed = false;
  }
};

// The liquid texture's RGB comes from the element's `gradientColours`, which is not a
// plain colour list: each entry packs an RGB in its low three bytes and a **key** in its
// high byte. Water's three stops are (61,164,180) at key 0, (29,130,141) at key 77 and
// (14,101,130) at key 255; magma's are (255,89,0) at 0, (255,26,0) at 22 and (234,24,0)
// at 255. The key is the alpha the cell computes, and the RGB is looked up along it.
inline void UnpackColour(uint32_t packed, uint8_t* rgb) {
  rgb[0] = static_cast<uint8_t>(packed & 0xFF);
  rgb[1] = static_cast<uint8_t>((packed >> 8) & 0xFF);
  rgb[2] = static_cast<uint8_t>((packed >> 16) & 0xFF);
}

// The fill curve both halves of the liquid texture run on:
//
//     powf(min(mass / 1000, 1), 0.45)
//
// The divisor is the literal **1000**, not the element's `maxMass`. Every liquid in the
// shipped table whose `maxMass` happens to be 1000 hides the difference, which is why the
// old `maxMass` spelling survived a full suite; the element that separates them is 133,
// `maxMass` 870, and a *full* cell of it reads a gradient position of 0.939 rather than 1.0.
//
// Two call sites, spelled differently in the game and kept apart here for the same
// reason: the alpha multiplies by an 0.001f constant, and the gradient divides by a 1000.0f
// passed in as an argument. They are not the same float.
//
// Note there is no lower clamp on either — a minimum against 1.0 and nothing else.
inline float LiquidFillAlpha(float mass) {
  float f = mass * 0.001f;
  if (f > 1.0f) f = 1.0f;
  return std::pow(f, 0.45f);
}

inline float LiquidFillGradient(float mass) {
  float f = mass / 1000.0f;
  if (f > 1.0f) f = 1.0f;
  return std::pow(f, 0.45f);
}

// `GetEstimatedLiquidGradientValue`: where along its element's gradient a
// liquid cell renders. 0 is the first stop, 1 the last.
//
// The rule is not "how full is this cell" — that is only one factor of three:
//
//   * a cell with **gas or vacuum directly above it is the surface** and scores 0 flat,
//     whatever it holds. That is where the old "a surface cell reads the first gradient
//     stop verbatim" observation came from: it is this branch, not a special case;
//   * otherwise the score is `(1 - sunlight/255) * fill`, so a **lit** cell is dragged back
//     towards the surface colour. The sunlight it reads is the published texture, which is
//     to say **last frame's** — this runs before the sweep that refills it;
//   * and it is capped by the cell above's own score, computed one level deep and not
//     recursively. So the first cell under a surface cell scores 0 too, and a column can
//     only darken downwards.
//
// A non-liquid cell answers the recursive call with 0 if it is gas or vacuum and 1 if it is
// solid, which is how a liquid under a floor is left uncapped.
inline float EstimatedLiquidGradient(const World& w, const ElementTable& table,
                                     const std::vector<uint8_t>& sun, size_t p,
                                     bool recurse) {
  const PhaseEntry& me = w.Phase(p);
  const uint8_t state = table.At(me.element).state & kStateMask;
  if (state != kStateLiquid) return state < kStateLiquid ? 0.0f : 1.0f;
  const float fill = LiquidFillGradient(me.mass);
  const size_t up = p + static_cast<size_t>(w.PaddedWidth());
  float value = 0.0f;
  if ((table.At(w.Phase(up).element).state & kStateMask) >= kStateLiquid) {
    const int64_t game = w.GameIndex(p);
    const uint8_t lit =
        (game >= 0 && static_cast<size_t>(game) < sun.size()) ? sun[static_cast<size_t>(game)] : 0;
    float shade = 1.0f - static_cast<float>(lit) * (1.0f / 255.0f);
    if (shade >= 0.0f) {
      if (shade > 1.0f) shade = 1.0f;
    } else {
      shade = 0.0f;
    }
    value = shade * fill;
  }
  float cap = 1.0f;
  if (recurse && value > 0.0f) cap = EstimatedLiquidGradient(w, table, sun, up, false);
  return cap <= value ? cap : value;
}

// The gradient walk inside `UpdateLiquidPropertyTexture`. It visits **every** stop in order
// and keeps the last one whose key is at or below the position — no break and no search —
// and the key is the stop's high byte over 255, not the raw byte.
//
// The interpolation truncates the **delta**, not the sum:
//
//     channel = (int)((float)(next - cur) * t) + cur
//
// which for a falling channel truncates towards zero and therefore rounds *up*. That one
// detail is the whole `sunliquid` RGB divergence: water a tenth full reads blue 141 in Klei,
// where flooring the sum and rounding the sum both give 140. The comment that used to sit
// here said "rounded, not truncated", which fitted the samples it was written from and was
// still the wrong law — rounding and this agree whenever the fractional part is above a half.
inline uint32_t LiquidGradientColour(const Element& e, float pos) {
  uint32_t rgb = e.colour;
  const int n = e.numberOfGradientColors;
  for (int j = 0; j < n && j < 6; ++j) {
    const uint32_t cur = e.gradientColours[j];
    const float key = static_cast<float>(cur >> 24) * (1.0f / 255.0f);
    if (!(key <= pos)) continue;
    // The last stop has no successor: Klei feeds it itself, every delta is zero and the
    // stop comes back verbatim.
    if (j == n - 1) {
      rgb = cur;
      continue;
    }
    const uint32_t next = e.gradientColours[j + 1];
    float t = (pos - key) / (static_cast<float>(next >> 24) * (1.0f / 255.0f) - key);
    if (t <= 0.0f) {
      t = 0.0f;
    } else if (1.0f <= t) {
      t = 1.0f;
    }
    uint32_t out = 0;
    for (int k = 0; k < 3; ++k) {
      const int c = static_cast<int>((cur >> (8 * k)) & 0xFF);
      const int d = static_cast<int>((next >> (8 * k)) & 0xFF) - c;
      out |= (static_cast<uint32_t>(static_cast<int>(static_cast<float>(d) * t) + c) & 0xFFu)
             << (8 * k);
    }
    rgb = out;
  }
  return rgb & 0xFFFFFFu;
}

// Sunlight. This is the one texture with a *gameplay* consumer rather than only a
// rendering one — WorldGenSimUtil binds it to Grid.exposedToSunlight, which solar panels
// and plants read.
//
// It is **per world**, and that is why it looked like it was never filled: it stays zero
// until `DefineWorldOffsets` says where the worlds are. Measured against the game with two worlds of different heights side by side
// (`diffsim --scenario sunlight`):
//
//   * each world's **top row** — `y = offsetY + h - 1`, across its own x-span — starts at
//     full exposure whatever the cell holds. Two worlds of different heights light two
//     different rows, which is what proves this is per world and not per grid;
//   * light then descends column by column, and each cell it passes through absorbs
//     `min(mass, maxMass) / maxMass * lightAbsorptionFactor` of it;
//   * the published byte is `exposure * 255` **truncated**, and the exposure clamps at zero.
//
// ONE PHASE TEST, and it is the solid one. A fluid absorbs in proportion to its mass, as above;
// a SOLID absorbs its whole `lightAbsorptionFactor` whatever it weighs (`sunglass`, measured
// against the game). Oxygen's factor is 0, so it is transparent however much of it there
// is -- but not every gas: carbon dioxide's is 0.1, methane's 0.25, and every gas has a
// `maxMass` of 1.8 (`ElementLoader`), so 1.8 kg of CO2 dims a cell by a tenth. Granite's factor
// is 1, so any granite tile absorbs the whole beam; glass and diamond (0.1) pass nine tenths of
// it, ice (0.33333) two thirds, and Klei's `Transparent` cell bit plays no part. Water (0.25,
// maxMass 1000) dims a full cell by a quarter; the liquid at element 133 (1.0, maxMass 870)
// puts out a full beam in a single cell, 255 to 15 at 815.65 kg.
//
// `currentSunlightIntensity` from `NewGameFrame` is **not** an input: every scenario here
// sends it as zero and Klei still lights the top row at 255.
inline void ComputeSunlight(const World& w, const ElementTable& table,
                            const ProjectionBuffers& projected, std::vector<uint8_t>* out) {
  std::fill(out->begin(), out->end(), static_cast<uint8_t>(0));
  const std::vector<World::WorldOffset>& worlds = w.WorldOffsets();
  if (worlds.empty()) return;

  const int32_t gw = w.GameWidth();
  const int32_t gh = w.GameHeight();
  for (const World::WorldOffset& world : worlds) {
    const int32_t x0 = world.x < 0 ? 0 : world.x;
    const int32_t x1 = world.x + world.w > gw ? gw : world.x + world.w;
    const int32_t y0 = world.y < 0 ? 0 : world.y;
    const int32_t y1 = world.y + world.h > gh ? gh : world.y + world.h;
    for (int32_t x = x0; x < x1; ++x) {
      float exposure = 1.0f;
      // Downward from the world's own top row, which is lit before anything in it absorbs.
      for (int32_t y = y1 - 1; y >= y0; --y) {
        const size_t game = static_cast<size_t>(y) * static_cast<size_t>(gw) +
                            static_cast<size_t>(x);
        if (game >= out->size()) continue;
        (*out)[game] = static_cast<uint8_t>(exposure * 255.0f);
        // From the **projection**, not from the cell. The projection is the state being
        // published this frame, and it is what Klei's texture agrees with: reading the live
        // grid instead puts three columns wrong wherever the two disagree about a cell's
        // mass.
        const float mass = projected.mass[game];
        const Element& el = table.At(projected.element[game]);
        if ((el.state & kStateMask) == kStateSolid) {
          // A solid takes its WHOLE `lightAbsorptionFactor`, whatever it weighs. Granite (1.0)
          // at a quarter of its `maxMass` blocks exactly as completely as granite at twice it --
          // measured on `sunlight`, whose plugs are 460, 920, 1380, 1840 and 2760 kg and which
          // reads 0 below every one of them. But that is factor 1, not "a solid stops the
          // beam": `sunglass` put glass (0.1) at 800 kg and diamond (0.1) at 700 kg
          // under Klei's DLL and both read 229 below, ice (0.33333) at 1000 kg read 170, a
          // glass plug WITHOUT the `Transparent` bit read the same 229 as one with it. Stacked,
          // the factors subtract in float: two glass panes read 203 below
          // the second, not 204, because `1 - 0.1f - 0.1f` truncates to 203, and three ice
          // slabs read 170, 85 and then 0 with the exposure still a hair above zero. Until then
          // this line was `break`, and every window tile, ice sheet and diamond blocked the sky
          // that vanilla lets through. The mass-proportional law below is for fluids only.
          exposure -= el.lightAbsorptionFactor;
          if (exposure <= 0.0f) {
            break;
          }
          continue;
        }
        if (el.maxMass > 0.0f && el.lightAbsorptionFactor > 0.0f && mass > 0.0f) {
          const float full = mass > el.maxMass ? el.maxMass : mass;
          exposure -= full / el.maxMass * el.lightAbsorptionFactor;
          if (exposure <= 0.0f) {
            // Everything below is dark; the rest of the column is already zero.
            break;
          }
        }
      }
    }
  }
}

// `UpdateFlowTexture`, and it is the whole function.
//
//     x = (accum[0] - accum[1]) * scale
//     y = (accum[3] - accum[2]) * scale
//
// with `scale = 1.0 / max(mass, 1.0)`, and `scale = 0` outright for any cell whose element
// has changed since the substep's **last** `CellSOA::CopyFrom` — the one *after* the liquid section, and is what `World::SnapshotFlowElements`
// records. It is not `LiquidSweepStart` and not the earlier copy the gas
// sweeps read; gating on the gas one puts 19 scenarios wrong, and gating on
// `LiquidSweepStart` silences every cell a liquid poured into. The clamp at 1.0 kg means gas
// publishes the raw transfer — a gas cell is a hundredth of a kilogram — while a 1000 kg
// water cell publishes a thousandth of it, which is where a live game's ~0.006 in a moving
// liquid comes from.
//
// The loop walks every game cell in Klei and cannot here. This field is zero wherever
// nothing is moving, which on a real asteroid is most of the grid and all of the solid; so
// it walks the cells the accumulator recorded, and puts last frame's back to zero first.
// `flow_dirty` is the previous frame's list, kept in game indices because zeroing needs
// nothing else.
inline void FillFlowTexture(const World& w, PropertyTextureBuffers* out) {
  for (uint32_t g : out->flow_dirty) {
    out->flow[static_cast<size_t>(g) * 2] = 0.0f;
    out->flow[static_cast<size_t>(g) * 2 + 1] = 0.0f;
  }
  out->flow_dirty.clear();

  const std::vector<float>& accum = w.FlowAccum();
  const std::vector<World::FlowCell>& touched = w.FlowTouched();
  const std::vector<uint16_t>& snapshot = w.FlowElements();
  for (size_t ti = 0; ti < touched.size(); ++ti) {
    const World::FlowCell& c = touched[ti];
    const size_t p = c.padded;
    const size_t g = c.game;
    if (g * 2 + 1 >= out->flow.size()) continue;
    const PhaseEntry& live = w.Phase(p);
    float scale = 0.0f;
    if (ti < snapshot.size() && snapshot[ti] == live.element) {
      scale = 1.0f / (live.mass > 1.0f ? live.mass : 1.0f);
    }
    const float* f = &accum[static_cast<size_t>(p) * 4];
    out->flow[g * 2] = (f[0] - f[1]) * scale;
    out->flow[g * 2 + 1] = (f[3] - f[2]) * scale;
    out->flow_dirty.push_back(static_cast<uint32_t>(g));
  }
}

inline void FillPropertyTextures(const World& w, const ElementTable& table,
                                 const ProjectionBuffers& projected,
                                 PropertyTextureBuffers* out) {
  const size_t n = w.GameCount();
  const bool all = !out->primed || out->table_generation != table.Generation();
  // Sunlight is filled **first**, because the liquid texture reads it: a lit liquid cell is
  // dragged back towards its first gradient stop, and reading the previous frame's light
  // instead put `sunliquid`'s whole water body two shades too dark from the frame the light
  // first appeared. The order is not free and it is measured, not assumed.
  ComputeSunlight(w, table, projected, &out->exposed_to_sun);
  // This loop is a scan and almost nothing else: on a 512x768 asteroid 1,145 cells of
  // 393,216 get past the test below, and the other 392,071 exist only to be tested. So it
  // is driven by the game index alone and pays for a padded index — a 64-bit division by a
  // run-time width — only in the cells that turn out to need one.
  for (size_t i = 0; i < n; ++i) {
    // Read from the projection rather than from the cell. Both say the same thing — for a
    // single-phase cell `element`, `mass` and `temperature` are `DominantElement`,
    // `TotalMass` and `MeanTemperature` of a list of one — but the projection's arrays are
    // game-indexed and packed, and `PhaseEntry` is twelve bytes with a row of padding
    // between each of them. Since almost every cell skips out on the very next line, what
    // this loop mostly does is stream one field, and streaming it from `element` moves 2
    // bytes a cell instead of 12.
    const uint16_t element = projected.element[i];
    // Everything below is a function of the element, and of the mass and temperature only
    // when the cell holds liquid. Skipping the rest is not a heuristic — the bytes it would
    // write are the bytes already there.
    if (!all && element == out->last_element[i] && !out->last_liquid[i]) continue;
    const float mass = projected.mass[i];
    const Element& el = table.At(element);
    const uint8_t phase = static_cast<uint8_t>(el.state & kStateMask);
    out->last_element[i] = element;
    // A liquid cell is recomputed every frame whether or not its element changed, because
    // two of its three inputs are not the element: the sunlight texture and whatever is
    // sitting on top of it both move underneath it. Klei has no cache here at all.
    out->last_liquid[i] = (phase == kStateLiquid) ? 1 : 0;

    uint32_t liquid_word = 0;
    uint32_t data_word = 0;
    uint32_t material_word = 0;

    // Klei's guard is the element's state and **nothing else** — no mass test. All three
    // textures are zero for every cell that is not a liquid, `materialData` included, which
    // is the one place the old code differed on paper: it wrote `materialProperties` for
    // every cell. Only one non-liquid element in the shipped table carries a non-zero one
    // (index 124, and the mask below eats it), so the suite could never see it.
    if (phase == kStateLiquid) {
      const size_t p = w.Padded(i);
      const size_t up = p + static_cast<size_t>(w.PaddedWidth());
      const uint8_t up_state = table.At(w.Phase(up).element).state & kStateMask;
      // Alpha is the fill curve, but only for a cell whose top is open. A covered cell is
      // fully opaque however little is in it — 5 kg of water under 500 kg reads 255, not 24
      // — and "covered" counts a `LiquidImpermeable` cell above as well as a liquid or
      // solid one, which is the only place that property bit reaches a texture.
      float alpha = 1.0f;
      if (up_state < kStateLiquid && !(w.Properties()[up] & kLiquidImpermeable)) {
        alpha = LiquidFillAlpha(mass);
      }
      // The RGB is the same gradient walk for surface and submerged cells alike; a surface
      // cell simply scores 0 and lands on the first stop.
      const float pos = EstimatedLiquidGradient(w, table, out->exposed_to_sun, p, true);
      liquid_word = (static_cast<uint32_t>(static_cast<int64_t>(alpha * 255.0f)) << 24) |
                    LiquidGradientColour(el, pos);

      // LiquidData is *not* a second view of the liquid's own colour, which is what the
      // stand-in here used to assume. Its RGB is the `colour` field of the element this one
      // would **transition into**, and its alpha is the cell's temperature normalised over
      // that element's range widened by three degrees at each end:
      //
      //     tnorm = (T - (lowTemp - 3)) / ((highTemp + 3) - (lowTemp - 3))
      //
      // and the half-way point of that ramp picks which transition it is — below, the cold
      // one; at or above, the hot one. Water at 293.15 K reads 255,255,255 (ice) at alpha
      // 56, exactly. The three-degree widening is what the old code was missing when it read
      // 197.6 against Klei's 193 for water at 350 K; it is not a fudge, it is in the source
      // twice, as a subtract and an add of the same 3.0f.
      float tnorm = 0.0f;
      uint16_t transition;
      const float lo = el.lowTemp - 3.0f;
      tnorm = (projected.temperature[i] - lo) / ((el.highTemp + 3.0f) - lo);
      if (tnorm >= 0.0f) {
        if (tnorm > 1.0f) {
          tnorm = 1.0f;
          transition = el.highTempTransitionIdx;
        } else {
          transition = tnorm < 0.5f ? el.lowTempTransitionIdx : el.highTempTransitionIdx;
        }
      } else {
        tnorm = 0.0f;
        transition = el.lowTempTransitionIdx;
      }
      // A liquid with nowhere to go renders white rather than black.
      const uint32_t tcolour =
          transition == 0xFFFF ? 0xFFFFFFu : (table.At(transition).colour & 0xFFFFFFu);
      data_word = (static_cast<uint32_t>(static_cast<int64_t>(tnorm * 255.0f)) << 24) | tcolour;

      // MaterialData is `materialProperties` **masked**, not copied: 0xff00000f. Every
      // sample it was originally derived from — magma's 0x01000009, water's 0x2 — survives
      // the mask untouched, so the mask has never been observable either.
      material_word = el.materialProperties & 0xFF00000Fu;
    }

    auto store = [](std::vector<uint8_t>& tex, size_t at, uint32_t word) {
      tex[at] = static_cast<uint8_t>(word & 0xFF);
      tex[at + 1] = static_cast<uint8_t>((word >> 8) & 0xFF);
      tex[at + 2] = static_cast<uint8_t>((word >> 16) & 0xFF);
      tex[at + 3] = static_cast<uint8_t>((word >> 24) & 0xFF);
    };
    store(out->liquid, i * 4, liquid_word);
    store(out->liquid_data, i * 4, data_word);
    store(out->material_data, i * 4, material_word);
  }
  out->primed = true;
  out->table_generation = table.Generation();
  FillFlowTexture(w, out);
}

}  // namespace oni_sim
