// Shared plumbing for driving Klei's SimDLL offline.
//
// Binding, the BinaryWriter-compatible payload builder, and loading the real element
// and disease tables out of a shim corpus. driver.cpp predates this header and still
// carries its own copies; experiments.cpp uses this one.

#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../abi/sim_abi.h"

namespace simhost {

using namespace oni_sim;

// ---------------------------------------------------------------- native binding

inline HMODULE g_sim = nullptr;
inline void (*sim_initialize)(int (*)(int, void*)) = nullptr;
inline void (*sim_shutdown)() = nullptr;
inline void* (*sim_handle_message)(int, int, const uint8_t*) = nullptr;

inline int DefaultMessageHandler(int message_id, void* data) {
  const char* text = data ? *reinterpret_cast<const char* const*>(data) : nullptr;
  printf("  [sim->game] id=%d %s\n", message_id, text ? text : "(no text)");
  return 0;
}

// SIM_BeginSave hands back a pointer to a blob it still owns; SIM_EndSave releases it.
// Copy before calling EndSave, exactly as Sim.Save does.
inline uint8_t* (*sim_begin_save)(int*, int, int) = nullptr;
inline void (*sim_end_save)() = nullptr;

inline bool Bind(const char* dll) {
  g_sim = LoadLibraryA(dll);
  if (!g_sim) {
    printf("LoadLibrary failed for %s (err %lu)\n", dll, GetLastError());
    return false;
  }
  sim_initialize = reinterpret_cast<void (*)(int (*)(int, void*))>(
      GetProcAddress(g_sim, "SIM_Initialize"));
  sim_shutdown = reinterpret_cast<void (*)()>(GetProcAddress(g_sim, "SIM_Shutdown"));
  sim_handle_message = reinterpret_cast<void* (*)(int, int, const uint8_t*)>(
      GetProcAddress(g_sim, "SIM_HandleMessage"));
  sim_begin_save = reinterpret_cast<uint8_t* (*)(int*, int, int)>(
      GetProcAddress(g_sim, "SIM_BeginSave"));
  sim_end_save = reinterpret_cast<void (*)()>(GetProcAddress(g_sim, "SIM_EndSave"));
  return sim_initialize && sim_shutdown && sim_handle_message && sim_begin_save &&
         sim_end_save;
}

inline std::vector<uint8_t> Save(int x, int y) {
  int size = 0;
  const uint8_t* blob = sim_begin_save(&size, x, y);
  std::vector<uint8_t> out;
  if (blob && size > 0) out.assign(blob, blob + size);
  sim_end_save();
  return out;
}

// ---------------------------------------------------------------- payload writer

// Mirrors System.IO.BinaryWriter: little-endian, no padding, bool as one byte,
// strings as an int32 UTF-8 byte count followed by the bytes.
struct Writer {
  std::vector<uint8_t> bytes;

  template <typename T>
  void Put(const T& v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    bytes.insert(bytes.end(), p, p + sizeof(T));
  }
  void PutBool(bool v) { bytes.push_back(v ? 1 : 0); }
  void PutRaw(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    bytes.insert(bytes.end(), b, b + n);
  }
};

inline void* Send(SimMessageHash hash, const Writer& w) {
  return sim_handle_message(static_cast<int32_t>(hash),
                            static_cast<int>(w.bytes.size()),
                            w.bytes.empty() ? nullptr : w.bytes.data());
}

inline void* SendRaw(SimMessageHash hash, const std::vector<uint8_t>& b) {
  return sim_handle_message(static_cast<int32_t>(hash), static_cast<int>(b.size()),
                            b.empty() ? nullptr : b.data());
}

inline void* SendEmpty(SimMessageHash hash) {
  return sim_handle_message(static_cast<int32_t>(hash), 0, nullptr);
}

// ---------------------------------------------------------------- element table

// A hand-built element table makes PrepareGameData fault with a read at -1, so the
// real tables are lifted out of a shim corpus and replayed verbatim.
struct Tables {
  std::vector<uint8_t> elements;
  std::vector<uint8_t> diseases;
  int32_t count = 0;

  const Element* At(int32_t i) const {
    return reinterpret_cast<const Element*>(elements.data() + 4 +
                                            static_cast<size_t>(i) * sizeof(Element));
  }
  // -1 when the hash is not in the table.
  int32_t IndexOf(int32_t sim_hash) const {
    for (int32_t i = 0; i < count; ++i) {
      if (At(i)->id == sim_hash) return i;
    }
    return -1;
  }
  float SpecificHeat(uint16_t idx) const {
    return idx < count ? At(idx)->specificHeatCapacity : 0.0f;
  }
  // `state` packs the phase into the low two bits and flags above it
  // (Element.State: Unbreakable = 4, Unstable = 8), so a raw comparison against
  // Solid == 3 silently misses unstable solids like sand.
  uint8_t State(uint16_t idx) const { return idx < count ? At(idx)->state : 0; }
  uint8_t Phase(uint16_t idx) const { return State(idx) & 3; }
  bool IsUnstable(uint16_t idx) const { return (State(idx) & 8) != 0; }
};

// Corpus record: int32 seq | int32 id | int32 length | int32 count | length*count bytes.
inline bool LoadTables(const char* path, Tables* out) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    printf("cannot open corpus %s\n", path);
    return false;
  }
  for (;;) {
    int32_t h[4];
    if (fread(h, sizeof(h), 1, f) != 1) break;
    const size_t bytes = static_cast<size_t>(h[2]) * (h[3] > 0 ? h[3] : 1);
    std::vector<uint8_t> payload(bytes);
    if (bytes && fread(payload.data(), 1, bytes, f) != bytes) break;
    if (h[1] == static_cast<int32_t>(SimMessageHash::Elements_CreateTable) &&
        out->elements.empty()) {
      out->elements = std::move(payload);
    } else if (h[1] == static_cast<int32_t>(SimMessageHash::Disease_CreateTable) &&
               out->diseases.empty()) {
      out->diseases = std::move(payload);
    }
    if (!out->elements.empty() && !out->diseases.empty()) break;
  }
  fclose(f);
  if (out->elements.size() < 4 || out->diseases.empty()) return false;
  memcpy(&out->count, out->elements.data(), 4);
  return out->count > 0 &&
         4 + static_cast<size_t>(out->count) * sizeof(Element) <= out->elements.size();
}

// SimHashes values, from the game's `SimHashes` enum.
enum : int32_t {
  kVacuum = 758759285,
  kOxygen = -1528777920,
  kWater = 1836671383,
  kSteam = -899515856,
  kIce = 873952427,
  kGranite = -105943486,
  kCarbonDioxide = 1960575215,
  kHydrogen = -1046145888,
  kSand = 381796644,
  kRegolith = 1362238252,
};

}  // namespace simhost
