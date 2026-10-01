// Passthrough SimDLL shim.
//
// Exports the same symbols as Klei's SimDLL.dll and forwards every call to the
// original, which must be present alongside this file as SimDLL_orig.dll. Each
// SIM_HandleMessage(s) call is logged with its message id, payload length and
// count, and the GameDataUpdate pointer handed back by Start is recorded.
//
// The log shows which messages the game actually sends, in what order and at what size,
// and the corpus capture below records real payloads for the offline tools in driver/.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../abi/sim_abi.h"

// Hoisted above the namespace because the cached-pointer table below needs it; the
// declaration inside extern "C" was the only one before.
typedef int (*GAME_MessageHandler)(int message_id, void* data);

namespace {

using namespace oni_sim;

HMODULE g_original = nullptr;
FILE* g_log = nullptr;
CRITICAL_SECTION g_log_lock;

// Message accounting.
//
// This runs on the sim's hot path: a late-game base pushes thousands of messages per
// frame, so anything per-message here is multiplied by the size of the player's colony.
// The first version scanned a flat array twice per message (once for the histogram, once
// for "have I sampled this id") and took a lock every time. That is a real frame cost
// that grows with the base, which is exactly the wrong shape for a diagnostic tool.
//
// Now: one open-addressed lookup, lock-free, and the lock is taken only when there is
// genuinely something to capture — which after the first few seconds is never.
//
// Ids are Klei's hash values, arbitrary int32, so the table is power-of-two sized with
// linear probing. 256 slots for ~62 live ids keeps the load factor low enough that
// probes are almost always a single hit.
constexpr int kMaxTrackedIds = 128;
constexpr int kSlots = 256;  // must be a power of two, and > 2 * kMaxTrackedIds

struct IdCount {
  int32_t id;
  bool used;
  bool sampled;  // phase-2 corpus capture has a sample of this id already
  volatile LONG64 calls;
  volatile LONG64 bytes;
  volatile LONG64 ticks;  // QPC ticks spent inside Klei's handler for this id
  volatile LONG last_length;
};
IdCount g_slots[kSlots];
volatile LONG g_num_counts = 0;

inline int SlotOf(int32_t id) {
  // Knuth multiplicative; the low bits of a Klei hash are not well distributed on their
  // own and collide badly with a plain mask.
  uint32_t h = static_cast<uint32_t>(id) * 2654435761u;
  return static_cast<int>((h >> 16) & (kSlots - 1));
}

// Lock-free lookup. Entries are never removed and `used` is published after `id` is
// written, so a reader either misses a brand-new entry (and takes the insert path, which
// locks and re-checks) or sees it fully formed.
IdCount* Find(int32_t id) {
  int i = SlotOf(id);
  for (int probe = 0; probe < kSlots; ++probe) {
    IdCount* e = &g_slots[i];
    if (!e->used) return nullptr;
    if (e->id == id) return e;
    i = (i + 1) & (kSlots - 1);
  }
  return nullptr;
}

// Rare: at most once per distinct message id for the life of the process.
IdCount* InsertLocked(int32_t id) {
  int i = SlotOf(id);
  for (int probe = 0; probe < kSlots; ++probe) {
    IdCount* e = &g_slots[i];
    if (e->used && e->id == id) return e;
    if (!e->used) {
      if (g_num_counts >= kMaxTrackedIds) return nullptr;
      e->id = id;
      e->calls = 0;
      e->bytes = 0;
      e->ticks = 0;
      e->last_length = 0;
      e->sampled = false;
      MemoryBarrier();  // publish id before used, so Find() cannot see a half-built slot
      e->used = true;
      ++g_num_counts;
      return e;
    }
    i = (i + 1) & (kSlots - 1);
  }
  return nullptr;
}

// ------------------------------------------------------------------ corpus capture
//
// Reconstructing payloads by hand is error-prone, especially struct packing. Instead, record
// the real bytes so the offline driver can replay them verbatim.
//
// Two phases:
//   1. In-order capture of everything, until kBootTicks PrepareGameData calls have gone
//      by. That yields a complete, replayable boot sequence plus a few genuine ticks.
//   2. After that, one sample of each message id not yet seen, so rare messages still
//      end up in the corpus without recording gigabytes of steady-state traffic.
//
// Record format, little-endian, concatenated:
//   int32 seq | int32 msg_id | int32 length | int32 count | length*count bytes

FILE* g_corpus = nullptr;
int32_t g_corpus_seq = 0;
volatile LONG g_prepare_seen = 0;
uint64_t g_corpus_bytes = 0;
bool g_corpus_full = false;
// Set once no message can ever produce another capture, so the hot path can bail out
// before it touches the lock. Written under the lock, read without it; a stale `false`
// costs one extra locked no-op, which is harmless.
volatile bool g_capture_done = false;

// TWENTY, NOT FIVE, AND THE REASON IS ONE FRAME IN TWELVE. `PrepareGameData` arrives once per
// RENDERED frame, so this counts frames, not sim ticks — but the sim only ticks on every twelfth
// sub-tick (`Game.SimEveryTick`), and the tick frame is the only one that sends
// `NewGameFrame(0.2f)`; the other eleven send `NewGameFrame(0f)`. A five-frame window therefore
// captured a 200 ms tick only if the machine happened to reach one within five frames, so
// whether the corpus holds a real ticking frame was luck.
//
// Twenty frames always contains one. It costs about 45 KB — three messages a frame, of which
// `PrepareGameData` is 3072 bytes — against a corpus that is otherwise a quarter of a megabyte,
// and it buys a corpus that advances the sim instead of one made only of idle frames.
constexpr int kBootTicks = 20;
constexpr uint64_t kCorpusByteLimit = 512ull * 1024 * 1024;
constexpr int32_t kPrepareGameData = 1078620451;

// "Have I sampled this id" now lives in the histogram entry, so the phase-2 check is the
// same O(1) lookup the histogram already did rather than a second scan.

// Caller must hold g_log_lock. `entry` is this id's slot, already found by the caller.
void CaptureLocked(int32_t id, int32_t length, int32_t count, const uint8_t* msg,
                   IdCount* entry) {
  if (!g_corpus || g_corpus_full) return;

  const bool booting = g_prepare_seen < kBootTicks;
  if (!booting && entry && entry->sampled) return;
  if (entry) entry->sampled = true;

  const uint64_t payload = static_cast<uint64_t>(length) * (count > 0 ? count : 1);
  if (g_corpus_bytes + payload > kCorpusByteLimit) {
    g_corpus_full = true;
    return;
  }

  const int32_t header[4] = {g_corpus_seq++, id, length, count};
  fwrite(header, sizeof(header), 1, g_corpus);
  if (msg && payload) fwrite(msg, 1, static_cast<size_t>(payload), g_corpus);
  g_corpus_bytes += payload;

  // FLUSH EVERY POST-BOOT CAPTURE, and the reason is the order of operations in the caller:
  // `Observe` captures BEFORE the call is forwarded to Klei. So a message that kills the sim is
  // already in this FILE's buffer and not yet on disk, and the flush at `SIM_Shutdown` never
  // runs. The records most likely to be the last thing a process does — the ones that rewrite
  // grid structure, `SimData_ResizeAndInitializeVacuumCells` and `SimData_FreeCells` — are also
  // the rarest in the corpus, so this loses precisely the records that are hardest to record
  // again.
  //
  // Free on the measurement that matters: post-boot captures are one per never-before-seen id,
  // so this runs at most ~60 times in a process and never once in steady state. Boot is left
  // buffered because phase 1 writes hundreds of records back to back.
  if (!booting) fflush(g_corpus);
}

// The whole per-message cost, and the only thing on the hot path.
//
// Steady state is: one hash lookup, two interlocked adds, one store, and two predictable
// branches. No lock, no scan, no I/O — capture is finished by then, so `sampled` is set
// for every id the game sends and `g_capture_done` short-circuits even that.
IdCount* Observe(int32_t id, int32_t length, int32_t count, const uint8_t* msg) {
  IdCount* e = Find(id);
  if (!e) {
    // First sighting of this id. Locked, and at most ~62 times per process.
    EnterCriticalSection(&g_log_lock);
    e = InsertLocked(id);
    LeaveCriticalSection(&g_log_lock);
    if (!e) return nullptr;  // table full: stop counting rather than corrupt anything
  }
  InterlockedAdd64(&e->calls, count);
  InterlockedAdd64(&e->bytes, static_cast<LONG64>(length) * count);
  e->last_length = length;

  // Capture is the expensive part, so decide whether to skip it without taking the lock.
  // During boot everything is captured; afterwards only ids never seen before, which
  // stops happening within seconds of a game starting.
  if (g_capture_done) return e;
  const bool booting = g_prepare_seen < kBootTicks;
  if (!booting && e->sampled) return e;

  EnterCriticalSection(&g_log_lock);
  CaptureLocked(id, length, count, msg, e);
  // Once the corpus is closed or full there is nothing left to do on any future message.
  if (!g_corpus || g_corpus_full) g_capture_done = true;
  LeaveCriticalSection(&g_log_lock);
  return e;
}

// ------------------------------------------------------------------ export timing
//
// Two clocks, and they do not measure the same thing.
//
// Clock A is this one: QPC taken either side of each forwarded call, call-in to
// call-out. That is the honest cost of the export as the game experiences it -- but
// only for work Klei does synchronously. ONI runs the simulation on its own thread, so
// an export that merely queues work returns in nanoseconds however expensive the work
// turns out to be.
//
// Clock B is frame accounting, further down. GameDataUpdate::numFramesProcessed says
// how many sim frames an update covers, so the wall time between consecutive updates
// divided by the frames they report is the rate the sim is actually sustaining.
//
// Neither alone is the number we are after, which is why both are collected. If Clock A
// on PrepareGameData is large, the game is blocking on the sim there and that block is
// the cost. If it stays small while Clock B stretches, the sim is keeping up
// asynchronously and its cost is invisible at this boundary. The capture decides which,
// and guessing in advance is how you end up with a baseline that measures the handoff.

enum ExportId {
  kEx_Initialize,
  kEx_Shutdown,
  kEx_HandleMessage,
  kEx_HandleMessages,
  kEx_BeginSave,
  kEx_EndSave,
  kEx_DebugCrash,
  kEx_SysinfoAcquire,
  kEx_SysinfoRelease,
  kEx_CtmInitialize,
  kEx_CtmShutdown,
  kEx_CtmAdd,
  kEx_CtmRemove,
  kEx_CtmSet,
  kEx_CtmClear,
  kEx_CtmUpdate,
  kEx_Count,
};

const char* const kExportName[kEx_Count] = {
    "SIM_Initialize",
    "SIM_Shutdown",
    "SIM_HandleMessage",
    "SIM_HandleMessages",
    "SIM_BeginSave",
    "SIM_EndSave",
    "SIM_DebugCrash",
    "SYSINFO_Acquire",
    "SYSINFO_Release",
    "ConduitTemperatureManager_Initialize",
    "ConduitTemperatureManager_Shutdown",
    "ConduitTemperatureManager_Add",
    "ConduitTemperatureManager_Remove",
    "ConduitTemperatureManager_Set",
    "ConduitTemperatureManager_Clear",
    "ConduitTemperatureManager_Update",
};

struct ExportStat {
  volatile LONG64 calls;
  volatile LONG64 ticks;
  volatile LONG64 max_ticks;
};
ExportStat g_export[kEx_Count];

LONG64 g_qpc_freq = 1;

// Scoped: the constructor stamps call-in, the destructor stamps call-out, so an export
// with several return paths cannot forget one.
struct Timed {
  ExportStat* stat;
  LARGE_INTEGER t0;
  LONG64 elapsed;  // readable by the caller after the call, for per-message charging

  explicit Timed(ExportId id) : stat(&g_export[id]), elapsed(0) {
    QueryPerformanceCounter(&t0);
  }
  ~Timed() { Stop(); }

  // Idempotent, so an export that wants the elapsed count before the scope ends can ask
  // for it without being charged twice.
  LONG64 Stop() {
    if (elapsed) return elapsed;
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    elapsed = t1.QuadPart - t0.QuadPart;
    if (elapsed <= 0) elapsed = 1;  // a same-tick call still happened
    InterlockedIncrement64(&stat->calls);
    InterlockedAdd64(&stat->ticks, elapsed);
    // Racy on purpose. A lost update costs one sample of a statistic that is a hint,
    // not a measurement, and a CAS loop on the hot path would cost more than it buys.
    if (elapsed > stat->max_ticks) stat->max_ticks = elapsed;
    return elapsed;
  }
};

// ---- Clock B: sim frame accounting.
//
// Touched only from the thread that calls PrepareGameData, so no interlocks; the
// snapshot writer reads slightly stale values and says so.
LONG64 g_sim_frames = 0;       // sum of numFramesProcessed
LONG64 g_prepare_calls = 0;    // PrepareGameData calls seen
LONG64 g_prepare_first = 0;    // QPC at the first one
LONG64 g_prepare_last = 0;     // QPC at the most recent one
// How many sim frames each update covered. Klei can report more than one, which is the
// signal that the sim fell behind and batched -- the only in-band evidence of that.
LONG64 g_frames_hist[8];

void NoteFrames(int32_t frames) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (!g_prepare_first) g_prepare_first = now.QuadPart;
  g_prepare_last = now.QuadPart;
  ++g_prepare_calls;
  if (frames > 0) g_sim_frames += frames;
  const int bucket = (frames < 0) ? 0 : (frames > 6 ? 7 : frames);
  ++g_frames_hist[bucket];
}

// ------------------------------------------------------------------ field notebook
//
// Written for one purpose: a live game, with real machinery running, is the only place
// several open questions can be answered, and asking the player to replay a session is
// expensive. So everything still unknown gets its evidence collected in one pass.
//
// What this is after, and why the offline harness cannot get it:
//   * Liquid texture alpha. Klei reports 255 at 98% full and 221 at 73%; no scaling of
//     the cell's own mass reproduces that, so real (mass, neighbours, alpha) triples are
//     needed.
//   * LiquidData RGB. Constant per element but matches none of that element's colours.
//   * ExposedToSunlight. Zero in every offline world including an open shaft, so its
//     driving input is something worldgen sends. A real surface base has actual sunlight.
//   * unstableCellInfo and spawnFallingLiquidInfo. Never fired offline; falling sand and
//     falling water are routine in a real game.
//   * Building heat delivery, which is off by ~1.26% at 1 kW in a way I could not
//     explain. Real buildings with known wattage give a second data set.
//   * Conduit temperatures, which are a whole unmodelled subsystem.
//
// Cost discipline, because this runs while someone is playing: the per-frame work is one
// counter, one modulo and about ten integer reads of counts that are already in cache.
// Everything expensive happens on a duty cycle, is bounded by a per-category budget, and
// stops entirely once the file hits its cap.
FILE* g_notes = nullptr;
uint64_t g_notes_bytes = 0;
int64_t g_frames = 0;
int32_t g_width = 0, g_height = 0, g_cells = 0;

constexpr uint64_t kNotesByteLimit = 24ull * 1024 * 1024;
// The sim runs at 5 Hz, so this is roughly one sample sweep every three minutes.
constexpr int kSampleEveryFrames = 900;

// Per-category caps, so one busy session cannot drown the interesting rare events.
struct Budget {
  const char* name;
  int used;
  int cap;
};
Budget g_budget[] = {
    {"unstable", 0, 400},   {"fallingliquid", 0, 400}, {"cellmelted", 0, 200},
    {"overheat", 0, 200},   {"buildingmelt", 0, 100},  {"disease", 0, 200},
    {"worlddamage", 0, 200}, {"backwall", 0, 200},     {"radiation", 0, 200},
    {"chunk", 0, 200},      {"handles", 0, 300},       {"liquidtex", 0, 4000},
    {"suntex", 0, 4000},    {"flowtex", 0, 2000},      {"buildingtemp", 0, 3000},
    {"conduit", 0, 400},    {"frameshape", 0, 400},
};

Budget* Spend(const char* name) {
  for (Budget& b : g_budget) {
    if (strcmp(b.name, name) == 0) {
      if (b.used >= b.cap) return nullptr;
      ++b.used;
      return &b;
    }
  }
  return nullptr;
}

// Caller must hold g_log_lock.
void NoteLocked(const char* fmt, ...) {
  if (!g_notes || g_notes_bytes > kNotesByteLimit) return;
  va_list args;
  va_start(args, fmt);
  const int n = vfprintf(g_notes, fmt, args);
  va_end(args);
  fputc('\n', g_notes);
  if (n > 0) g_notes_bytes += static_cast<uint64_t>(n) + 1;
}

// A count that is negative or absurd means the struct is being misread; refuse to walk
// the array rather than fault inside the player's game.
inline bool SaneCount(int32_t n) { return n > 0 && n < 4000000; }

void Note(const char* fmt, ...) {
  if (!g_notes || g_notes_bytes > kNotesByteLimit) return;
  EnterCriticalSection(&g_log_lock);
  va_list args;
  va_start(args, fmt);
  const int n = vfprintf(g_notes, fmt, args);
  va_end(args);
  fputc('\n', g_notes);
  if (n > 0) g_notes_bytes += static_cast<uint64_t>(n) + 1;
  LeaveCriticalSection(&g_log_lock);
}

// Rare events. Checked every frame, but the check is just reading counts that the sim
// wrote moments ago, and the body only runs when something actually happened.
void NoteRareEvents(const GameDataUpdate* u) {
  auto dump_ore = [&](const char* tag, int32_t n, const SpawnOreInfo* v) {
    if (!SaneCount(n) || !v) return;
    for (int32_t i = 0; i < n && i < 8; ++i) {
      if (!Spend(tag)) return;
      NoteLocked("%s f=%lld cell=%d elem=%u mass=%.4f temp=%.3f disease=%u/%d", tag,
                 (long long)g_frames, v[i].cellIdx, v[i].elemIdx, v[i].mass,
                 v[i].temperature, v[i].diseaseIdx, v[i].diseaseCount);
    }
  };

  // Falling sand. Never once observed offline, and the reason is still unknown — this is
  // the single most wanted record in this file.
  if (SaneCount(u->numUnstableCellInfo) && u->unstableCellInfo) {
    for (int32_t i = 0; i < u->numUnstableCellInfo && i < 8; ++i) {
      if (!Spend("unstable")) break;
      const UnstableCellInfo& e = u->unstableCellInfo[i];
      NoteLocked("unstable f=%lld cell=%d elem=%u falling=%u mass=%.4f temp=%.3f",
                 (long long)g_frames, e.cellIdx, e.elemIdx, e.fallingInfo, e.mass,
                 e.temperature);
    }
  }
  if (SaneCount(u->numSpawnFallingLiquidInfo) && u->spawnFallingLiquidInfo) {
    for (int32_t i = 0; i < u->numSpawnFallingLiquidInfo && i < 8; ++i) {
      if (!Spend("fallingliquid")) break;
      const SpawnFallingLiquidInfo& e = u->spawnFallingLiquidInfo[i];
      NoteLocked("fallingliquid f=%lld cell=%d elem=%u mass=%.4f temp=%.3f",
                 (long long)g_frames, e.cellIdx, e.elemIdx, e.mass, e.temperature);
    }
  }
  dump_ore("spawnore", u->numSpawnOreInfo, u->spawnOreInfo);

  if (SaneCount(u->numCellMeltedInfos) && u->cellMeltedInfos) {
    for (int32_t i = 0; i < u->numCellMeltedInfos && i < 8; ++i) {
      if (!Spend("cellmelted")) break;
      NoteLocked("cellmelted f=%lld cell=%d", (long long)g_frames,
                 u->cellMeltedInfos[i].gameCell);
    }
  }
  if (SaneCount(u->numBuildingOverheatInfos) && u->buildingOverheatInfos) {
    for (int32_t i = 0; i < u->numBuildingOverheatInfos && i < 4; ++i) {
      if (!Spend("overheat")) break;
      NoteLocked("overheat f=%lld handle=%d", (long long)g_frames,
                 u->buildingOverheatInfos[i].handle);
    }
  }
  if (SaneCount(u->numBuildingMeltedInfos) && u->buildingMeltedInfos) {
    for (int32_t i = 0; i < u->numBuildingMeltedInfos && i < 4; ++i) {
      if (!Spend("buildingmelt")) break;
      NoteLocked("buildingmelt f=%lld handle=%d", (long long)g_frames,
                 u->buildingMeltedInfos[i].handle);
    }
  }
  if (SaneCount(u->numDiseaseEmittedInfos) && u->diseaseEmittedInfos) {
    for (int32_t i = 0; i < u->numDiseaseEmittedInfos && i < 4; ++i) {
      if (!Spend("disease")) break;
      NoteLocked("diseaseemitted f=%lld disease=%u count=%d", (long long)g_frames,
                 u->diseaseEmittedInfos[i].diseaseIdx,
                 u->diseaseEmittedInfos[i].count);
    }
  }
  if (SaneCount(u->numWorldDamageInfo) && u->worldDamageInfo) {
    for (int32_t i = 0; i < u->numWorldDamageInfo && i < 4; ++i) {
      if (!Spend("worlddamage")) break;
      NoteLocked("worlddamage f=%lld cell=%d srcoffset=%d", (long long)g_frames,
                 u->worldDamageInfo[i].gameCell,
                 u->worldDamageInfo[i].damageSourceOffset);
    }
  }
  if (SaneCount(u->numBackwallElementChangedInfos) && u->backwallElementChangedInfos) {
    for (int32_t i = 0; i < u->numBackwallElementChangedInfos && i < 4; ++i) {
      if (!Spend("backwall")) break;
      NoteLocked("backwallchanged f=%lld cell=%d", (long long)g_frames,
                 u->backwallElementChangedInfos[i].gameCell);
    }
  }
  if (SaneCount(u->numRadiationConsumedCallbacks) && u->radiationConsumedCallbacks) {
    for (int32_t i = 0; i < u->numRadiationConsumedCallbacks && i < 4; ++i) {
      if (!Spend("radiation")) break;
      NoteLocked("radiationconsumed f=%lld cell=%d rads=%.4f", (long long)g_frames,
                 u->radiationConsumedCallbacks[i].gameCell,
                 u->radiationConsumedCallbacks[i].radiation);
    }
  }
  if (SaneCount(u->numElementChunkInfos) && u->elementChunkInfos) {
    for (int32_t i = 0; i < u->numElementChunkInfos && i < 4; ++i) {
      if (!Spend("chunk")) break;
      NoteLocked("elementchunk f=%lld temp=%.3f deltaKJ=%.4f", (long long)g_frames,
                 u->elementChunkInfos[i].temperature,
                 u->elementChunkInfos[i].deltaKJ);
    }
  }
  // Handle registrations map a callback index to a sim handle. Worth having the real
  // mapping for buildings, consumers and emitters.
  if (SaneCount(u->numComponentStateChangedMessages) && u->componentStateChangedMessages) {
    for (int32_t i = 0; i < u->numComponentStateChangedMessages && i < 8; ++i) {
      if (!Spend("handles")) break;
      NoteLocked("handle f=%lld cb=%d simhandle=%d", (long long)g_frames,
                 u->componentStateChangedMessages[i].callbackIdx,
                 u->componentStateChangedMessages[i].simHandle);
    }
  }
}

// The duty-cycle sweep. Everything here is bounded by a sample cap, not by grid size, so
// its cost does not grow with the size of the player's base.
void NoteSamples(const GameDataUpdate* u) {
  if (g_cells <= 0 || g_width <= 0) return;

  // The shape of a real frame: how many of each event list a live base actually
  // produces. Nothing offline can tell me this, and it decides what a replacement has to
  // make fast.
  if (Spend("frameshape")) {
    NoteLocked("frameshape f=%lld solid=%d substance=%d liquidchange=%d callback=%d"
               " fallliquid=%d dig=%d ore=%d fx=%d unstable=%d damage=%d buildtemp=%d"
               " massconsumed=%d massemitted=%d comp=%d removedmass=%d emittedmass=%d"
               " chunk=%d cellmelt=%d diseaseemit=%d radconsumed=%d",
               (long long)g_frames, u->numSolidInfo, u->numSubstanceChangeInfo,
               u->numLiquidChangeInfo, u->numCallbackInfo, u->numSpawnFallingLiquidInfo,
               u->numDigInfo, u->numSpawnOreInfo, u->numSpawnFXInfo,
               u->numUnstableCellInfo, u->numWorldDamageInfo, u->numBuildingTemperatures,
               u->numMassConsumedCallbacks, u->numMassEmittedCallbacks,
               u->numComponentStateChangedMessages, u->numRemovedMassEntries,
               u->numEmittedMassEntries, u->numElementChunkInfos, u->numCellMeltedInfos,
               u->numDiseaseEmittedInfos, u->numRadiationConsumedCallbacks);
  }

  const auto* liquid = static_cast<const uint8_t*>(u->propertyTextureLiquid);
  const auto* liquid_data = static_cast<const uint8_t*>(u->propertyTextureLiquidData);
  const auto* material = static_cast<const uint8_t*>(u->propertyTextureMaterialData);
  const auto* sun = static_cast<const uint8_t*>(u->propertyTextureExposedToSunlight);
  const auto* flow = static_cast<const float*>(u->propertyTextureFlow);

  // Walk with a stride rather than scanning the whole grid, and stop at a fixed budget.
  // A prime-ish stride avoids sampling the same column of a regular base every sweep.
  const int32_t stride = 37;
  int liquid_taken = 0, sun_taken = 0, flow_taken = 0;
  for (int32_t c = static_cast<int32_t>(g_frames / kSampleEveryFrames) % stride;
       c < g_cells; c += stride) {
    if (liquid_taken >= 24 && sun_taken >= 24 && flow_taken >= 12) break;
    const int32_t x = c % g_width, y = c / g_width;

    // Liquid alpha against mass, plus the neighbour above and below, since the alpha
    // curve looks like it sees its neighbours rather than only this cell.
    if (liquid && liquid_data && material && u->mass && u->elementIdx &&
        liquid[c * 4 + 3] != 0 && liquid_taken < 24) {
      const int32_t above = c + g_width, below = c - g_width;
      const float m_above =
          (above < g_cells && u->mass) ? u->mass[above] : -1.0f;
      const float m_below = (below >= 0 && u->mass) ? u->mass[below] : -1.0f;
      if (Spend("liquidtex")) {
        ++liquid_taken;
        NoteLocked("liquidtex f=%lld c=%d x=%d y=%d elem=%u mass=%.4f temp=%.3f"
                   " above=%.4f below=%.4f liq=%u,%u,%u,%u data=%u,%u,%u,%u"
                   " mat=%u,%u,%u,%u",
                   (long long)g_frames, c, x, y, u->elementIdx[c], u->mass[c],
                   u->temperature ? u->temperature[c] : 0.0f, m_above, m_below,
                   liquid[c * 4], liquid[c * 4 + 1], liquid[c * 4 + 2], liquid[c * 4 + 3],
                   liquid_data[c * 4], liquid_data[c * 4 + 1], liquid_data[c * 4 + 2],
                   liquid_data[c * 4 + 3], material[c * 4], material[c * 4 + 1],
                   material[c * 4 + 2], material[c * 4 + 3]);
      }
    }

    // Sunlight, the texture whose driving input is entirely unknown. Only lit cells are
    // interesting — a dark cell tells me nothing I do not already have.
    if (sun && sun[c] != 0 && sun_taken < 24 && u->elementIdx && u->mass) {
      if (Spend("suntex")) {
        ++sun_taken;
        NoteLocked("suntex f=%lld c=%d x=%d y=%d elem=%u mass=%.4f sun=%u",
                   (long long)g_frames, c, x, y, u->elementIdx[c], u->mass[c], sun[c]);
      }
    }

    // Flow, to find out what the two floats actually mean.
    if (flow && flow_taken < 12 && (flow[c * 2] != 0.0f || flow[c * 2 + 1] != 0.0f) &&
        u->elementIdx && u->mass) {
      if (Spend("flowtex")) {
        ++flow_taken;
        NoteLocked("flowtex f=%lld c=%d x=%d y=%d elem=%u mass=%.4f flow=%.6f,%.6f",
                   (long long)g_frames, c, x, y, u->elementIdx[c], u->mass[c],
                   flow[c * 2], flow[c * 2 + 1]);
      }
    }
  }

  // Building temperatures over time. Paired with the AddBuildingHeatExchange payloads
  // already in the corpus, this gives wattage-in against temperature-out for real
  // buildings — a second data set for the 1.26% discrepancy that offline testing could
  // not explain.
  if (SaneCount(u->numBuildingTemperatures) && u->buildingTemperatures) {
    for (int32_t i = 0; i < u->numBuildingTemperatures && i < 12; ++i) {
      if (!Spend("buildingtemp")) break;
      NoteLocked("buildingtemp f=%lld handle=%d temp=%.4f", (long long)g_frames,
                 u->buildingTemperatures[i].handle,
                 u->buildingTemperatures[i].temperature);
    }
  }
}

void Log(const char* fmt, ...) {
  if (!g_log) return;
  EnterCriticalSection(&g_log_lock);
  va_list args;
  va_start(args, fmt);
  vfprintf(g_log, fmt, args);
  va_end(args);
  fputc('\n', g_log);
  fflush(g_log);
  LeaveCriticalSection(&g_log_lock);
}

// The directory this DLL was loaded from. Every artefact the shim writes lives beside
// it, so the path is resolved once in DllMain rather than rebuilt per file.
char g_dir[MAX_PATH] = {0};

// Rewritten in place on a duty cycle rather than appended, so the file on disk is always
// one complete snapshot. A session that ends in a crash -- which is exactly the session
// worth having numbers from -- still leaves a readable report behind.
void WriteTimingReport(const char* why) {
  if (!g_dir[0]) return;
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%ssim_timing.log", g_dir);
  FILE* f = fopen(path, "w");
  if (!f) return;

  const double us = 1e6 / static_cast<double>(g_qpc_freq ? g_qpc_freq : 1);

  fprintf(f, "# SimDLL shim timing report (%s)\n", why);
  fprintf(f, "# qpc_freq %lld\n", (long long)g_qpc_freq);
  fprintf(f, "# NOTE: Clock A below times each export call-in to call-out. Work Klei\n");
  fprintf(f, "#       does on the sim thread is NOT included unless the export blocks\n");
  fprintf(f, "#       on it. Compare against Clock B before believing either.\n");

  fprintf(f, "\n[clockA.exports]\n");
  fprintf(f, "%-38s %10s %14s %12s %12s\n", "export", "calls", "total_us", "mean_us",
          "max_us");
  for (int i = 0; i < kEx_Count; ++i) {
    const LONG64 calls = g_export[i].calls;
    if (!calls) continue;
    const double total = static_cast<double>(g_export[i].ticks) * us;
    fprintf(f, "%-38s %10lld %14.1f %12.3f %12.3f\n", kExportName[i], (long long)calls,
            total, total / static_cast<double>(calls),
            static_cast<double>(g_export[i].max_ticks) * us);
  }

  // Per-message-id cost. SIM_HandleMessage is one export covering sixty-odd different
  // handlers, so its mean on its own says nothing; this is the split that matters and
  // it doubles as the call census.
  fprintf(f, "\n[clockA.messages]\n");
  fprintf(f, "%12s %12s %14s %12s %14s %8s\n", "msg_id", "calls", "total_us", "mean_us",
          "bytes", "last_len");
  for (int i = 0; i < kSlots; ++i) {
    if (!g_slots[i].used) continue;
    const LONG64 calls = g_slots[i].calls;
    const double total = static_cast<double>(g_slots[i].ticks) * us;
    fprintf(f, "%12d %12lld %14.1f %12.4f %14llu %8d\n", g_slots[i].id, (long long)calls,
            total, calls ? total / static_cast<double>(calls) : 0.0,
            (unsigned long long)g_slots[i].bytes, (int)g_slots[i].last_length);
  }

  fprintf(f, "\n[clockB.frames]\n");
  const double wall_us = static_cast<double>(g_prepare_last - g_prepare_first) * us;
  fprintf(f, "prepare_calls   %lld\n", (long long)g_prepare_calls);
  fprintf(f, "sim_frames      %lld\n", (long long)g_sim_frames);
  fprintf(f, "wall_us         %.1f\n", wall_us);
  fprintf(f, "us_per_simframe %.3f\n",
          g_sim_frames ? wall_us / static_cast<double>(g_sim_frames) : 0.0);
  fprintf(f, "us_per_prepare  %.3f\n",
          g_prepare_calls ? wall_us / static_cast<double>(g_prepare_calls) : 0.0);
  // A bucket above 1 means one update carried several sim frames, i.e. the sim was
  // behind and caught up in a batch. At a steady 60fps against a 5Hz sim, bucket 0
  // dominates and anything past bucket 1 is the interesting part of the distribution.
  for (int i = 0; i < 8; ++i) {
    fprintf(f, "framesProcessed[%d%s] %lld\n", i, i == 7 ? "+" : "",
            (long long)g_frames_hist[i]);
  }

  fclose(f);
}

// Snapshot cadence. PrepareGameData arrives once per rendered frame, so this is roughly
// one rewrite a minute -- far off the hot path, and frequent enough that a crash costs
// at most a minute of accounting.
constexpr LONG64 kSnapshotEveryPrepares = 3000;

// Resolve the original export on first use. Returning null here would crash the
// game with no explanation, so a missing symbol is logged and then fatal.
FARPROC Resolve(const char* name) {
  FARPROC p = g_original ? GetProcAddress(g_original, name) : nullptr;
  if (!p) {
    Log("FATAL: SimDLL_orig.dll has no export '%s'", name);
    MessageBoxA(nullptr, name, "SimDLL shim: missing export", MB_ICONERROR);
    ExitProcess(1);
  }
  return p;
}

template <typename Fn>
Fn Bind(const char* name) {
  return reinterpret_cast<Fn>(Resolve(name));
}

// Resolved once, at attach.
//
// Every forwarded call used to go through Bind(), which is a GetProcAddress -- an
// export-table binary search -- per call. On SIM_HandleMessage that is once per message,
// thousands of times a frame in a late-game base. A passthrough log never showed it
// because the log only counted messages; now that this file times them, that search sits
// inside the measurement window and would be attributed to Klei. It has to go before any
// number here means anything.
struct Orig {
  void (*Initialize)(GAME_MessageHandler);
  void (*Shutdown)();
  void* (*HandleMessage)(int, int, uint8_t*);
  void* (*HandleMessages)(int, int, int, uint8_t*);
  uint8_t* (*BeginSave)(int*, int, int);
  void (*EndSave)();
  void (*DebugCrash)();
  char* (*SysinfoAcquire)();
  void (*SysinfoRelease)();
  void (*CtmInitialize)();
  void (*CtmShutdown)();
  int (*CtmAdd)(float, float, int, int, float, float, int32_t);
  void (*CtmRemove)(int);
  int (*CtmSet)(int, float, float, int);
  void (*CtmClear)();
  void* (*CtmUpdate)(float, void*);
};
Orig g_orig;

void ResolveAll() {
  g_orig.Initialize = Bind<void (*)(GAME_MessageHandler)>("SIM_Initialize");
  g_orig.Shutdown = Bind<void (*)()>("SIM_Shutdown");
  g_orig.HandleMessage = Bind<void* (*)(int, int, uint8_t*)>("SIM_HandleMessage");
  g_orig.HandleMessages = Bind<void* (*)(int, int, int, uint8_t*)>("SIM_HandleMessages");
  g_orig.BeginSave = Bind<uint8_t* (*)(int*, int, int)>("SIM_BeginSave");
  g_orig.EndSave = Bind<void (*)()>("SIM_EndSave");
  g_orig.DebugCrash = Bind<void (*)()>("SIM_DebugCrash");
  g_orig.SysinfoAcquire = Bind<char* (*)()>("SYSINFO_Acquire");
  g_orig.SysinfoRelease = Bind<void (*)()>("SYSINFO_Release");
  g_orig.CtmInitialize = Bind<void (*)()>("ConduitTemperatureManager_Initialize");
  g_orig.CtmShutdown = Bind<void (*)()>("ConduitTemperatureManager_Shutdown");
  g_orig.CtmAdd = Bind<int (*)(float, float, int, int, float, float, int32_t)>(
      "ConduitTemperatureManager_Add");
  g_orig.CtmRemove = Bind<void (*)(int)>("ConduitTemperatureManager_Remove");
  g_orig.CtmSet = Bind<int (*)(int, float, float, int)>("ConduitTemperatureManager_Set");
  g_orig.CtmClear = Bind<void (*)()>("ConduitTemperatureManager_Clear");
  g_orig.CtmUpdate = Bind<void* (*)(float, void*)>("ConduitTemperatureManager_Update");
}

}  // namespace

extern "C" {

__declspec(dllexport) void SIM_Initialize(GAME_MessageHandler callback) {
  Log("SIM_Initialize callback=%p", reinterpret_cast<void*>(callback));
  Timed t(kEx_Initialize);
  g_orig.Initialize(callback);
}

__declspec(dllexport) void SIM_Shutdown() {
  Log("SIM_Shutdown");
  // Flush now rather than at DLL_PROCESS_DETACH: the corpus is the expensive artefact
  // to re-collect, and a crash later in the session would otherwise lose it.
  if (g_corpus) fflush(g_corpus);
  // The notebook is buffered for speed, so it must be flushed at every teardown or a
  // crash late in a session loses the rare events it exists to catch.
  if (g_notes) {
    EnterCriticalSection(&g_log_lock);
    NoteLocked("=== shutdown at frame %lld, %llu bytes written ===", (long long)g_frames,
               (unsigned long long)g_notes_bytes);
    fflush(g_notes);
    LeaveCriticalSection(&g_log_lock);
  }
  Log("corpus: %d records, %llu payload bytes%s", g_corpus_seq,
      static_cast<unsigned long long>(g_corpus_bytes),
      g_corpus_full ? " (LIMIT REACHED, truncated)" : "");
  // The histogram now lives in a hash table, so this walks slots rather than a dense
  // prefix. Order is no longer insertion order; check_log.py keys on the id, not the row.
  Log("--- message histogram (id, calls, bytes, last_payload_len) ---");
  for (int i = 0; i < kSlots; ++i) {
    if (!g_slots[i].used) continue;
    Log("%11d %10llu %12llu %6d", g_slots[i].id,
        static_cast<unsigned long long>(g_slots[i].calls),
        static_cast<unsigned long long>(g_slots[i].bytes),
        static_cast<int>(g_slots[i].last_length));
  }
  WriteTimingReport("SIM_Shutdown");
  Timed t(kEx_Shutdown);
  g_orig.Shutdown();
}

__declspec(dllexport) void* SIM_HandleMessage(int sim_msg_id, int msg_length,
                                              uint8_t* msg) {
  IdCount* slot = Observe(sim_msg_id, msg_length, 1, msg);
  if (sim_msg_id == kPrepareGameData && g_prepare_seen < kBootTicks) {
    EnterCriticalSection(&g_log_lock);
    ++g_prepare_seen;
    LeaveCriticalSection(&g_log_lock);
  }
  // Grid dimensions arrive in the first two ints of either allocation message, and the
  // notebook needs them to turn a cell index into x,y.
  if ((sim_msg_id == 1092408308 || sim_msg_id == 2062421945) && msg && msg_length >= 8) {
    memcpy(&g_width, msg, 4);
    memcpy(&g_height, msg + 4, 4);
    g_cells = (g_width > 0 && g_height > 0) ? g_width * g_height : 0;
  }

  void* result;
  {
    Timed t(kEx_HandleMessage);
    result = g_orig.HandleMessage(sim_msg_id, msg_length, msg);
    const LONG64 elapsed = t.Stop();
    // Charge the same interval to this message id. One export, sixty-odd handlers: the
    // export mean is meaningless without this split, and the split is the call census.
    if (slot) InterlockedAdd64(&slot->ticks, elapsed);
  }
  // -931446686 is SimMessageHashes.Start, the one call whose return value is the
  // GameDataUpdate the managed Grid arrays are bound to.
  if (sim_msg_id == -931446686) {
    Log("Start -> GameDataUpdate* = %p", result);
  }

  // Clock B. Kept outside the notebook's guard below on purpose: frame accounting has
  // to survive the notebook filling up, and it is the half of the measurement that sees
  // work the sim thread did without blocking this call.
  if (sim_msg_id == kPrepareGameData && result) {
    NoteFrames(static_cast<const GameDataUpdate*>(result)->numFramesProcessed);
    if (g_prepare_calls % kSnapshotEveryPrepares == 0) WriteTimingReport("snapshot");
  }

  // The notebook reads the update the sim has just produced. Everything below is off the
  // hot path in the sense that matters: one counter, one compare, and a handful of
  // integer reads unless something rare happened or the duty cycle came round.
  if (sim_msg_id == kPrepareGameData && result && g_notes &&
      g_notes_bytes <= kNotesByteLimit) {
    const GameDataUpdate* u = static_cast<const GameDataUpdate*>(result);
    ++g_frames;
    EnterCriticalSection(&g_log_lock);
    NoteRareEvents(u);
    if (g_frames % kSampleEveryFrames == 0) NoteSamples(u);
    LeaveCriticalSection(&g_log_lock);
  }
  return result;
}

__declspec(dllexport) void* SIM_HandleMessages(int sim_msg_id, int msg_length,
                                               int msg_count, uint8_t* msg) {
  IdCount* slot = Observe(sim_msg_id, msg_length, msg_count, msg);
  Timed t(kEx_HandleMessages);
  void* result = g_orig.HandleMessages(sim_msg_id, msg_length, msg_count, msg);
  const LONG64 elapsed = t.Stop();
  if (slot) InterlockedAdd64(&slot->ticks, elapsed);
  return result;
}

__declspec(dllexport) uint8_t* SIM_BeginSave(int* size, int x, int y) {
  Timed t(kEx_BeginSave);
  uint8_t* result = g_orig.BeginSave(size, x, y);
  t.Stop();
  Log("SIM_BeginSave x=%d y=%d -> %d bytes at %p", x, y, size ? *size : -1, result);
  return result;
}

__declspec(dllexport) void SIM_EndSave() {
  Timed t(kEx_EndSave);
  g_orig.EndSave();
}

__declspec(dllexport) void SIM_DebugCrash() {
  Timed t(kEx_DebugCrash);
  g_orig.DebugCrash();
}

__declspec(dllexport) char* SYSINFO_Acquire() {
  Timed t(kEx_SysinfoAcquire);
  return g_orig.SysinfoAcquire();
}

__declspec(dllexport) void SYSINFO_Release() {
  Timed t(kEx_SysinfoRelease);
  g_orig.SysinfoRelease();
}

// ConduitTemperatureManager is a separate native subsystem with its own handle
// vector; it is forwarded untouched. A replacement sim has to reimplement these
// too, but they are independent of the cell grid.

__declspec(dllexport) void ConduitTemperatureManager_Initialize() {
  Log("ConduitTemperatureManager_Initialize");
  Timed t(kEx_CtmInitialize);
  g_orig.CtmInitialize();
}

__declspec(dllexport) void ConduitTemperatureManager_Shutdown() {
  Timed t(kEx_CtmShutdown);
  g_orig.CtmShutdown();
}

// Signatures taken verbatim from the [DllImport] declarations in the managed
// ConduitTemperatureManager. Argument *position* selects the register on x64, so
// these must match exactly or forwarded calls receive garbage. The trailing bool
// is a 4-byte Win32 BOOL, which is how P/Invoke marshals `bool` by default.
__declspec(dllexport) int ConduitTemperatureManager_Add(
    float contents_temperature, float contents_mass, int contents_element_hash,
    int conduit_structure_temperature_handle, float conduit_heat_capacity,
    float conduit_thermal_conductivity, int32_t conduit_insulated) {
  if (Spend("conduit")) {
    Note("conduit_add temp=%.4f mass=%.4f elem=%d structhandle=%d heatcap=%.4f"
         " conductivity=%.4f insulated=%d",
         contents_temperature, contents_mass, contents_element_hash,
         conduit_structure_temperature_handle, conduit_heat_capacity,
         conduit_thermal_conductivity, conduit_insulated);
  }
  Timed t(kEx_CtmAdd);
  return g_orig.CtmAdd(contents_temperature, contents_mass, contents_element_hash,
                       conduit_structure_temperature_handle, conduit_heat_capacity,
                       conduit_thermal_conductivity, conduit_insulated);
}

// Conduit contents are a separate world from the cell grid and are completely unmodelled
// in the replacement. These three record its real inputs and outputs so it can be built
// from evidence rather than from the export signatures alone.

__declspec(dllexport) void ConduitTemperatureManager_Remove(int handle) {
  Timed t(kEx_CtmRemove);
  g_orig.CtmRemove(handle);
}

__declspec(dllexport) int ConduitTemperatureManager_Set(int handle,
                                                        float contents_temperature,
                                                        float contents_mass,
                                                        int contents_element_hash) {
  if (Spend("conduit")) {
    Note("conduit_set handle=%d temp=%.4f mass=%.4f elem=%d", handle,
         contents_temperature, contents_mass, contents_element_hash);
  }
  Timed t(kEx_CtmSet);
  return g_orig.CtmSet(handle, contents_temperature, contents_mass,
                       contents_element_hash);
}

__declspec(dllexport) void ConduitTemperatureManager_Clear() {
  Timed t(kEx_CtmClear);
  g_orig.CtmClear();
}

__declspec(dllexport) void* ConduitTemperatureManager_Update(
    float dt, void* building_conductivity_data) {
  Timed t(kEx_CtmUpdate);
  void* r = g_orig.CtmUpdate(dt, building_conductivity_data);
  t.Stop();
  // The return value's layout is not known. Record the pointer and a small hex window so
  // it can be decoded later; the first Update per session is enough to see its shape.
  static int seen = 0;
  if (seen < 4 && r) {
    ++seen;
    const uint8_t* b = static_cast<const uint8_t*>(r);
    char hex[3 * 48 + 1];
    for (int i = 0; i < 48; ++i) sprintf(hex + i * 3, "%02x ", b[i]);
    Note("conduit_update dt=%.4f data=%p ret=%p first48=%s", dt,
         building_conductivity_data, r, hex);
  }
  return r;
}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    InitializeCriticalSection(&g_log_lock);

    // Load the original from this DLL's own directory rather than the search
    // path, so the game's working directory cannot pick up a different copy.
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(module, path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
      char* slash = strrchr(path, '\\');
      if (slash) {
        strcpy(slash + 1, "SimDLL_orig.dll");
        g_original = LoadLibraryA(path);
        // Everything the shim writes goes here; the timing report resolves its own path
        // from this rather than rebuilding it per snapshot.
        *(slash + 1) = '\0';
        const size_t dirlen = strlen(path);
        if (dirlen < sizeof(g_dir)) memcpy(g_dir, path, dirlen + 1);
        strcpy(slash + 1, "sim_shim.log");
        g_log = fopen(path, "w");
        strcpy(slash + 1, "sim_corpus.bin");
        g_corpus = fopen(path, "wb");
        // Appended, not truncated: the notebook is meant to accumulate across the
        // several SIM_Initialize/Shutdown cycles of one session and across sessions,
        // because the rare events it is hunting may only happen once.
        strcpy(slash + 1, "sim_notes.log");
        g_notes = fopen(path, "a");
        if (g_notes) {
          setvbuf(g_notes, nullptr, _IOFBF, 1 << 16);
          fprintf(g_notes, "=== session start ===\n");
        }
      }
    }
    if (!g_original) {
      MessageBoxA(nullptr,
                  "SimDLL_orig.dll not found next to SimDLL.dll.\n"
                  "Rename Klei's original SimDLL.dll to SimDLL_orig.dll.",
                  "SimDLL shim", MB_ICONERROR);
      return FALSE;
    }
    Log("shim attached, original at %p", reinterpret_cast<void*>(g_original));

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpc_freq = freq.QuadPart ? freq.QuadPart : 1;
    // Resolve every export now, once. Doing it here rather than per call is what keeps
    // GetProcAddress out of the interval this build is timing.
    ResolveAll();
    Log("qpc_freq=%lld, %d exports resolved", (long long)g_qpc_freq, (int)kEx_Count);
  } else if (reason == DLL_PROCESS_DETACH) {
    WriteTimingReport("DLL_PROCESS_DETACH");
    if (g_corpus) fclose(g_corpus);
    if (g_log) fclose(g_log);
    DeleteCriticalSection(&g_log_lock);
  }
  return TRUE;
}
