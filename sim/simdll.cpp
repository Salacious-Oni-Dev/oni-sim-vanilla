// The replacement SimDLL.
//
// Sixteen exports and one message queue, matching Klei's C ABI exactly. What is here is
// the *shape* of the sim: boot, message dispatch, the projection contract, save and
// load, plus conduction and flow. What is deliberately not here yet: state changes and
// element interactions, so water heated past 372 K stays water.
//
// That is a real milestone rather than a placeholder: it is what lets the differential
// harness (driver/src/diffsim.cpp) compare us against Klei tick for tick, and it means
// every divergence it reports from now on is a physics divergence rather than an ABI
// one. A world already at equilibrium should match Klei exactly today.
//
// Not installed over the game. Build with sim/build.sh; the harness loads it directly.

// First, and before <windows.h>: it pulls in <winsock2.h>, which must precede the Winsock 1
// declarations <windows.h> would otherwise bring in.
#include "kprofiler.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>

#include "../abi/sim_abi.h"
#include "buildings.h"
#include "cellmod.h"
#include "chunks.h"
#include "disease.h"
#include "conduits.h"
#include "emitters.h"
#include "radiation.h"
#include "physics.h"
#include "projection.h"
#include "saveblob.h"
#include "textures.h"
#include "world.h"

using namespace oni_sim;

namespace {

using GameMessageHandler = int (*)(int, void*);

// ------------------------------------------------------------- the published frame
//
// The game reads the arrays a `GameDataUpdate` names for the whole of its tick, and once the
// sim runs on its own thread it is filling the *next* frame while that read is happening. So
// what the update points at cannot be the sim's own vectors: `Project` and
// `FillPropertyTextures` are incremental and keep theirs across frames on purpose, which is
// exactly what makes them the wrong thing to hand out.
//
// Klei's answer is two whole `SimData` objects, `CopySimDataToGame` filling one and the game
// side swapping the pointers. This is the same answer with one
// allocation instead of forty: every array the update names is copied into `bytes` and the
// update's pointer patched to the copy. Two of these alternate, so the frame the game is
// reading is never the frame the worker is filling.
//
// `bytes` is never shrunk and never re-zeroed — `Begin` only resets the write cursor — so
// after the first frame a publication is one pass of memcpy into a buffer already the right
// size. That pass is the price of threading and it is paid on the worker, off the frame.
struct PublishedFrame {
  std::vector<uint8_t> bytes;
  std::vector<std::pair<void**, size_t>> fixups;
  size_t used = 0;
  GameDataUpdate update{};

  void Begin() {
    used = 0;
    fixups.clear();
    memset(&update, 0, sizeof(update));
  }

  // The pointer cannot be handed out here: a later `Put` may still grow `bytes` and move it.
  // Offsets are recorded instead and resolved once, in `Finish`.
  template <typename T, typename V>
  void Put(T*& slot, const std::vector<V>& src) {
    const size_t off = (used + 7u) & ~static_cast<size_t>(7);
    const size_t n = src.size() * sizeof(V);
    if (off + n > bytes.size()) bytes.resize(off + n);
    if (n != 0) memcpy(bytes.data() + off, src.data(), n);
    used = off + n;
    fixups.emplace_back(reinterpret_cast<void**>(&slot), off);
  }

  void Finish() {
    for (const auto& f : fixups) *f.first = bytes.data() + f.second;
  }
};

struct Sim {
  GameMessageHandler callback = nullptr;
  ElementTable elements;
  DiseaseTable diseases;
  World world;
  ProjectionBuffers buffers;
  ProjectionEvents projection_events;
  PropertyTextureBuffers textures;
  // Buildings live outside the grid and outside the world, so they survive a load the way
  // Klei's do: the save blob has no room for them and the game re-registers every one.
  BuildingState buildings;
  // Element chunks: matter the game holds outside the grid. Same lifetime as buildings —
  // not in the save blob, re-registered by the game after every load.
  ElementChunkState chunks;
  // Element consumers and emitters: the two components that move matter between a building
  // and the grid. First and second in `SimData`'s component list, so they run before chunks.
  ElementFlowState flow;
  // The radiation emitter: third in the component list, and the only owner of the
  // `radiation` array other than the game itself.
  RadiationState radiation;
  // The game's visibility mask, three buffers deep the way Klei's is. `visible` is
  // `SimData::visibleGrid`, what a frame reads: the solid emitter's ore drop and the falling
  // liquid both refuse a cell the player cannot see. `visible_game` is the `visibleGrid` of
  // the two `GameData` buffers `FrameSync` alternates. `PrepareGameData` copies the payload
  // into the game-side one and flips which is which (`GameSyncFunction` writes the
  // game-side buffer, then `FrameSync::GameSync` swaps the pair); the end of every frame
  // swaps `visible` with the sim-side one (`GameData::swapVisibleGrid` at the tail of
  // `CopySimDataToGame`). So a frame sees the mask the game sent two `PrepareGameData`s
  // earlier, and the first frames of a world see a zeroed one and hand nothing over.
  std::vector<uint8_t> visible;
  std::vector<uint8_t> visible_game[2];
  int visible_sim_slot = 0;
  bool debug_editing = false;
  // Conduit contents are not part of the world either, and unlike buildings they are driven
  // from the game thread between frames rather than from the frame itself.
  ConduitTemperatures conduits;

  bool started = false;
  bool first_frame = true;
  int skip_physics_frames = 1;
  float elapsed_seconds = 0.0f;
  // Leftover frame time that did not add up to a whole substep, carried forward.
  float substep_carry = 0.0f;
  // Klei spares one massless cell per region from being zeroed, but only the first time.
  bool first_physics_substep = true;
  // The game's substep counter: a uint16 zeroed at construction and incremented at the very
  // end of every substep. `DisplaceGas` starts
  // its neighbour scan at it, so it decides which of several equally good cells receives the
  // gas a liquid pushes aside. It belongs to the SimData object, not to the world contents:
  // it is *not* reset by InitializeFromCells or by a load, only by an Alloc.
  uint16_t displace_rotation = 0;
  // The game's sweep direction: a signed 1 negated at the top of every substep, so it
  // alternates once per substep. It is the column order the gas
  // pressure sweep walks in *and* the horizontal neighbour each cell pairs with, and it
  // therefore also picks which of the two upward diagonals exists this substep. Same
  // lifetime as `displace_rotation`: SimData state, not world contents.
  int32_t pressure_dir = -1;

  // Per-frame event lists. GameDataUpdate carries count+pointer pairs into these, and
  // the game reads them before the next call, so clearing them at the top of each frame
  // is safe and is what keeps a stale event from being replayed.
  // Transition ores, collected per substep and drained into spawn_ore_info at the end of
  // the frame — a frame can run several substeps and each of them may produce drops.
  std::vector<StateChangeOre> state_change_ores;

  std::vector<SpawnOreInfo> dig_info;
  std::vector<SpawnOreInfo> spawn_ore_info;
  std::vector<MassConsumedCallback> mass_consumed;
  std::vector<DiseaseConsumptionCallback> disease_consumed;
  std::vector<MassEmittedCallback> mass_emitted;
  std::vector<CallbackInfo> callbacks;
  // Unstable solids the post-process sweep handed to the game this frame. Produced by a
  // kernel rather than by the projection, so unlike `substanceChangeInfo` it cannot be
  // recovered by comparing two frames — it has to be collected as it happens.
  std::vector<UnstableCellInfo> unstable_cells;
  // `SimEvents::spawnLiquidInfo`: liquid the flow sweep handed to the game as falling
  // particles this frame, in the order it was handed over. See `StepFlow`.
  std::vector<SpawnFallingLiquidInfo> falling_liquid;
  // `SimEvents::worldDamageInfo`: one record per wall cell an over-full liquid broke this
  // frame, source cell alongside. See `DoPressureBreak`.
  std::vector<WorldDamageInfo> world_damage;
  std::vector<BackwallShouldTransitionInfo> backwall_transitions;
  std::vector<ComponentStateChangedMessage> component_state;

  // Two published frames, alternating: the worker fills one while the game reads the other.
  PublishedFrame pub[2];
  int pub_slot = 0;
  // What the last completed frame published. Written by whichever thread ran that frame and
  // read by the game thread only after it has waited for the worker, so that wait is the
  // synchronisation and there is nothing else to lock.
  GameDataUpdate* last_published = nullptr;
  // False until a frame has been published *into* the pipeline. The first `PrepareGameData`
  // after a Start, an Alloc or a Load runs its frame on the calling thread and primes it;
  // every one after that returns the frame the worker finished during the game's tick.
  bool pipelined = false;
  std::vector<uint8_t> save_blob;

  // Messages arrive between frames and take effect when the frame runs, exactly as they
  // do in Klei's sim. Applying them on receipt would let the game observe a half-stepped
  // world, and it is also how the real sim behaves — a probe that saves before ticking
  // sees none of them.
  struct Pending {
    int32_t id;
    std::vector<uint8_t> payload;
  };
  // Klei's `SimFrameManager::currentFrame`: everything that has arrived since the last
  // `NewGameFrame`. `SimFrameManager::HandleMessage` puts every queued message
  // into it, whatever the message is.
  std::vector<Pending> queue;
  // And the frame that is actually being processed. `NewGameFrame` closes `currentFrame` into
  // `queuedFrames` (`SimFrameManager::NewFrame`) and `BeginFrameProcessing`
  // moves that to `activeFrames` — but the frame the game *observes* through a given
  // `PrepareGameData` is the one before it, because the sim runs on its own thread and the
  // call publishes what has already finished rather than waiting for what it just queued.
  //
  // So a message sent during tick N takes effect on tick N+1, always, and that is a
  // property of the boundary rather than of the harness's waits. Measured directly:
  // `diffsim --scenario chunkenergy --chunks` sends 5000 kJ before tick 3 and Klei's chunk
  // does not move until tick 4, twice over, on both of its energy messages.
  //
  // Nothing had ever tested it. Every scenario in the suite sends its messages before tick 1
  // and `skip_physics_frames` covers that one case by running no substep; the chunk probes
  // are the first thing here to send a message to a component that is already running.
  std::vector<Pending> active;

  int32_t next_handle = 0;
  std::vector<std::pair<int32_t, int32_t>> unknown_messages;  // id -> count
};

Sim* g = nullptr;

// ------------------------------------------------------------------ the sim thread
//
// Klei's sim runs on its own thread and meets the game at a strict
// alternating rendezvous over a double buffer, so a sim frame overlaps the game's *render*
// rather than the game's sim call. Nothing about what gets published changes — the same
// bytes in the same frame order — which is why the test for this is `diffsim` coming back
// unchanged rather than any new golden.
//
// Raw Win32 rather than <thread>: this toolchain is `x86_64-w64-mingw32-g++` with the win32
// thread model, where the standard threading headers are not dependable, and two auto-reset
// events are the whole protocol anyway.
GameDataUpdate* RunFrame();

// The mask a frame reads: `Sim::visible`, zeroed to the grid's size when it is not a full
// mask, because Klei's buffers start zeroed. Never null, so an unseen cell is refused.
const uint8_t* FrameVisible() {
  if (g->visible.size() != g->world.GameCount()) g->visible.assign(g->world.GameCount(), 0);
  return g->visible.data();
}

// A new world: all three visibility buffers start zeroed again.
void ResetVisibility() {
  g->visible.clear();
  g->visible_game[0].clear();
  g->visible_game[1].clear();
  g->visible_sim_slot = 0;
}

struct FrameWorker {
  HANDLE thread = nullptr;
  HANDLE work = nullptr;  // game -> sim: run a frame
  HANDLE done = nullptr;  // sim -> game: the frame is published
  // Game thread only, and true exactly between a `Kick` and the `WaitIdle` that collects it.
  bool busy = false;
  // Set once, before the thread starts, and read by both after that.
  bool enabled = false;
  volatile long quit = 0;
};
FrameWorker g_worker;

DWORD WINAPI WorkerMain(LPVOID) {
  for (;;) {
    WaitForSingleObject(g_worker.work, INFINITE);
    if (g_worker.quit != 0) break;
    RunFrame();
    SetEvent(g_worker.done);
  }
  return 0;
}

// Everything that is not a queued message touches state the worker owns, so it waits here
// first. With no frame in flight this is one predicted branch.
void WaitIdle() {
  if (!g_worker.busy) return;
  WaitForSingleObject(g_worker.done, INFINITE);
  g_worker.busy = false;
}

void Kick() {
  g_worker.busy = true;
  SetEvent(g_worker.work);
}

void StartWorker() {
  g_worker = FrameWorker();
  // An escape hatch for bisecting a divergence: `sim_nothread.on` puts the frame back on the
  // calling thread *and* takes the pipeline out with it, restoring the arrangement every golden
  // in the suite was taken under.
  //
  // IN THE WORKING DIRECTORY. This `fopen` is relative, so it resolves against the cwd of
  // whatever process loaded this DLL — `driver/` for the offline tools, the game's own directory
  // for a live run — and NOT against the directory the DLL sits in. A marker put beside the
  // DLL is simply not found, and the hatch reports nothing, so it looks tried when it has not
  // been. See docs/THREADING.md.
  if (FILE* marker = fopen("sim_nothread.on", "rb")) {
    fclose(marker);
    return;
  }
  g_worker.work = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  g_worker.done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!g_worker.work || !g_worker.done) return;
  g_worker.thread = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr);
  if (!g_worker.thread) return;
  g_worker.enabled = true;
}

void StopWorker() {
  WaitIdle();
  if (g_worker.thread != nullptr) {
    g_worker.quit = 1;
    SetEvent(g_worker.work);
    WaitForSingleObject(g_worker.thread, INFINITE);
    CloseHandle(g_worker.thread);
  }
  if (g_worker.work != nullptr) CloseHandle(g_worker.work);
  if (g_worker.done != nullptr) CloseHandle(g_worker.done);
  g_worker = FrameWorker();
}

void Report(const char* text) {
  if (!g || !g->callback) return;
  // Sent as id 0, `GameHandledMessages.ExceptionHandler`, whose payload is two char*: the
  // game passes the first to its crash reporter as the call stack, and the second, null
  // here, as the dump file name.
  struct {
    const char* message;
    const char* callstack;
  } msg{text, nullptr};
  g->callback(0, &msg);
}

void NoteUnknown(int32_t id) {
  for (auto& p : g->unknown_messages) {
    if (p.first == id) {
      ++p.second;
      return;
    }
  }
  g->unknown_messages.emplace_back(id, 1);
}

// ------------------------------------------------------------------ live profiler
//
// `ToggleProfiler` is a message *into* the sim — the game sends it from the backtick key
// (`DebugHandler`, `Sim.SIM_HandleMessage(-409964931, 0, null)`), and Klei answers it with a
// `kprofiler` statically linked into their DLL. This answers it with per-kernel timings,
// because there is otherwise no way to see where a frame goes in a colony somebody actually
// built: `bench` runs offline, on scenarios somebody wrote, and does not call `StepPhysics`
// at all. These are the only numbers this project can take from a real world.
//
// Off costs one predicted branch per kernel call and nothing else — no timer is read. State
// lives outside `Sim` on purpose, so a toggle survives a load, an Alloc and a Shutdown; a
// profiling run that ended when the player reloaded would be useless.
struct Profiler {
  enum Slot {
    kDrainQueue,
    kConduction,
    kStateChange,
    kGasPressure,
    kGasDisplacement,
    kFlow,
    kPostProcess,
    kDisease,
    kZeroMassless,
    kElementFlow,
    kRadiation,
    kElementChunk,
    kBuildingHeat,
    kBuildingToBuilding,
    kProject,
    kTextures,
    kFrame,
    kSlotCount
  };

  static const char* Name(int s) {
    switch (s) {
      case kDrainQueue: return "DrainQueue";
      case kConduction: return "StepConduction";
      case kStateChange: return "StepStateChange";
      case kGasPressure: return "StepGasPressure";
      case kGasDisplacement: return "StepGasDisplacement";
      case kFlow: return "StepFlow";
      case kPostProcess: return "StepPostProcess";
      case kDisease: return "StepDisease";
      case kZeroMassless: return "ZeroMasslessCells";
      case kElementFlow: return "StepElementFlow";
      case kRadiation: return "StepRadiationEmitters";
      case kElementChunk: return "StepElementChunks";
      case kBuildingHeat: return "StepBuildingHeatExchange";
      case kBuildingToBuilding: return "StepBuildingToBuilding";
      case kProject: return "Project";
      case kTextures: return "FillPropertyTextures";
      case kFrame: return "whole frame";
      default: return "?";
    }
  }

  bool on = false;
  int64_t freq = 0;
  double ms[kSlotCount] = {};
  int64_t calls[kSlotCount] = {};
  int frames = 0;

  // A scope, not a start/stop pair: several of the kernels below sit inside a region loop
  // that an early return could leave.
  struct Scope {
    int slot;
    int64_t t0;
    bool kprofiler;  // a kprofiler section was opened, so one must be closed
    explicit Scope(int s);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
  };
};

Profiler g_prof;

// The same kernel scopes feed Klei's kprofiler (`sim/kprofiler.h`) while a capture is running,
// so a trace taken from the game shows the sim worker's kernels nested in its frame. Names are
// recorded once; the worker names itself the first time it records under a session.
void KprofilerBeginKernel(int slot) {
  static uint64_t ids[Profiler::kSlotCount];
  static uint64_t category = 0;
  static std::atomic<bool> named{false};
  if (!named.load(std::memory_order_relaxed)) {
    for (int i = 0; i < Profiler::kSlotCount; ++i) {
      ids[i] = kprof::G().strings.Record(Profiler::Name(i));
    }
    category = kprof::G().strings.Record("SimDLL");
    named.store(true);
  }
  static thread_local int named_thread_session = -1;
  const int session = kprof::G().session.load(std::memory_order_relaxed);
  if (named_thread_session != session) {
    named_thread_session = session;
    kprof::SetThreadInfo(0, kprof::G().strings.Record("SimDLL frame worker"), category);
  }
  kprof::BeginSection(ids[slot], category, -1);
}

Profiler::Scope::Scope(int s) : slot(g_prof.on ? s : -1), t0(0), kprofiler(kprof::Running()) {
  if (kprofiler) KprofilerBeginKernel(s);
  if (slot < 0) return;
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  t0 = t.QuadPart;
}

Profiler::Scope::~Scope() {
  if (kprofiler) kprof::EndSection(-1);
  if (slot < 0) return;
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  g_prof.ms[slot] += static_cast<double>(t.QuadPart - t0) * 1000.0 /
                     static_cast<double>(g_prof.freq);
  ++g_prof.calls[slot];
}

#define PROF(slot) Profiler::Scope prof_scope_##slot(Profiler::slot)

// Both halves on purpose. `Report` goes through the game's own message handler and lands in
// `Player.log`, which is the only channel out of a sim DLL that needs no file at all — but a
// log the player has to scroll is a bad place to compare two builds, so the same lines are
// appended to `sim_profile.log` in the process working directory (the game root, where
// `PerformanceCaptureData.json` lands too). A failed fopen is silent: an unwritable install
// must not cost the caller its timings.
void EmitProfile() {
  FILE* f = fopen("sim_profile.log", "a");
  char line[192];

  auto emit = [&](const char* text) {
    Report(text);
    if (f) fprintf(f, "%s\n", text);
  };

  const double frames = g_prof.frames > 0 ? static_cast<double>(g_prof.frames) : 1.0;
  snprintf(line, sizeof(line), "sim profiler: %d frames, %.3f ms/frame",
           g_prof.frames, g_prof.ms[Profiler::kFrame] / frames);
  emit(line);

  for (int s = 0; s < Profiler::kSlotCount; ++s) {
    if (g_prof.calls[s] == 0) continue;
    if (s == Profiler::kFrame) continue;
    // Per *frame*, not per call: a kernel runs once per region per substep, so its call
    // count is the wrong denominator for anything a 16.67 ms budget is compared against.
    snprintf(line, sizeof(line), "  %-26s %8.3f ms/frame  %8.4f ms/call  (%lld calls)",
             Profiler::Name(s), g_prof.ms[s] / frames,
             g_prof.ms[s] / static_cast<double>(g_prof.calls[s]),
             static_cast<long long>(g_prof.calls[s]));
    emit(line);
  }
  if (f) {
    fprintf(f, "\n");  // one blank line per run: the file is appended to, not replaced
    fclose(f);
  }
}

// The message carries no payload, so there is no "start" and "stop" to distinguish: the
// first press arms and zeroes, the second reports and disarms. Zeroing on arm rather than on
// disarm is what makes a second run comparable to the first.
void ToggleProfiler() {
  if (g_prof.on) {
    g_prof.on = false;
    EmitProfile();
    return;
  }
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_prof.freq = f.QuadPart ? f.QuadPart : 1;
  for (int s = 0; s < Profiler::kSlotCount; ++s) {
    g_prof.ms[s] = 0.0;
    g_prof.calls[s] = 0;
  }
  g_prof.frames = 0;
  g_prof.on = true;
  Report("sim profiler: on (press again to report)");
}

// ------------------------------------------------------------------ queued messages

template <typename T>
bool Payload(const Sim::Pending& p, T* out) {
  if (p.payload.size() < sizeof(T)) return false;
  memcpy(out, p.payload.data(), sizeof(T));
  return true;
}

void ApplyModifyCell(const Sim::Pending& p) {
  ModifyCellMessage m{};
  if (!Payload(p, &m) || !g->world.ValidGameCell(m.cellIdx)) return;
  const size_t cell = g->world.Padded(static_cast<size_t>(m.cellIdx));
  // The projection walks rectangles now, and a message can write a cell no kernel drove.
  // The paths below mark the further cells they reach; this covers the named one, which
  // some of them leave untouched.
  g->world.MarkProjectDirty(cell);

  const CellModContext cm{&g->world, &g->elements, &g->diseases, g->displace_rotation};

  // The clamp. It is a *conditional* clamp — the values are only touched when
  // the message is already out of range, so an in-range negative-mass message keeps its
  // temperature. Klei reports it to the crash handler on the way through and carries on.
  float temperature = m.temperature;
  float mass = m.mass;
  if ((temperature <= 0.0f && mass > 0.0f) || temperature > kCellModMaxTemperature) {
    if (temperature >= kCellModMaxTemperature) temperature = kCellModMaxTemperature;
    if (temperature <= 0.0f) temperature = 0.0f;
    if (mass <= 0.0f) {
      mass = 0.0f;
      temperature = 0.0f;
    }
  }

  switch (m.replaceType) {
    case kReplaceNone: {
      if (m.elementIdx >= static_cast<uint16_t>(g->elements.Count())) break;
      // Dispatched on the phase of the element in the **message**, not the one in the cell.
      switch (g->elements.Phase(m.elementIdx)) {
        case kStateGas:
          if (mass > 0.0f) {
            AddGas(cm, cell, m.elementIdx, mass, temperature, m.diseaseIdx, m.diseaseCount);
          } else {
            RemoveFromCell(cm, cell, kStateGas, mass);
          }
          break;
        case kStateLiquid:
          if (mass > 0.0f) {
            AddLiquid(cm, cell, m.elementIdx, mass, temperature, m.diseaseIdx, m.diseaseCount);
          } else if (mass < 0.0f) {
            RemoveFromCell(cm, cell, kStateLiquid, mass);
          }
          break;
        case kStateSolid:
          if (mass > 0.0f) {
            AddSolid(cm, cell, m.elementIdx, mass, temperature, m.diseaseIdx, m.diseaseCount,
                     m.addSubType);
          } else if (mass < 0.0f) {
            RemoveFromCell(cm, cell, kStateSolid, mass);
          }
          break;
        default:
          // Vacuum. reports "Invalid replacement type" and does nothing else,
          // so a message that adds Vacuum to a cell is silently dropped.
          break;
      }
      break;
    }
    case kReplaceElement:
      ReplaceElementAt(cm, cell, m.elementIdx, mass, temperature, m.diseaseIdx,
                       m.diseaseCount);
      break;
    case kReplaceAndDisplace:
      ReplaceAndDisplaceElementAt(cm, cell, m.elementIdx, mass, temperature, m.diseaseIdx,
                                  m.diseaseCount);
      break;
    default:
      break;
  }

  // `!= -1`, not `>= 0`:. Every other negative index is pushed and handed back
  // to the game, which is the sort of thing that only matters once.
  if (m.callbackIdx != -1) g->callbacks.push_back(CallbackInfo{m.callbackIdx});
}

void ApplyDig(const Sim::Pending& p) {
  DigMessage m{};
  if (!Payload(p, &m) || !g->world.ValidGameCell(m.cellIdx)) return;
  const size_t cell = g->world.Padded(static_cast<size_t>(m.cellIdx));
  // The projection walks rectangles now, and a message can write a cell no kernel drove.
  g->world.MarkProjectDirty(cell);
  PhaseEntry& e = g->world.Phase(cell);
  if (!g->elements.IsSolid(e.element) || e.mass <= 0.0f) return;
  if (g->world.Properties()[cell] & kUnbreakable) return;

  // Digging hands the cell's mass to the game as ore and empties the cell. The measured
  // behaviour of Klei's sim is that this is exact: 48000 kg dug produced 48000 kg of
  // ore, so anything less here is a bug, not a design choice.
  SpawnOreInfo ore{};
  ore.cellIdx = m.cellIdx;
  ore.elemIdx = e.element;
  ore.mass = e.mass;
  ore.temperature = e.temperature;
  ore.diseaseIdx = g->world.DiseaseIdx()[cell];
  ore.diseaseCount = g->world.Disease()[cell].count;
  if (!m.skipEvent) g->dig_info.push_back(ore);

  // Charged whether or not the event was skipped: `skipEvent` withholds the announcement,
  // not the mass, and the cell is emptied either way.
  g->world.NoteDug(e.mass);
  e = PhaseEntry{};
  g->world.MutableDisease(cell) = SaveDisease{};
  g->world.MutableDiseaseIdx(cell) = 0xFF;
  if (m.callbackIdx >= 0) g->callbacks.push_back(CallbackInfo{m.callbackIdx});
}

// `ProcessMassEmission`. One building putting a fixed bite of matter into one
// cell, with no component and no flood — the electrolyzer and everything like it.
//
// This used to be a twenty-line approximation that refused anything landing on a different
// element and reported the message's own numbers back on the refusal. Four things were wrong
// with that, and the first is the one that matters in play:
//
//   * **Klei displaces what is in the way.** A gas emission into a cell holding a different
//     gas calls `DisplaceGas`, a liquid emission calls `DisplaceLiquid`, and the emission
//     goes ahead if either succeeds. Only a solid in the way, or a displacement with nowhere
//     to go, is a refusal.
//   * A refusal reports **nothing** — element `0xffff`, zero mass, zero temperature — rather
//     than an echo of the request. The `emitted` flag is the only field that carries meaning.
//   * The disease on the message is added to the cell, through the same merge every other
//     germ writer uses.
//   * The temperature mix is `CalculateCombinedTemperature`, so it is clamped to the two
//     inputs; the old open mix could overshoot on a large mass ratio.
//
// And the callback test is `!= -1`, not `>= 0`, as in `ModifyCell`.
void ApplyMassEmission(const Sim::Pending& p) {
  MassEmissionMessage m{};
  if (!Payload(p, &m) || !g->world.ValidGameCell(m.cellIdx)) return;
  const size_t cell = g->world.Padded(static_cast<size_t>(m.cellIdx));
  // The projection walks rectangles now, and a message can write a cell no kernel drove.
  g->world.MarkProjectDirty(cell);
  const CellModContext cm{&g->world, &g->elements, &g->diseases, g->displace_rotation};

  PhaseEntry& e = g->world.Phase(cell);
  const uint16_t vacuum = g->elements.VacuumIndex();
  const uint16_t was = e.element;
  bool ok = was == m.elementIdx || was == vacuum;
  if (!ok) {
    const uint8_t phase = g->elements.Phase(m.elementIdx);
    if (phase == kStateGas) ok = DisplaceGasFromDrain(cm, cell, was);
    else if (phase == kStateLiquid) ok = DisplaceLiquid(cm, cell, was);
  }

  if (!ok) {
    if (m.callbackIdx != -1) {
      MassEmittedCallback cb{};
      cb.callbackIdx = m.callbackIdx;
      cb.elemIdx = 0xFFFF;
      cb.suceeded = 0;
      cb.diseaseIdx = 0xFF;
      cb.mass = 0.0f;
      cb.temperature = 0.0f;
      cb.diseaseCount = 0;
      g->mass_emitted.push_back(cb);
    }
    return;
  }

  // Re-read: a displacement moved the cell's contents out from under the reference.
  PhaseEntry& c = g->world.Phase(cell);
  const uint16_t before_element = c.element;
  const float before = c.mass;
  c.temperature = CalculateCombinedTemperature(c.mass, c.temperature, m.mass, m.temperature);
  c.mass = before + m.mass;
  g->world.NoteEmitted(c.mass - before);
  if (m.diseaseIdx != 0xFF) {
    AddDiseaseToCell(&g->world, g->diseases, cell, m.diseaseIdx, m.diseaseCount);
  }
  // The element is written and announced only when it actually changes. Klei checks against
  // the element the cell held *before* the displacement, not after.
  if (before_element != m.elementIdx) {
    c.element = m.elementIdx;
    g->world.TouchSubstance(cell);
  }

  if (m.callbackIdx != -1) {
    MassEmittedCallback cb{};
    cb.callbackIdx = m.callbackIdx;
    cb.elemIdx = m.elementIdx;
    cb.suceeded = 1;
    cb.diseaseIdx = m.diseaseIdx;
    cb.mass = m.mass;
    cb.temperature = m.temperature;
    cb.diseaseCount = m.diseaseCount;
    g->mass_emitted.push_back(cb);
  }
}

// `ProcessMassConsumption`. The other half of the same pair, and it is not a
// single-cell operation at all: the message carries a radius and a height, and Klei runs the
// **same drain the element consumer runs** over either a flood or a rectangle.
//
//   * `height == 0` floods outward `radius` steps, through solids if the element is a solid
//     and through everything else if it is not.
//   * `height != 0` walks a `radius x height` rectangle whose top-left corner is the cell.
//
// So the old handler — take from one cell, report the cell's temperature — was wrong about
// the extent, about the disease it hands back, and about the temperature, which is the
// mass-weighted mix of every cell it drained rather than any one cell's.
void ApplyMassConsumption(const Sim::Pending& p) {
  MassConsumptionMessage m{};
  if (!Payload(p, &m) || !g->world.ValidGameCell(m.cellIdx)) return;
  const size_t cell = g->world.Padded(static_cast<size_t>(m.cellIdx));
  const int32_t pw = g->world.PaddedWidth();
  const int32_t x = static_cast<int32_t>(cell % static_cast<size_t>(pw));
  const int32_t y = static_cast<int32_t>(cell / static_cast<size_t>(pw));

  ConsumedMassInfo info{};
  info.simHandle = -1;
  info.removedElemIdx = m.elementIdx;
  info.diseaseIdx = 0xFF;
  info.mass = 0.0f;
  info.temperature = 0.0f;
  info.diseaseCount = 0;

  float remaining = m.mass;
  if (m.height == 0) {
    FloodRemoved(&g->world, g->elements, g->diseases, &remaining, m.elementIdx, &info, x, y,
                 m.radius, &g->flow.scratch);
  } else {
    RectangularRemoved(&g->world, g->elements, g->diseases, &remaining, m.elementIdx, &info,
                       x, y, m.radius, m.height);
  }
  g->world.NoteConsumed(info.mass);

  if (m.callbackIdx != -1) {
    MassConsumedCallback cb{};
    cb.callbackIdx = m.callbackIdx;
    cb.elemIdx = info.removedElemIdx;
    cb.diseaseIdx = info.diseaseIdx;
    cb.mass = info.mass;
    cb.temperature = info.temperature;
    cb.diseaseCount = info.diseaseCount;
    g->mass_consumed.push_back(cb);
  }
}

// The two byte-array writes `SimFrameManager::ProcessFrame` does before anything else,
// (insulation) and (strength). Both are `(char)(int)v`:
// truncation toward zero into the low byte, with **no rounding and no clamp** — and only
// the insulation one scales by 255. Strength is stored exactly as it arrives.
//
// This used to round (`* 255.0f + 0.5f`), clamp to [0, 1], and scale strength by 255 as
// well, all three of which are wrong, and a green suite said nothing about any of it
// because **no scenario had ever sent either message** — the insulation coverage the suite
// does have arrives in the world payload instead. Exactly the hole `Cell.properties` sat in.
//
// The strength half is still unobservable: the byte is projected but nothing in
// `GameDataUpdate` publishes it and no kernel of ours reads it, so `setinsul` scores the
// insulation half only and the strength half is untested.
void ApplyCellFloat(const Sim::Pending& p, bool insulation) {
  SetCellFloatValueMessage m{};
  if (!Payload(p, &m) || !g->world.ValidGameCell(m.cellIdx)) return;
  const size_t cell = g->world.Padded(static_cast<size_t>(m.cellIdx));
  const float scaled = insulation ? m.value * 255.0f : m.value;
  const uint8_t byte = static_cast<uint8_t>(static_cast<int32_t>(scaled));
  if (insulation) g->world.MutableInsulation(cell) = byte;
  else g->world.MutableStrength(cell) = byte;
}

// `ProcessCellEnergyModifications`. The third busiest message in the live
// census and the only one of the top five that is not a component: it is how everything
// game-side that heats or cools a cell without moving mass reaches the grid — a running
// machine's waste heat, a duplicant's body heat, a radiant pipe.
//
// Four gates, and the two in the middle are the ones a guess gets wrong:
//
//   * the cell's element must have a **phase**, `state & 3`. A Vacuum cell is refused
//     before its mass is even read, so energy poured into empty space is discarded rather
//     than accumulated.
//   * `mass > 0.001f`, strictly, against the same 1 g floor the rest of the sim uses.
//   * `maxTemperature > 0.0f`. A message with no ceiling does **nothing at all** — the
//     ceiling is not optional and zero is not "unlimited".
//
// The ceiling itself is `max(temperature, maxTemperature)`, not `maxTemperature`: a cell
// already hotter than the ceiling is not cooled back down to it, it is simply left where
// it is. So the message can only ever push a cell *towards* the ceiling from below, and
// negative kilojoules cool without limit.
//
// Klei then clamps a result outside `(0, 10000]` into `[1, 10000]` and shouts on stderr,
// and skips the write entirely if the result is not positive.
void ApplyCellEnergy(const Sim::Pending& p) {
  ModifyCellEnergyMessage m{};
  if (!Payload(p, &m) || !g->world.ValidGameCell(m.cellIdx)) return;
  const size_t cell = g->world.Padded(static_cast<size_t>(m.cellIdx));
  PhaseEntry& c = g->world.Phase(cell);
  const Element& e = g->elements.At(c.element);
  if ((e.state & kStateMask) == 0) return;
  const float mass = c.mass;
  if (!(mass > 0.001f) || !(m.maxTemperature > 0.0f)) return;

  const float t = c.temperature;
  float next = t;
  if (t <= m.maxTemperature) next = m.maxTemperature;
  const float raised = t + m.kilojoules / (mass * e.specificHeatCapacity);
  if (raised <= next) next = raised;
  if (next <= 0.0f || next > kMaxTemperature) {
    if (kMaxTemperature <= next) next = kMaxTemperature;
    if (next <= 1.0f) next = 1.0f;
  }
  if (!(next > 0.0f)) return;

  c.temperature = next;
  // A message can write a cell no kernel drove, and `Project` walks rectangles.
  g->world.MarkProjectDirty(cell);
  // Klei calls `DoStateTransition` on the cell here, in the drain, before any kernel of
  // this frame has looked at it — so a cell this message boils is already steam when the
  // first substep runs.
  const int32_t pw = g->world.PaddedWidth();
  const int32_t y = static_cast<int32_t>(cell / static_cast<size_t>(pw));
  const int32_t x = static_cast<int32_t>(cell - static_cast<size_t>(y) * pw);
  if (TransitionCell(&g->world, g->elements, x, y, &g->state_change_ores, FrameVisible(),
                     g->debug_editing, &g->falling_liquid)) {
    g->world.TouchSubstance(cell);
  }
}

void ApplyCellProperties(const Sim::Pending& p) {
  CellPropertiesMessage m{};
  if (!Payload(p, &m) || !g->world.ValidGameCell(m.cellIdx)) return;
  const size_t cell = g->world.Padded(static_cast<size_t>(m.cellIdx));
  if (m.set) g->world.MutableProperties(cell) |= m.properties;
  else g->world.MutableProperties(cell) &= static_cast<uint8_t>(~m.properties);
  if (m.callbackIdx >= 0) g->callbacks.push_back(CallbackInfo{m.callbackIdx});
}

void ApplyDiseaseModification(const Sim::Pending& p) {
  CellDiseaseModification m{};
  if (!Payload(p, &m) || !g->world.ValidGameCell(m.cellIdx)) return;
  const size_t cell = g->world.Padded(static_cast<size_t>(m.cellIdx));
  if (m.diseaseIdx != 0xFF) {
    g->world.MutableDiseaseIdx(cell) = m.diseaseIdx;
    g->world.MutableDisease(cell).diseaseHash = g->diseases.HashOf(m.diseaseIdx);
  }
  int64_t count = g->world.Disease()[cell].count + static_cast<int64_t>(m.diseaseCount);
  if (count < 0) count = 0;
  g->world.MutableDisease(cell).count = static_cast<int32_t>(count);
  if (count == 0) {
    g->world.MutableDiseaseIdx(cell) = 0xFF;
    g->world.MutableDisease(cell).diseaseHash = 0;
  }
}

// `ProcessConsumeDisease`. The disease *consumer*, and it is not a component
// update at all — `DiseaseConsumer` has only `Register` and `Unregister`, and no entry in
// `SimData`'s component list. The consuming is done by a per-frame message, drained by
// `ProcessFrame` immediately **before** the inline disease-modification loop.
//
// Per record: take `percentToConsume` of the cell's germs, rounded with a **+0.5f** before
// the truncation, capped at `maxToConsume`; subtract it; and if that leaves the cell below
// one germ, clear all four disease fields the way `ClearCell` does.
//
// The callback carries the disease index the cell has **afterwards** — 0xFF when the cell was
// empty to begin with, and 0xFF again when this call emptied it — together with the amount
// actually taken, which is 0 in both of those cases.
void ApplyConsumeDisease(const Sim::Pending& p) {
  ConsumeDiseaseMessage m{};
  if (!Payload(p, &m) || !g->world.ValidGameCell(m.gameCell)) return;
  const size_t cell = g->world.Padded(static_cast<size_t>(m.gameCell));
  uint8_t reported_idx = 0xFF;
  int32_t taken = 0;
  if (g->world.DiseaseIdx(cell) != 0xFF) {
    const int32_t count = g->world.Disease()[cell].count;
    taken = static_cast<int32_t>(static_cast<float>(count) * m.percentToConsume + 0.5f);
    if (taken > m.maxToConsume) taken = m.maxToConsume;
    g->world.MutableDisease(cell).count = count - taken;
    if (g->world.Disease()[cell].count < 1) {
      g->world.MutableDiseaseIdx(cell) = 0xFF;
      g->world.MutableDisease(cell).count = 0;
      g->world.MutableDisease(cell).diseaseHash = 0;
      g->world.MutableDiseaseInfest(cell) = 0;
      g->world.MutableDiseaseAccum(cell) = 0.0f;
    }
    reported_idx = g->world.DiseaseIdx(cell);
  }
  if (m.callbackIdx != -1) {
    DiseaseConsumptionCallback cb{};
    cb.callbackIdx = m.callbackIdx;
    cb.diseaseIdx = reported_idx;
    cb.diseaseCount = taken;
    g->disease_consumed.push_back(cb);
  }
}

// `ProcessCellRadiationChanges`, first half.
//
// The callback does not go into `callbackInfo` — it goes into its own array,
// `radiationConsumedCallbacks` at `SimEvents + 0x180`, and it carries the cell and an
// amount as well as the index. The amount is **the delta that was applied**, except when
// the delta drove the cell to zero or below, in which case it is the rads the cell *had*:
// a consumer that asks for more than is there is told how much it actually got.
void ApplyRadiationModification(const Sim::Pending& p) {
  CellRadiationModification m{};
  if (!Payload(p, &m) || !g->world.ValidGameCell(m.cellIdx)) return;
  const size_t cell = g->world.Padded(static_cast<size_t>(m.cellIdx));
  const float before = g->world.Radiation()[cell];
  float reported = m.radiationDelta;
  float v = before + reported;
  if (v <= 0.0f) {
    v = 0.0f;
    reported = before;
  }
  g->world.MutableRadiation(cell) = v;
  if (m.callbackIdx != -1) {
    g->radiation.consumed.push_back(
        ConsumedRadiationCallback{m.callbackIdx, m.cellIdx, reported});
  }
}

// `ProcessCellRadiationChanges`, second half. Five of the six type codes land somewhere;
// type 1 is not in Klei's chain at all and is silently dropped.
void ApplyRadiationParams(const Sim::Pending& p) {
  RadiationParamsModification m{};
  if (!Payload(p, &m)) return;
  g->world.SetRadiationParam(m.RadiationParamsType, m.value);
}

void ApplyBackwall(const Sim::Pending& p) {
  SetBackwallDataMsg m{};
  if (!Payload(p, &m) || !g->world.ValidGameCell(m.gameCell)) return;
  const size_t cell = g->world.Padded(static_cast<size_t>(m.gameCell));
  g->world.MutableBackwall(cell) =
      {g->elements.BackwallHash(m.elementIdx), m.mass, m.temperature};
}

// Component messages are drained **by type**, in the order `SimFrameManager::ProcessFrame`
// runs them, not in arrival order: every Add, then every Modify and energy
// change, then every Remove. The game registers a building and then immediately modifies it,
// so arrival order and type order genuinely differ on the frame a building is placed.
//
// Every Add reports the handle it allocated back through `componentStateChangedMessages`,
// which is the only way the game learns it — `SIM_HandleMessage` returns null for these.
void DrainBuildingQueue() {
  auto each = [](SimMessageHash id, void (*fn)(const Sim::Pending&)) {
    for (const Sim::Pending& p : g->active) {
      if (p.id == static_cast<int32_t>(id)) fn(p);
    }
  };

  each(SimMessageHash::SetDebugProperties, [](const Sim::Pending& p) {
    DebugProperties m{};
    if (!Payload(p, &m)) return;
    g->buildings.temperature_scale = m.buildingTemperatureScale;
    g->buildings.to_building_temperature_scale = m.buildingToBuildingTemperatureScale;
    // The third field. Nothing read it until the solid element emitter needed it: it is
    // what lets the sandbox tools drop ore into a cell the player has not uncovered.
    g->debug_editing = m.isDebugEditing != 0;
  });

  each(SimMessageHash::AddBuildingHeatExchange, [](const Sim::Pending& p) {
    AddBuildingHeatExchangeMessage m{};
    if (!Payload(p, &m)) return;
    const int32_t handle = g->buildings.exchange.Add(BuildingDataFromMessage(
        g->elements, m.elemIdx, m.mass, m.temperature, m.thermalConductivity,
        m.overheatTemperature, m.operatingKilowatts, m.minX, m.minY, m.maxX, m.maxY));
    if (m.callbackIdx >= 0) {
      g->component_state.push_back(ComponentStateChangedMessage{m.callbackIdx, handle});
    }
  });

  each(SimMessageHash::ModifyBuildingHeatExchange, [](const Sim::Pending& p) {
    ModifyBuildingHeatExchangeMessage m{};
    // `callbackIdx` carries the handle on this message rather than a callback. The field is
    // reused, not misnamed: `SimMessages.ModifyBuildingHeatExchange` writes `sim_handle`
    // into it.
    if (!Payload(p, &m)) return;
    ModifyBuilding(&g->buildings, m.callbackIdx,
                   BuildingDataFromMessage(g->elements, m.elemIdx, m.mass, m.temperature,
                                           m.thermalConductivity, m.overheatTemperature,
                                           m.operatingKilowatts, m.minX, m.minY, m.maxX,
                                           m.maxY));
  });

  each(SimMessageHash::ModifyBuildingEnergy, [](const Sim::Pending& p) {
    ModifyBuildingEnergyMessage m{};
    if (!Payload(p, &m)) return;
    if (BuildingHeatExchangeData* d = g->buildings.exchange.Get(m.handle)) {
      ApplyBuildingEnergy(d, m.deltaKJ, m.minTemperature, m.maxTemperature);
    }
  });

  each(SimMessageHash::RemoveBuildingHeatExchange, [](const Sim::Pending& p) {
    RemoveBuildingHeatExchangeMessage m{};
    if (!Payload(p, &m)) return;
    g->buildings.exchange.Remove(m.handle);
    if (m.callbackIdx >= 0) g->callbacks.push_back(CallbackInfo{m.callbackIdx});
  });

  // The two building-to-building hashes are crossed over relative to their names, and this
  // is not a transcription slip: `SimMessages.RegisterBuildingToBuildingHeatExchange` sends
  // `AddBuildingToBuildingHeatExchange`, and `SimMessages.AddBuildingToBuildingHeatExchange`
  // sends `AddInContactBuildingToBuildingToBuildingHeatExchange`. Register creates the
  // contact group and hands back its own handle; Add puts a neighbour into an existing one.
  each(SimMessageHash::AddBuildingToBuildingHeatExchange, [](const Sim::Pending& p) {
    RegisterBuildingToBuildingHeatExchangeMessage m{};
    if (!Payload(p, &m)) return;
    if (!g->buildings.exchange.Valid(m.structureTemperatureHandler)) return;
    BuildingToBuildingData d;
    d.self = m.structureTemperatureHandler;
    const int32_t handle = g->buildings.contact.Add(d);
    if (m.callbackIdx >= 0) {
      g->component_state.push_back(ComponentStateChangedMessage{m.callbackIdx, handle});
    }
  });

  each(SimMessageHash::RemoveBuildingInContactFromBuildingToBuildingHeatExchange,
       [](const Sim::Pending& p) {
         RemoveBuildingInContactFromBuildingToBuildingHeatExchangeMessage m{};
         if (!Payload(p, &m)) return;
         BuildingToBuildingData* d = g->buildings.contact.Get(m.selfHandler);
         if (!d) return;
         for (size_t i = 0; i < d->contacts.size(); ++i) {
           if (d->contacts[i].handle != m.buildingNoLongerInContactHandler) continue;
           d->contacts.erase(d->contacts.begin() + static_cast<ptrdiff_t>(i));
           break;
         }
       });

  each(SimMessageHash::AddInContactBuildingToBuildingToBuildingHeatExchange,
       [](const Sim::Pending& p) {
         AddBuildingToBuildingHeatExchangeMessage m{};
         if (!Payload(p, &m)) return;
         BuildingToBuildingData* d = g->buildings.contact.Get(m.selfHandler);
         if (!d) return;
         d->contacts.push_back(
             InContactBuilding{m.buildingInContactHandle, m.cellsInContact});
       });

  each(SimMessageHash::RemoveBuildingToBuildingHeatExchange, [](const Sim::Pending& p) {
    RemoveBuildingToBuildingHeatExchangeMessage m{};
    if (!Payload(p, &m)) return;
    g->buildings.contact.Remove(m.selfHandler);
    if (m.callbackIdx >= 0) g->callbacks.push_back(CallbackInfo{m.callbackIdx});
  });
}

// The chunk half of `SimFrameManager::ProcessFrame` plus the whole of
// `ProcessElementChunkMessages`. Same by-type drain the buildings get, and the
// order matters for the same reason: the game registers a chunk and modifies it in the same
// frame.
//
//   Add -> Move -> SetData -> Energy -> Adjuster -> Remove
//
// Note where `Move` sits. A chunk registered and moved in one frame lands where the move
// says; a chunk moved and then removed never exchanges from the new cell at all.
void DrainChunkQueue() {
  auto each = [](SimMessageHash id, void (*fn)(const Sim::Pending&)) {
    for (const Sim::Pending& p : g->active) {
      if (p.id == static_cast<int32_t>(id)) fn(p);
    }
  };

  each(SimMessageHash::AddElementChunk, [](const Sim::Pending& p) {
    AddElementChunkMessage m{};
    if (!Payload(p, &m)) return;
    const int32_t handle =
        g->chunks.chunks.Add(ChunkDataFromMessage(g->elements, g->world, m));
    if (m.callbackIdx != -1) {
      g->component_state.push_back(ComponentStateChangedMessage{m.callbackIdx, handle});
    }
  });

  each(SimMessageHash::MoveElementChunk, [](const Sim::Pending& p) {
    MoveElementChunkMessage m{};
    if (!Payload(p, &m)) return;
    MoveChunk(&g->chunks, g->world, m);
  });

  each(SimMessageHash::SetElementChunkData, [](const Sim::Pending& p) {
    SetElementChunkDataMessage m{};
    if (!Payload(p, &m)) return;
    ModifyChunk(&g->chunks, m);
  });

  each(SimMessageHash::ModifyElementChunkEnergy, [](const Sim::Pending& p) {
    ModifyElementChunkEnergyMessage m{};
    if (!Payload(p, &m)) return;
    ModifyChunkEnergy(&g->chunks, m);
  });

  each(SimMessageHash::ModifyChunkTemperatureAdjuster, [](const Sim::Pending& p) {
    ModifyElementChunkAdjusterMessage m{};
    if (!Payload(p, &m)) return;
    ModifyChunkAdjuster(&g->chunks, m);
  });

  // A removal reports `-1` back through the same channel an add reports its handle on, which
  // is how the game learns the handle it was holding is now dead.
  each(SimMessageHash::RemoveElementChunk, [](const Sim::Pending& p) {
    RemoveElementChunkMessage m{};
    if (!Payload(p, &m)) return;
    g->chunks.chunks.Remove(m.handle);
    if (m.callbackIdx != -1) {
      g->component_state.push_back(ComponentStateChangedMessage{m.callbackIdx, -1});
    }
  });
}

// The element consumer and the element emitter, in the order `SimFrameManager::ProcessFrame`
// walks them: both come after the chunk block, and within each component it is
// register, then modify, then unregister.
//
// `SetElementConsumerData` is Klei's `ModifyElementConsumerMessage` under a different name —
// the hash the game sends is `SetElementConsumerData` and the struct it carries is the modify
// payload, handle plus cell plus rate.
void DrainElementFlowQueue() {
  auto each = [](SimMessageHash id, void (*fn)(const Sim::Pending&)) {
    for (const Sim::Pending& p : g->active) {
      if (p.id == static_cast<int32_t>(id)) fn(p);
    }
  };

  each(SimMessageHash::AddElementConsumer, [](const Sim::Pending& p) {
    AddElementConsumerMessage m{};
    if (!Payload(p, &m)) return;
    if (!g->world.ValidGameCell(m.cellIdx)) return;
    ElementConsumerData d{};
    // `Register` seeds the rate at zero and leaves it there: a consumer does nothing at all
    // until a `SetElementConsumerData` gives it one.
    d.consumption_rate = 0.0f;
    d.cell = static_cast<int32_t>(g->world.Padded(static_cast<size_t>(m.cellIdx)));
    d.element = m.elementIdx;
    d.max_depth = m.radius;
    d.configuration = m.configuration;
    d.offset_idx = 0;
    const int32_t handle = g->flow.consumers.Add(d);
    if (m.callbackIdx != -1) {
      g->component_state.push_back(ComponentStateChangedMessage{m.callbackIdx, handle});
    }
  });

  each(SimMessageHash::SetElementConsumerData, [](const Sim::Pending& p) {
    SetElementConsumerDataMessage m{};
    if (!Payload(p, &m)) return;
    ElementConsumerData* d = g->flow.consumers.Get(m.handle);
    if (d == nullptr || !g->world.ValidGameCell(m.cell)) return;
    // Klei writes the cell first and the rate second, and writes nothing else — the
    // element, the radius and the configuration are fixed at registration.
    d->cell = static_cast<int32_t>(g->world.Padded(static_cast<size_t>(m.cell)));
    d->consumption_rate = m.consumptionRate;
  });

  each(SimMessageHash::RemoveElementConsumer, [](const Sim::Pending& p) {
    RemoveElementConsumerMessage m{};
    if (!Payload(p, &m)) return;
    g->flow.consumers.Remove(m.handle);
    if (m.callbackIdx != -1) {
      g->component_state.push_back(ComponentStateChangedMessage{m.callbackIdx, -1});
    }
  });

  each(SimMessageHash::AddElementEmitter, [](const Sim::Pending& p) {
    AddElementEmitterMessage m{};
    if (!Payload(p, &m)) return;
    // Every field but the pressure ceiling and the two callbacks comes from `Register`'s own
    // constants, which are `ElementEmitterData`'s defaults: interval FLT_MAX, mass zero,
    // temperature -1, blocked state 0xff.
    ElementEmitterData d{};
    d.max_pressure = m.maxPressure;
    d.blocked_cb = m.onBlockedCB;
    d.unblocked_cb = m.onUnblockedCB;
    const int32_t handle = g->flow.emitters.Add(d);
    // And the emitter's report slot is blanked at registration, before it has ever run.
    const size_t slot = static_cast<size_t>(handle & kHandleIndexMask);
    if (slot >= g->flow.emitted.size()) g->flow.emitted.resize(slot + 1);
    EmittedMassInfo& e = g->flow.emitted[slot];
    e.elemIdx = 0xFFFF;
    e.diseaseIdx = 0xFF;
    e.mass = 0.0f;
    e.temperature = 0.0f;
    e.diseaseCount = 0;
    if (m.callbackIdx != -1) {
      g->component_state.push_back(ComponentStateChangedMessage{m.callbackIdx, handle});
    }
  });

  each(SimMessageHash::ModifyElementEmitter, [](const Sim::Pending& p) {
    ModifyElementEmitterMessage m{};
    if (!Payload(p, &m)) return;
    ElementEmitterData* d = g->flow.emitters.Get(m.handle);
    if (d == nullptr || !g->world.ValidGameCell(m.cellIdx)) return;
    d->elapsed_time = 0.0f;
    d->emit_interval = m.emitInterval;
    d->emit_mass = m.emitMass;
    d->emit_temperature = m.emitTemperature;
    d->max_pressure = m.maxPressure;
    d->emit_disease_count = m.diseaseCount;
    d->cell = static_cast<int32_t>(g->world.Padded(static_cast<size_t>(m.cellIdx)));
    d->element = m.elementIdx;
    d->max_depth = m.maxDepth;
    d->offset_idx = 0;
    d->disease_idx = m.diseaseIdx;
    // The one conditional write in the whole function,: an emitter modified
    // to carry no mass has its blocked state forced back to the third value, so the next
    // update fires a blocked callback whatever it fired last time.
    if (!(m.emitMass > 0.0f)) d->blocked_state = 0xFF;
  });

  // The disease emitter. `Register` leaves a slot with no cell and no disease,
  // so a registered-but-unmodified emitter does nothing at all.
  each(SimMessageHash::AddDiseaseEmitter, [](const Sim::Pending& p) {
    AddDiseaseEmitterMessage m{};
    if (!Payload(p, &m)) return;
    const int32_t handle = g->flow.disease_emitters.Add(DiseaseEmitterData{});
    const size_t slot = static_cast<size_t>(handle & kHandleIndexMask);
    if (slot >= g->flow.disease_emitted.size()) g->flow.disease_emitted.resize(slot + 1);
    // `UpdateDataListOnly` writes exactly this into every slot, so it is what
    // an emitter that has never fired reports.
    g->flow.disease_emitted[slot].diseaseIdx = 0xFF;
    g->flow.disease_emitted[slot].count = 0;
    if (m.callbackIdx != -1) {
      g->component_state.push_back(ComponentStateChangedMessage{m.callbackIdx, handle});
    }
  });

  // `DiseaseEmitter::Modify`. Five writes and **no reset of `elapsedTime`** —
  // the element emitter's `Modify` zeroes its clock and this one does not, so a re-modified
  // disease emitter keeps counting from where it was.
  each(SimMessageHash::ModifyDiseaseEmitter, [](const Sim::Pending& p) {
    ModifyDiseaseEmitterMessage m{};
    if (!Payload(p, &m)) return;
    DiseaseEmitterData* d = g->flow.disease_emitters.Get(m.handle);
    if (d == nullptr || !g->world.ValidGameCell(m.gameCell)) return;
    d->emit_interval = m.emitInterval;
    d->disease_idx = m.diseaseIdx;
    d->range = m.maxDepth;
    d->emit_count = m.emitCount;
    d->cell = static_cast<int32_t>(g->world.Padded(static_cast<size_t>(m.gameCell)));
  });

  each(SimMessageHash::RemoveDiseaseEmitter, [](const Sim::Pending& p) {
    RemoveDiseaseEmitterMessage m{};
    if (!Payload(p, &m)) return;
    g->flow.disease_emitters.Remove(m.handle);
    if (m.callbackIdx != -1) {
      g->component_state.push_back(ComponentStateChangedMessage{m.callbackIdx, -1});
    }
  });

  each(SimMessageHash::RemoveElementEmitter, [](const Sim::Pending& p) {
    RemoveElementEmitterMessage m{};
    if (!Payload(p, &m)) return;
    g->flow.emitters.Remove(m.handle);
    if (m.callbackIdx != -1) {
      g->component_state.push_back(ComponentStateChangedMessage{m.callbackIdx, -1});
    }
  });
}

// The radiation emitter's three messages, drained after the element emitter's and before
// the disease emitter's, which is where `ProcessFrame` puts them.
//
// Unlike the element emitter, `Register` takes the whole configuration from the message —
// there is no inert default state to modify away from. The one thing it does invent is the
// clamp: `emitSpeed` is stored as `min(emitSpeed, emitRads)` here and as
// `min(emitRate, emitSpeed)` in `Modify`, which is a real asymmetry in the game.
void DrainRadiationQueue() {
  auto each = [](SimMessageHash id, void (*fn)(const Sim::Pending&)) {
    for (const Sim::Pending& p : g->active) {
      if (p.id == static_cast<int32_t>(id)) fn(p);
    }
  };

  each(SimMessageHash::AddRadiationEmitter, [](const Sim::Pending& p) {
    AddRadiationEmitterMessage m{};
    if (!Payload(p, &m)) return;
    if (!g->world.ValidGameCell(m.cell)) return;
    RadiationEmitterData d{};
    d.cell = static_cast<int32_t>(g->world.Padded(static_cast<size_t>(m.cell)));
    d.radius_x = static_cast<uint16_t>(m.emitRadiusX);
    d.radius_y = static_cast<uint16_t>(m.emitRadiusY);
    d.emit_rads = m.emitRads;
    d.emit_rate = m.emitRate;
    d.emit_speed = m.emitSpeed <= m.emitRads ? m.emitSpeed : m.emitRads;
    d.emit_direction = m.emitDirection;
    d.emit_angle = m.emitAngle;
    d.emit_timer = 0.0f;
    d.emit_step_timer = 0.0f;
    d.emit_type = m.emitType;
    d.emit_step = 0;
    const int32_t handle = g->radiation.emitters.Add(d);
    if (m.callbackIdx != -1) {
      g->component_state.push_back(ComponentStateChangedMessage{m.callbackIdx, handle});
    }
  });

  each(SimMessageHash::ModifyRadiationEmitter, [](const Sim::Pending& p) {
    ModifyRadiationEmitterMessage m{};
    if (!Payload(p, &m)) return;
    RadiationEmitterData* d = g->radiation.emitters.Get(m.handle);
    if (d == nullptr || !g->world.ValidGameCell(m.cell)) return;
    // The three pieces of running state — both timers and the phase — are **not** touched:
    // a modified emitter carries on mid-sweep.
    d->cell = static_cast<int32_t>(g->world.Padded(static_cast<size_t>(m.cell)));
    d->radius_x = static_cast<uint16_t>(m.emitRadiusX);
    d->radius_y = static_cast<uint16_t>(m.emitRadiusY);
    d->emit_rads = m.emitRads;
    d->emit_rate = m.emitRate;
    d->emit_speed = m.emitRate <= m.emitSpeed ? m.emitRate : m.emitSpeed;
    d->emit_direction = m.emitDirection;
    d->emit_angle = m.emitAngle;
    d->emit_type = m.emitType;
  });

  each(SimMessageHash::RemoveRadiationEmitter, [](const Sim::Pending& p) {
    RemoveRadiationEmitterMessage m{};
    if (!Payload(p, &m)) return;
    g->radiation.emitters.Remove(m.handle);
    if (m.callbackIdx != -1) {
      g->component_state.push_back(ComponentStateChangedMessage{m.callbackIdx, -1});
    }
  });
}

// One pass over the frame's messages, for one hash. The buildings and the chunks have had
// their own copy of this since they were written; it is at file scope now because the cell
// messages need it too. See `DrainQueue`.
void EachActive(SimMessageHash id, void (*fn)(const Sim::Pending&)) {
  for (const Sim::Pending& p : g->active) {
    if (p.id == static_cast<int32_t>(id)) fn(p);
  }
}

bool KnownMessage(int32_t id) {
  switch (static_cast<SimMessageHash>(id)) {
    case SimMessageHash::ModifyCell:
    case SimMessageHash::Dig:
    case SimMessageHash::MassEmission:
    case SimMessageHash::MassConsumption:
    case SimMessageHash::SetInsulationValue:
    case SimMessageHash::SetStrengthValue:
    case SimMessageHash::ModifyCellEnergy:
    case SimMessageHash::ChangeCellProperties:
    case SimMessageHash::ConsumeDisease:
    case SimMessageHash::CellDiseaseModification:
    case SimMessageHash::CellRadiationModification:
    case SimMessageHash::ModifyBackwallData:
    case SimMessageHash::SetDebugProperties:
    case SimMessageHash::AddBuildingHeatExchange:
    case SimMessageHash::ModifyBuildingHeatExchange:
    case SimMessageHash::ModifyBuildingEnergy:
    case SimMessageHash::RemoveBuildingHeatExchange:
    case SimMessageHash::AddBuildingToBuildingHeatExchange:
    case SimMessageHash::AddInContactBuildingToBuildingToBuildingHeatExchange:
    case SimMessageHash::RemoveBuildingInContactFromBuildingToBuildingHeatExchange:
    case SimMessageHash::RemoveBuildingToBuildingHeatExchange:
    case SimMessageHash::AddElementChunk:
    case SimMessageHash::MoveElementChunk:
    case SimMessageHash::SetElementChunkData:
    case SimMessageHash::ModifyElementChunkEnergy:
    case SimMessageHash::ModifyChunkTemperatureAdjuster:
    case SimMessageHash::RemoveElementChunk:
    case SimMessageHash::AddElementConsumer:
    case SimMessageHash::SetElementConsumerData:
    case SimMessageHash::RemoveElementConsumer:
    case SimMessageHash::AddElementEmitter:
    case SimMessageHash::ModifyElementEmitter:
    case SimMessageHash::RemoveElementEmitter:
    case SimMessageHash::AddDiseaseEmitter:
    case SimMessageHash::ModifyDiseaseEmitter:
    case SimMessageHash::RemoveDiseaseEmitter:
    case SimMessageHash::AddRadiationEmitter:
    case SimMessageHash::ModifyRadiationEmitter:
    case SimMessageHash::RemoveRadiationEmitter:
    case SimMessageHash::RadiationParamsModification:
      return true;
    default:
      return false;
  }
}

// **Klei does not drain in arrival order.** `SIM_HandleMessage` sorts each message into a
// per-category vector on `SimFrameInfo`, and `SimFrameManager::ProcessFrame`
// then walks those vectors in a fixed order that has nothing to do with the order the game
// sent them in. The order below is that function read top to bottom:
//
//   insulation, strength, `ProcessCellEnergyModifications`,
//   `ProcessCellProperties`, `ProcessMassConsumption`, `ProcessMassEmission`,
//   `ProcessConsumeDisease`, the inline disease-modification loop,
//   `ProcessCellRadiationChanges`, `ProcessDigPoints`, `ProcessCellModifications`,
//   then the components.
//
// Arrival order would be wrong, and a suite cannot tell unless a scenario sends two
// categories in one frame — see `msgorder`, which sends a
// `ModifyCell` and a `ModifyCellEnergy` at one cell and is worth 519 K when this is wrong.
// In live play the game sends thousands of messages a frame across a dozen categories, so
// arrival order would have been wrong on nearly every frame.
//
// Cost is one pass per category rather than one pass total. Eleven linear scans of a vector
// that holds a few thousand small structs is not measurable next to a substep, and bucketing
// on the way in would put the sort in `SIM_HandleMessage`, which runs on the game's thread.
void DrainQueue() {
  EachActive(SimMessageHash::SetInsulationValue,
             [](const Sim::Pending& p) { ApplyCellFloat(p, true); });
  EachActive(SimMessageHash::SetStrengthValue,
             [](const Sim::Pending& p) { ApplyCellFloat(p, false); });
  EachActive(SimMessageHash::ModifyCellEnergy, &ApplyCellEnergy);
  EachActive(SimMessageHash::ChangeCellProperties, &ApplyCellProperties);
  EachActive(SimMessageHash::MassConsumption, &ApplyMassConsumption);
  EachActive(SimMessageHash::MassEmission, &ApplyMassEmission);
  EachActive(SimMessageHash::ConsumeDisease, &ApplyConsumeDisease);
  EachActive(SimMessageHash::CellDiseaseModification, &ApplyDiseaseModification);
  EachActive(SimMessageHash::CellRadiationModification, &ApplyRadiationModification);
  EachActive(SimMessageHash::RadiationParamsModification, &ApplyRadiationParams);
  EachActive(SimMessageHash::Dig, &ApplyDig);
  EachActive(SimMessageHash::ModifyCell, &ApplyModifyCell);
  // The backwall is the one cell message with no home in `ProcessFrame` — the game's
  // backwall data is not one of the vectors that function walks, and where Klei applies it
  // has not been found. It is last here because nothing else reads it.
  EachActive(SimMessageHash::ModifyBackwallData, &ApplyBackwall);

  // Then the components, in `SimData`'s own order.
  DrainBuildingQueue();
  DrainChunkQueue();
  DrainElementFlowQueue();
  DrainRadiationQueue();

  for (const Sim::Pending& p : g->active) {
    if (!KnownMessage(p.id)) NoteUnknown(p.id);
  }
  g->active.clear();
}

// ------------------------------------------------------------------ the frame

// The backwall is inert on Klei's side — no kernel moves its mass or its temperature,
// measured — and the one thing the sim does with it is tell the game, every frame, which
// backwalls have left their own element's range. The swap itself is the game's, and it
// comes back as a `ModifyBackwallData`.
//
// The test, from `diffsim --scenario backwall`: a granite backwall at 1000 K announces
// (`highTemp` 942) while the same backwall at 500 K and at 100 K does not; water announces
// on both sides of 272.5 / 372.5; a massless backwall never announces at any temperature;
// and a Vacuum backwall at 20000 K never announces either. That last one is why the range
// alone is not the rule — Vacuum's `highTemp` is 0, so a pure range test would fire — its
// high transition points back at Vacuum, so there is nowhere for it to go.
//
// **Level-triggered, not edge-triggered**: Klei announces the same 40 cells on all 49
// frames of that run, 1960 announcements, for as long as they stay out of range. It read
// as a one-shot at first because `diffsim` stops comparing at the first divergent tick.
//
// So the list is a property of the world rather than of the frame, and it is kept as one:
// the backwall only ever changes when something writes it, so the set is rebuilt from the
// cells written since the last frame and published whole every frame.
bool BackwallShouldTransition(const World& w, const ElementTable& t, size_t p) {
  const SaveBackwall& b = w.Backwall()[p];
  if (b.mass <= 0.0f || !t.HasHash(b.elementHash)) return false;
  const uint16_t idx = t.IndexOfHash(b.elementHash);
  const Element& e = t.At(idx);
  const bool high = b.temperature > e.highTemp &&
                    e.highTempTransitionIdx != 0xFFFF && e.highTempTransitionIdx != idx;
  const bool low = b.temperature < e.lowTemp &&
                   e.lowTempTransitionIdx != 0xFFFF && e.lowTempTransitionIdx != idx;
  return high || low;
}

void UpdateBackwallTransitions() {
  World& w = g->world;
  if (!w.Allocated()) return;
  if (!w.BackwallDirtyAll() && w.BackwallDirty().empty()) return;
  const ElementTable& t = g->elements;
  std::vector<BackwallShouldTransitionInfo>& out = g->backwall_transitions;
  if (w.BackwallDirtyAll()) {
    // Ascending game-cell order, which is the order Klei announces them in.
    out.clear();
    for (size_t game = 0; game < w.GameCount(); ++game) {
      if (BackwallShouldTransition(w, t, w.Padded(game))) {
        out.push_back(BackwallShouldTransitionInfo{static_cast<int32_t>(game)});
      }
    }
  } else {
    for (uint32_t p : w.BackwallDirty()) {
      const int64_t game = w.GameIndex(p);
      if (!w.ValidGameCell(game)) continue;
      const BackwallShouldTransitionInfo key{static_cast<int32_t>(game)};
      // Kept sorted, so a cell a message writes lands where a full rescan would have put
      // it rather than at the end.
      auto at = std::lower_bound(out.begin(), out.end(), key,
                                 [](const BackwallShouldTransitionInfo& a,
                                    const BackwallShouldTransitionInfo& b) {
                                   return a.gameCell < b.gameCell;
                                 });
      const bool present = at != out.end() && at->gameCell == key.gameCell;
      if (BackwallShouldTransition(w, t, p)) {
        if (!present) out.insert(at, key);
      } else if (present) {
        out.erase(at);
      }
    }
  }
  w.ClearBackwallDirty();
}

void ClearFrameEvents() {
  g->state_change_ores.clear();
  g->dig_info.clear();
  g->spawn_ore_info.clear();
  g->mass_consumed.clear();
  g->disease_consumed.clear();
  g->mass_emitted.clear();
  g->callbacks.clear();
  g->radiation.consumed.clear();
  g->unstable_cells.clear();
  g->falling_liquid.clear();
  g->world_damage.clear();
  g->world.ClearCellMelted();
  g->component_state.clear();
  g->projection_events.Clear();
  // Cleared per frame, not per substep: Klei's `SimEvents` vectors live across the substeps
  // of one `PrepareGameData` and are drained by `CopySimDataToGame`, so a cell touched in
  // the first substep is still announced if the second one leaves it alone.
  if (g->world.Allocated()) g->world.ClearSubstanceTouched();
  // `temperatures` is deliberately not cleared: it is indexed by handle and rebuilt in full
  // by every substep that runs, so a frame with no physics still reports the last value each
  // building had rather than an empty list.
  g->buildings.events.ClearFrame();
  // `info` is deliberately not cleared here, for the same reason `temperatures` is not: it
  // is indexed by handle and carries a running `deltaKJ` that only a substep-free frame
  // resets. `melted` is a per-frame list and does get cleared.
  g->chunks.ClearFrame();
}

// Physics: conduction, state changes, sublimation and flow, in that order.
//
// The order is measured, not chosen. Conduction runs before the transition pass — a cell
// walked across its boundary by a hot neighbour is never observed above the boundary and
// still unchanged. The transition pass runs before flow — water on the floor of a sealed
// vacuum shaft has already lost mass upward on the first frame it reads as steam.
// Sublimation runs last of the three, which fell out of an element whose sublimation
// product freezes at room temperature and so oscillates every frame.
//
// The frame's elapsed time is not a timestep — it decides how many fixed 0.2 s substeps to
// run, with the remainder carried into the next frame. Measured: 0.05 s, 0.15 s and 0.2 s
// frames all give identical results and a 0.4 s frame gives exactly what two 0.2 s frames
// give.
//
// The first frame after Start or Load does no physics. That is not a guard, it is
// measured: Klei's sim transfers nothing on its first PrepareGameData and starts on the
// second, and an off-by-one here shows up as a permanent one-tick lead over the whole
// world.
// Returns the number of frames processed, which is what `GameDataUpdate` reports. Always
// one here, and deliberately so: Klei's count is *not* a fixed function of the call. Its sim
// runs on a worker thread and a `PrepareGameData` that finds two frames queued runs both and
// reports 2. Measured with `diffsim`, which now prints the count — roughly one call in
// fifteen. Reproducing that would mean reproducing the scheduler, so this reports the honest
// one-in-one-out and the harness treats a mismatch as a Klei-side event rather than a defect.
int StepPhysics(float elapsed) {
  if (g->skip_physics_frames > 0) {
    --g->skip_physics_frames;
    // `Sim::Main` takes the `UpdateComponentsDataListOnly` branch on a tick that runs no
    // frame, so the components still report — and for chunks that branch is
    // the only thing that ever zeroes `deltaKJ`. On *this* tick they report nothing at all,
    // because `RunFrame` has not drained the queue either: see the note there.
    UpdateChunkDataList(&g->chunks);
    UpdateEmitterDataList(&g->flow);
    return 1;
  }
  // A paused game, not a startup skip: `Sim::Main` takes this same
  // `UpdateComponentsDataListOnly` branch whenever `dt <= 0`, which is what
  // the game sends every render frame while paused -- it keeps calling PrepareGameData with
  // no speed throttling between calls, since there is nothing to throttle. Without this
  // branch a paused frame falls into `SubstepsForFrame`, which forces at least one substep
  // even at dt = 0 (that "always at least one" rule is measured and correct for a genuinely
  // short *running* frame, e.g. dt = 0.05 at the slowest game speed -- it just does not
  // apply to dt <= 0, which is a different condition and never a running frame). The result
  // was one full substep per paused render frame, uncapped by any game speed, which is the
  // fastest the sim can possibly run -- confirmed against Klei with `diffsim --scenario
  // paused`, which sends dt = 0.0 every tick: Klei's cells sit at the seed temperature all
  // 20 ticks, ours drifted up to 55 K. Handled the same way the startup skip already is,
  // since it is the identical branch: no substep, no flow, just republish with a zeroed
  // `deltaKJ`.
  if (elapsed <= 0.0f) {
    UpdateChunkDataList(&g->chunks);
    UpdateEmitterDataList(&g->flow);
    return 1;
  }
  // Conduction runs *before* flow within a substep, and the order is measured rather than
  // chosen. A 20 kg pocket of gas about to spill into an empty cell loses exactly the heat
  // a single 20 kg cell would lose, and the two cells then read the same temperature —
  // which only happens if the whole pocket conducts as one lump first and the transfer
  // splits the result afterwards. Flowing first leaves the two cells 8 K apart.
  // The flow accumulator holds one **frame** of transfers, not one substep. Where Klei
  // clears it has not been found — `SimBase::UpdateData` does not, and `UpdateFlowTexture`
  // only reads — but the cadence is measured twice over. `traceflow` publishes a different
  // set of cells on each of its first four ticks and nothing carries over, so it is not
  // cumulative; and `substeps`, the one scenario that sends `dt = 1.0` and gets five
  // substeps in a frame, is exact only if all five accumulate into the same buffer.
  //
  // It sits after the skip test, not before it, so a skipped physics frame leaves the
  // buffer alone. That is what Klei does by construction: it does not call `UpdateData`.
  g->world.ClearFlow();
  // Whether any of the disease work below has anything to do. See `RefreshDiseaseActive`.
  g->world.RefreshDiseaseActive();
  const int substeps = SubstepsForFrame(elapsed, &g->substep_carry);
  for (int i = 0; i < substeps; ++i) {
    g->pressure_dir = -g->pressure_dir;
    // The whole substep body runs **once per region**, which is `SimBase::UpdateData`'s own
    // shape: is one loop over the region list at stride 0x18 and everything from
    // the conduction dispatch to `Disease::PostProcess` sits inside it. Only
    // three things are outside — the pressure-direction negation, the outer
    // `CellSOA::CopyFrom` and the substep counter — and they are
    // the three that stay outside this loop as well.
    //
    // With one region this is exactly what running each sweep over the region list gave, so
    // every scenario that predates the region probes is unchanged; `regionadj` and
    // `regionlap` are the two that can tell the orderings apart.
    for (size_t ri = 0; ri < g->world.RegionCount(); ++ri) {
      // What this region's substep can leave for `Project` to publish. The inclusive
      // rectangle is the widest any of the kernels below drives from — `StepPostProcess`
      // and the far end of a gas-pressure pair use it — and `World::kProjectReach` covers
      // how far past its own loop each of them can write. `StepGasDisplacement` is the one
      // exception, because its rows are clamped *away* from the region rather than towards
      // it, so it marks its own.
      const World::PaddedRect& pr = g->world.PaddedRegionInclusive(ri);
      g->world.MarkProjectDirtyRect(pr.x0, pr.y0, pr.x1, pr.y1);
      { PROF(kConduction); StepConduction(&g->world, g->elements, ri); }
      {
        PROF(kStateChange);
        StepStateChange(&g->world, g->elements, &g->state_change_ores, ri, FrameVisible(),
                        g->debug_editing, &g->falling_liquid);
      }
      // Gas and liquid are two sweeps, not one, and gas goes first. `SimBase::UpdateData`
      // runs the `UpdatePressure` sweep and only then `UpdateLiquid`,
      // with a fresh `CellSOA::CopyFrom` between them — so liquid never sees a
      // grid gas is halfway through moving across, and gas never sees one a liquid has
      // already fallen through.
      { PROF(kGasPressure); StepGasPressure(&g->world, g->elements, g->diseases, g->pressure_dir, ri); }
      // The second gas sweep, off the *same* snapshot as the first — `UpdateData` does not
      // take a fresh `CellSOA::CopyFrom` until, after both. It has to come
      // straight after `StepGasPressure`, which is what owns that snapshot.
      {
        PROF(kGasDisplacement);
        StepGasDisplacement(&g->world, g->elements, g->pressure_dir, ri, &g->diseases);
      }
      {
        PROF(kFlow);
        StepFlow(&g->world, g->elements, g->first_physics_substep, g->displace_rotation, ri,
                 &g->falling_liquid, FrameVisible(), g->debug_editing, &g->diseases);
        // The second liquid sweep, off the *same* snapshot as the first — `UpdateData` runs
        // it inline between `UpdateLiquid` and the `CellSOA::CopyFrom`.
        // It has to come straight after `StepFlow`, which owns that snapshot
        // and the flow accumulator this sweep reads back.
        StepLiquidDisplacement(&g->world, g->elements, g->pressure_dir, g->displace_rotation,
                               ri, &g->diseases);
        // The fourth and last `CellSOA::CopyFrom` of the substep,. Two readers:
        // the flow texture's gate, so only the elements the texture can ask about are recorded
        // here (see `World::SnapshotFlowElements`), and the germ pair sweep, which retakes
        // the germ half itself (`StepDiseaseDiffusion`).
        g->world.SnapshotFlowElements();
      }
      // Post-process runs **after** flow, not before it. `SimBase::UpdateData` calls
      // `UpdatePressure`, then `UpdateLiquid`, then
      // `PostProcessCell`, in that order, and it is a real difference rather
      // than bookkeeping: the shuffle and `DoDensityDisplacement` see the grid a fluid cell
      // has already moved through, so the cell a liquid vacated is gas again by the time the
      // sweep reaches it and draws its number. Running it first cost one draw per falling
      // liquid cell per substep and put the whole world onto a different random stream.
      // The germ pair sweep sits between the liquid sweep and `PostProcessCell`, which is
      // where `UpdateData` puts it: is after `UpdateLiquid` and
      // before `PostProcessCell`. It reads the copy Klei takes after both
      // liquid sweeps, which it retakes itself, so it has to stay downstream of
      // them.
      if (g->world.DiseaseActive()) {
        PROF(kDisease);
        StepDiseaseDiffusion(&g->world, g->elements, g->diseases, ri);
      }
      // The components run after the fluid kernels and before the substep counter turns over,
      // which is where `SimData::UpdateComponents` sits inside `SimBase::UpdateData`. The
      // order is the order they sit in `SimData`: element consumer, element emitter,
      // radiation emitter, element chunk, building cell exchange, building-to-building,
      // disease emitter. Only the disease emitter is still missing.
      // The radiation field: decay, the cosmic occlusion column walk, and the three
      // sources. It sits immediately before `SimData::UpdateComponents` in `UpdateData`,
      // which puts it ahead of the emitter that feeds it.
      {
        PROF(kRadiation);
        StepRadiationField(&g->world, g->elements, g->diseases, &g->radiation, ri);
      }
      {
        PROF(kElementFlow);
        StepElementConsumers(&g->world, g->elements, g->diseases, &g->flow, kSubstepSeconds,
                             ri);
        StepElementEmitters(&g->world, g->elements, g->diseases, &g->flow,
                            g->displace_rotation, kSubstepSeconds, &g->spawn_ore_info,
                            &g->callbacks, FrameVisible(),
                            g->debug_editing, ri);
      }
      {
        PROF(kRadiation);
        StepRadiationEmitters(&g->world, g->elements, &g->radiation, kSubstepSeconds, ri);
      }
      {
        PROF(kElementChunk);
        StepElementChunks(&g->world, g->elements, &g->chunks, kSubstepSeconds, ri);
      }
      {
        PROF(kBuildingHeat);
        StepBuildingHeatExchange(&g->world, g->elements, &g->buildings, kSubstepSeconds,
                                 &g->state_change_ores, ri, FrameVisible(), g->debug_editing,
                                 &g->falling_liquid);
      }
      {
        PROF(kBuildingToBuilding);
        StepBuildingToBuilding(&g->world, &g->buildings, kSubstepSeconds, ri);
      }
      // The disease emitter is **seventh and last** in `SimData`'s component list
      // (constructor), so it runs after both building exchanges. It is the
      // only component that creates germs.
      {
        PROF(kElementFlow);
        StepDiseaseEmitters(&g->world, g->elements, g->diseases, &g->flow, kSubstepSeconds,
                            ri);
      }
      {
        PROF(kPostProcess);
        const CellModContext cm{&g->world, &g->elements, &g->diseases, g->displace_rotation};
        StepPostProcess(&g->world, g->elements, g->displace_rotation, &g->unstable_cells, ri,
                        &g->falling_liquid, FrameVisible(), g->debug_editing, &g->world_damage,
                        OverfullDisplace{&cm, &DoOverfullDisplace}, &g->diseases);
      }
      // Only now are the cells the substep emptied turned into vacuum. Klei has no such
      // sweep at all — it clears a cell at the moment a mover empties it, or leaves the
      // element in place until `PostProcessCell` reaches it and `Evaporate`s it — so the
      // pass has to sit *after* post-process or the sweep loses a gas cell, its two draws
      // and one negation of the shuffle stride. See the note above the function.
      //
      // It stays a whole-grid pass inside the per-region loop rather than a per-region one:
      // a mover may empty a cell outside the rectangle it is sweeping, because the
      // destination test is the active mask and that is the union of every region.
      { PROF(kZeroMassless); ZeroMasslessCells(&g->world, g->elements); }
      // `Disease::PostProcess` is the last call inside `UpdateData`'s region
      // loop — after the components, after `PostProcessCell` — so the growth sweep sees a
      // cell at the temperature and mass the whole substep left it at.
      if (g->world.DiseaseActive()) {
        PROF(kDisease);
        StepDiseasePostProcess(&g->world, g->elements, g->diseases,
                               g->world.RadiationEnabled(), ri);
      }
    }
    g->first_physics_substep = false;
    ++g->displace_rotation;
  }
  // A transition that names a transition ore hands that share of the cell's mass to the
  // game rather than keeping it in the grid, so it has to be announced or the mass is
  // simply gone. Klei reports it in the same list as a dug-out cell.
  for (const StateChangeOre& o : g->state_change_ores) {
    SpawnOreInfo ore{};
    ore.cellIdx = o.game_cell;
    ore.elemIdx = o.element;
    ore.mass = o.mass;
    ore.temperature = o.temperature;
    ore.diseaseIdx = o.disease_idx;
    ore.diseaseCount = o.disease_count;
    g->spawn_ore_info.push_back(ore);
  }
  g->state_change_ores.clear();
  return 1;
}

// The `spawnOreInfo` export, which is not a copy: `SimBase::CopySimDataToGame` **sorts** the
// sim's list and then **coalesces** it (0x180032... , the `_Sort_unchecked` at the top of the
// spawn-ore block and the run-merge loop after it).
//
// Two ore drops that name the same cell *and* the same element become one entry carrying
// their combined mass and a mass-weighted temperature clamped to the two — through
// `FastCalculateCombinedTemperature`, with the cooler side passed first. `diseaseIdx` and
// `diseaseCount` are **not** merged: the first entry of a run keeps its own and the rest are
// discarded.
//
// Nothing in this project needed it until a solid element emitter turned up, because that is
// the first thing that can drop several lumps of ore in one frame. It applies to transition
// ore as well, which is why it lives here rather than in the emitter: the sort is what makes
// the list's *order* a property of the world rather than of the kernel that filled it.
//
// `digInfo` is a separate vector and is **not** sorted, even though it holds the same struct.
void PublishSpawnOre() {
  std::vector<SpawnOreInfo>& v = g->spawn_ore_info;
  if (v.size() < 2) return;
  std::sort(v.begin(), v.end(), [](const SpawnOreInfo& a, const SpawnOreInfo& b) {
    if (a.cellIdx != b.cellIdx) return a.cellIdx < b.cellIdx;
    return a.elemIdx < b.elemIdx;
  });
  size_t out = 0;
  for (size_t i = 1; i < v.size(); ++i) {
    SpawnOreInfo& head = v[out];
    const SpawnOreInfo& next = v[i];
    if (head.cellIdx != next.cellIdx || head.elemIdx != next.elemIdx) {
      v[++out] = next;
      continue;
    }
    const float total = head.mass + next.mass;
    if (total > 0.0f) {
      const bool next_cooler = next.temperature < head.temperature;
      const float m_a = next_cooler ? next.mass : head.mass;
      const float t_a = next_cooler ? next.temperature : head.temperature;
      const float m_b = next_cooler ? head.mass : next.mass;
      const float t_b = next_cooler ? head.temperature : next.temperature;
      head.temperature = FastCalculateCombinedTemperature(m_a, t_a, m_b, t_b);
    } else {
      head.temperature = 0.0f;
    }
    head.mass = head.mass + next.mass;
  }
  v.resize(out + 1);
}

// ---------------------------------------------------------------- the in-game digest log
//
// A golden that needs no mod, no Klei and no written scenario. Once per published frame it
// hashes the arrays the game is about to read and appends one line to `sim_digest.log`, next
// to the DLL.
//
// Two jobs. It is the **determinism** oracle: the same save run twice must produce the same
// digests line for line, and when the sim moves onto its own thread the
// digests must still match the ones taken while it was synchronous — which is the whole
// argument that threading changed nothing. And it carries `SIM_DebugLedger`'s mass drift,
// which is the one correctness instrument that works on a real 234-cycle colony where no
// golden exists.
//
// Off unless a marker file `sim_digest.on` is in the working directory (a relative `fopen`,
// as for `sim_nothread.on`), because hashing four arrays
// over a quarter of a million cells is not free and would otherwise pollute every frame-time
// measurement taken in the live game.
FILE* g_digest = nullptr;
bool g_digest_checked = false;
uint64_t g_digest_frame = 0;

uint64_t DigestBytes(uint64_t h, const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) {
    h ^= b[i];
    h *= 1099511628211ULL;  // FNV-1a
  }
  return h;
}

void WriteDigest(const GameDataUpdate& u, int frames) {
  if (!g_digest_checked) {
    g_digest_checked = true;
    if (FILE* marker = fopen("sim_digest.on", "rb")) {
      fclose(marker);
      g_digest = fopen("sim_digest.log", "w");
    }
  }
  if (!g_digest) return;
  const size_t n = g->world.GameCount();
  uint64_t h = 1469598103934665603ULL;
  h = DigestBytes(h, u.elementIdx, n * sizeof(uint16_t));
  h = DigestBytes(h, u.mass, n * sizeof(float));
  h = DigestBytes(h, u.temperature, n * sizeof(float));
  h = DigestBytes(h, u.diseaseCount, n * sizeof(int32_t));
  // The mass ledger, the same numbers `SIM_DebugLedger` exports: the grid total and the net
  // of every sanctioned flow. `grid - net` is the drift, and what matters is whether it
  // *moves* from frame to frame — on a real colony there is no golden to check against, so a
  // drift that stays put is the strongest correctness statement available.
  const World::Ledger& l = g->world.Books();
  // Per **call site**, in `SIM_DebugLedger`'s order minus its field 0. The buckets are
  // cumulative, so what goes in the log is each one's *delta* for this frame: a drift step
  // is only diagnosable next to the list of sites that ran on the frame it stepped. Sites
  // that did nothing are left out, which keeps the line short on the overwhelming majority
  // of frames where nothing crosses the boundary at all.
  static const char* const kBucketNames[] = {
      "emitted", "modified", "consumed", "dug",  "ore",     "unstable",  "sublimated",
      "wisp",    "thin_liq", "cleared",  "comp_consumed", "comp_emitted", "emitter_ore"};
  const double buckets[] = {l.emitted,   l.modified,   l.consumed,          l.dug,
                            l.ore,       l.unstable,   l.sublimated,        l.wisp,
                            l.thin_liquid, l.cleared,  l.component_consumed,
                            l.component_emitted, l.emitter_ore};
  const int kBuckets = static_cast<int>(sizeof(buckets) / sizeof(buckets[0]));
  static double prev_buckets[sizeof(buckets) / sizeof(buckets[0])] = {};
  static double prev_drift = 0.0;

  // Gains minus losses, and the *same* formula as `LedgerNet` in `driver/src/diffsim.cpp`
  // — the two have to agree or the offline suite and the live game are measuring different
  // quantities under the same name. `emitter_ore` is deliberately not in the sum: a solid
  // element emitter hands the game a lump of ore made out of mass the building holds
  // outside the sim, so it is neither a grid gain nor a grid loss.
  const double net = l.emitted + l.modified + l.component_emitted -
                     (l.consumed + l.dug + l.ore + l.unstable + l.sublimated + l.wisp +
                      l.thin_liquid + l.cleared + l.component_consumed);
  const double grid = g->world.TotalGridMass();
  const double drift = grid - net;

  fprintf(g_digest,
          "frame %llu frames %d cells %zu digest %016llx grid %.6f net %.6f drift %.6f "
          "ddrift %.6f",
          static_cast<unsigned long long>(g_digest_frame++), frames, n,
          static_cast<unsigned long long>(h), grid, net, drift, drift - prev_drift);
  prev_drift = drift;
  for (int i = 0; i < kBuckets; ++i) {
    const double d = buckets[i] - prev_buckets[i];
    prev_buckets[i] = buckets[i];
    if (d != 0.0) fprintf(g_digest, " %s %.6f", kBucketNames[i], d);
  }
  fputc('\n', g_digest);
  fflush(g_digest);
}

GameDataUpdate* BuildUpdate(int frames) {
  // `CopySimDataToGame` ends by swapping the frame's visibility buffer with the sim-side
  // `GameData`'s. See `Sim::visible`.
  std::swap(g->visible, g->visible_game[g->visible_sim_slot]);
  // The slot the worker is allowed to fill, and it advances on every publication rather than
  // only on the threaded ones — a Start followed by the first `PrepareGameData` publishes
  // twice in a row with the game holding the first pointer in between.
  PublishedFrame& p = g->pub[g->pub_slot];
  g->pub_slot ^= 1;
  p.Begin();
  GameDataUpdate& u = p.update;
  u.numFramesProcessed = frames;

  ProjectionBuffers& b = g->buffers;
  p.Put(u.elementIdx, b.element);
  p.Put(u.temperature, b.temperature);
  p.Put(u.mass, b.mass);
  p.Put(u.properties, b.properties);
  p.Put(u.insulation, b.insulation);
  p.Put(u.strengthInfo, b.strength);
  p.Put(u.radiation, b.radiation);
  p.Put(u.diseaseIdx, b.disease_idx);
  p.Put(u.diseaseCount, b.disease_count);
  p.Put(u.backwallElement, b.backwall_element);
  p.Put(u.backwallMass, b.backwall_mass);
  p.Put(u.backwallTemperature, b.backwall_temperature);
  p.Put(u.accumulatedFlow, b.accumulated_flow);

  u.numSubstanceChangeInfo = static_cast<int32_t>(g->projection_events.substance.size());
  p.Put(u.substanceChangeInfo, g->projection_events.substance);
  u.numSolidInfo = static_cast<int32_t>(g->projection_events.solid.size());
  p.Put(u.solidInfo, g->projection_events.solid);
  u.numSolidSubstanceChangeInfo =
      static_cast<int32_t>(g->projection_events.solid_substance.size());
  p.Put(u.solidSubstanceChangeInfo, g->projection_events.solid_substance);
  u.numLiquidChangeInfo = static_cast<int32_t>(g->projection_events.liquid.size());
  p.Put(u.liquidChangeInfo, g->projection_events.liquid);
  u.numDigInfo = static_cast<int32_t>(g->dig_info.size());
  p.Put(u.digInfo, g->dig_info);
  PublishSpawnOre();
  u.numSpawnOreInfo = static_cast<int32_t>(g->spawn_ore_info.size());
  p.Put(u.spawnOreInfo, g->spawn_ore_info);
  u.numMassConsumedCallbacks = static_cast<int32_t>(g->mass_consumed.size());
  p.Put(u.massConsumedCallbacks, g->mass_consumed);
  u.numMassEmittedCallbacks = static_cast<int32_t>(g->mass_emitted.size());
  p.Put(u.massEmittedCallbacks, g->mass_emitted);
  u.numCallbackInfo = static_cast<int32_t>(g->callbacks.size());
  p.Put(u.callbackInfo, g->callbacks);
  u.numUnstableCellInfo = static_cast<int32_t>(g->unstable_cells.size());
  p.Put(u.unstableCellInfo, g->unstable_cells);
  u.numCellMeltedInfos = static_cast<int32_t>(g->world.CellMelted().size());
  p.Put(u.cellMeltedInfos, g->world.CellMelted());
  u.numWorldDamageInfo = static_cast<int32_t>(g->world_damage.size());
  p.Put(u.worldDamageInfo, g->world_damage);
  u.numSpawnFallingLiquidInfo = static_cast<int32_t>(g->falling_liquid.size());
  p.Put(u.spawnFallingLiquidInfo, g->falling_liquid);
  u.numBackwallShouldTransitionInfos =
      static_cast<int32_t>(g->backwall_transitions.size());
  p.Put(u.backwallShouldTransitionInfos, g->backwall_transitions);
  u.numComponentStateChangedMessages =
      static_cast<int32_t>(g->component_state.size());
  p.Put(u.componentStateChangedMessages, g->component_state);
  // Filled by `ProcessCellRadiationChanges`, not by the emitters: the sim tells the game how
  // many rads each `CellRadiationModification` with a callback actually moved.
  u.numRadiationConsumedCallbacks = static_cast<int32_t>(g->radiation.consumed.size());
  p.Put(u.radiationConsumedCallbacks, g->radiation.consumed);

  // The two element components. `consumed` is a list of what the pumps took this frame and
  // is handed over whole; `emitted` is indexed by handle slot and is only partly reset, so
  // both go through a published copy the reset cannot walk over.
  PublishElementFlow(&g->flow, g->elements.VacuumIndex());
  u.numDiseaseConsumptionCallbacks = static_cast<int32_t>(g->disease_consumed.size());
  p.Put(u.diseaseConsumptionCallbacks, g->disease_consumed);
  u.numDiseaseEmittedInfos = static_cast<int32_t>(g->flow.published_disease_emitted.size());
  p.Put(u.diseaseEmittedInfos, g->flow.published_disease_emitted);
  u.numRemovedMassEntries = static_cast<int32_t>(g->flow.published_consumed.size());
  p.Put(u.removedMassEntries, g->flow.published_consumed);
  u.numEmittedMassEntries = static_cast<int32_t>(g->flow.published_emitted.size());
  p.Put(u.emittedMassEntries, g->flow.published_emitted);

  // Element chunks. `info` is indexed by handle slot, `melted` is a bare handle each.
  // `PublishChunkInfo` hands out a copy and zeroes the accumulator behind it, which is what
  // makes `deltaKJ` one frame's energy rather than the running total the substep builds.
  PublishChunkInfo(&g->chunks);
  u.numElementChunkInfos = static_cast<int32_t>(g->chunks.published.size());
  p.Put(u.elementChunkInfos, g->chunks.published);
  u.numElementChunkMeltedInfos = static_cast<int32_t>(g->chunks.melted.size());
  p.Put(u.elementChunkMeltedInfos, g->chunks.melted);

  // Buildings. `buildingTemperatures` is the one the game reads every frame — it is how a
  // machine's temperature gets back into `StructureTemperatureComponents` at all. The other
  // three are `MeltedInfo`, a bare handle each.
  BuildingEvents& be = g->buildings.events;
  u.numBuildingTemperatures = static_cast<int32_t>(be.temperatures.size());
  p.Put(u.buildingTemperatures, be.temperatures);
  u.numBuildingOverheatInfos = static_cast<int32_t>(be.overheated.size());
  p.Put(u.buildingOverheatInfos, be.overheated);
  u.numBuildingNoLongerOverheatedInfos =
      static_cast<int32_t>(be.no_longer_overheated.size());
  p.Put(u.buildingNoLongerOverheatedInfos, be.no_longer_overheated);
  u.numBuildingMeltedInfos = static_cast<int32_t>(be.melted.size());
  p.Put(u.buildingMeltedInfos, be.melted);

  // These are raw buffers the game feeds to Texture2D.LoadRawTextureData, not handles.
  // A null here is not "no texture", it is a null dereference inside Unity, so they are
  // always allocated and always published.
  p.Put(u.propertyTextureFlow, g->textures.flow);
  p.Put(u.propertyTextureLiquid, g->textures.liquid);
  p.Put(u.propertyTextureLiquidData, g->textures.liquid_data);
  p.Put(u.propertyTextureMaterialData, g->textures.material_data);
  p.Put(u.propertyTextureExposedToSunlight, g->textures.exposed_to_sun);

  // Only now do the offsets become pointers: every `Put` above may have moved the buffer.
  p.Finish();
  WriteDigest(u, frames);
  g->last_published = &u;
  return &u;
}

// Once per frame start, on the game thread, immediately before the frame begins -- the one
// release Klei makes at the top of `Sim::Main`. Called from `PrepareGameData`
// on both of its paths: before the synchronous `RunFrame` of the first call after a Start, an
// Alloc or a Load (or of every call under `sim_nothread.on`), and before each `Kick`. Exactly
// one call per frame, so the parity in `ConduitTemperatures` flips exactly as often as it did
// when this lived inside `RunFrame`, and a handle the game removes during tick N is still
// released at the start of frame N + 1.
//
// One difference from the old placement, stated rather than left silent: this runs even on a
// `PrepareGameData` that finds no world allocated, where `RunFrame`'s own early return used to
// skip the release. That is Klei's arrangement rather than a new one -- its release sits at the
// top of `Sim::Main`, ahead of anything that could decline to step a world -- and the queue is
// empty in that window unless the game added and removed a conduit handle before allocating.
//
// WHY NOT INSIDE THE FRAME. `RunFrame` runs on the sim worker; the conduit exports run on the
// game thread and are not behind `WaitIdle` (they would stall every conduit call on the
// in-flight frame). A release on the worker compacted the conduit vector concurrently with
// `ConduitTemperatureManager_Update` iterating it and `_Set` writing through a pointer into it
// -- and `ConduitFlow.RebuildConnections` removes EVERY handle of a conduit type, so the queue
// is non-empty after every pipe placed or removed. Found by reading; no offline scenario
// reaches conduits, so the goldens cannot witness it either way. See docs/THREADING.md.
void ReleaseConduitHandlesForNextFrame() { g->conduits.ReleaseQueuedHandles(); }

GameDataUpdate* RunFrame() {
  if (!g->world.Allocated()) return nullptr;
  PROF(kFrame);
  if (g_prof.on) ++g_prof.frames;
  ClearFrameEvents();
  // Klei releases the conduit handles removed during the previous frame HERE,
  // immediately before `BeginFrameProcessing` drains the queue. Ours does not, and the move
  // is a threading fix rather than a reordering: this function runs on the sim worker, while
  // every `ConduitTemperatureManager_*` export runs on the game thread (and `Set` on the
  // game's job threads) straight after `PrepareGameData` kicks this frame. Releasing here
  // compacted the conduit vector underneath an `Update` walking it. The release now happens
  // in `ReleaseConduitHandlesForNextFrame`, on the game thread, at the one point that is
  // still "the start of this frame" as the game can observe it -- nothing the game does can
  // land between that call and this frame starting.
  // The frame drains the messages of the tick *before* it, not its own. See `Sim::active`.
  // The rotation that fills `active` is not here any more: it belongs to the game thread now
  // and happens at `PrepareGameData`, once this frame is collected and before the next one is
  // handed over. Doing it here would mean touching `g->queue` while the game is pushing onto
  // it. The mapping is unchanged — a message still waits exactly one frame.
  { PROF(kDrainQueue); DrainQueue(); }
  const int frames = StepPhysics(g->elapsed_seconds);
  UpdateBackwallTransitions();
  {
    PROF(kProject);
    Project(g->world, g->elements, &g->buffers, &g->projection_events, g->first_frame);
  }
  {
    PROF(kTextures);
    FillPropertyTextures(g->world, g->elements, g->buffers, &g->textures);
  }
  // See `World::PromoteWorldOffsets`: the frame that received the offsets publishes without
  // them, exactly as Klei's does.
  g->world.PromoteWorldOffsets();
  g->first_frame = false;
  return BuildUpdate(frames);
}

}  // namespace

extern "C" {

__declspec(dllexport) void SIM_Initialize(GameMessageHandler callback) {
  StopWorker();
  delete g;
  g = new Sim();
  g->callback = callback;
  StartWorker();
}

__declspec(dllexport) void SIM_Shutdown() {
  // Before `delete g`, not after: the worker dereferences it on every frame.
  StopWorker();
  delete g;
  g = nullptr;
}

// The deferred messages, split out of the dispatch below for the sim thread's sake. These
// change the world and must land on a frame boundary, so all they do here is copy bytes onto
// `g->queue` — which belongs to the game thread alone. The worker reads `g->active`, and the
// rotation between the two happens at `PrepareGameData` with the worker stopped.
//
// Everything *else* touches state the worker owns and has to wait for it, and waiting on each
// of the thousands of these the game sends per frame would give back exactly the overlap
// threading buys. That is why the split exists at all.
bool QueueDeferredMessage(int sim_msg_id, size_t len, const uint8_t* msg) {
  switch (static_cast<SimMessageHash>(sim_msg_id)) {
    case SimMessageHash::ModifyCell:
    case SimMessageHash::Dig:
    case SimMessageHash::MassEmission:
    case SimMessageHash::MassConsumption:
    case SimMessageHash::SetInsulationValue:
    case SimMessageHash::SetStrengthValue:
    case SimMessageHash::ModifyCellEnergy:
    case SimMessageHash::ChangeCellProperties:
    case SimMessageHash::ConsumeDisease:
    case SimMessageHash::CellDiseaseModification:
    case SimMessageHash::CellRadiationModification:
    case SimMessageHash::ModifyBackwallData:
    // The building components. `SetDebugProperties` is in this list because it carries the
    // two scales the components multiply every transfer by, and the game sends it once per
    // frame — 3,819 times in one recorded session.
    case SimMessageHash::SetDebugProperties:
    case SimMessageHash::AddBuildingHeatExchange:
    case SimMessageHash::ModifyBuildingHeatExchange:
    case SimMessageHash::ModifyBuildingEnergy:
    case SimMessageHash::RemoveBuildingHeatExchange:
    case SimMessageHash::AddBuildingToBuildingHeatExchange:
    case SimMessageHash::AddInContactBuildingToBuildingToBuildingHeatExchange:
    case SimMessageHash::RemoveBuildingInContactFromBuildingToBuildingHeatExchange:
    case SimMessageHash::RemoveBuildingToBuildingHeatExchange:
    // Element chunks: matter the game holds outside the grid, and the busiest thing it
    // sends after the per-frame messages — 12,194 calls over these five hashes in a
    // 38-second capture.
    case SimMessageHash::AddElementChunk:
    case SimMessageHash::MoveElementChunk:
    case SimMessageHash::SetElementChunkData:
    case SimMessageHash::ModifyElementChunkEnergy:
    case SimMessageHash::ModifyChunkTemperatureAdjuster:
    case SimMessageHash::RemoveElementChunk:
    // The two element components. Third and fourth busiest component groups in the census,
    // and the pair that moves matter between a building and the grid.
    case SimMessageHash::AddElementConsumer:
    case SimMessageHash::SetElementConsumerData:
    case SimMessageHash::RemoveElementConsumer:
    case SimMessageHash::AddElementEmitter:
    case SimMessageHash::ModifyElementEmitter:
    case SimMessageHash::RemoveElementEmitter:
    case SimMessageHash::AddDiseaseEmitter:
    case SimMessageHash::ModifyDiseaseEmitter:
    case SimMessageHash::RemoveDiseaseEmitter:
    // The radiation emitter, third in the component list. `RadiationParamsModification` is
    // in this list rather than with the cell messages because it *is* one: the same
    // function drains it, straight after the per-cell rads changes.
    case SimMessageHash::AddRadiationEmitter:
    case SimMessageHash::ModifyRadiationEmitter:
    case SimMessageHash::RemoveRadiationEmitter:
    case SimMessageHash::RadiationParamsModification:
      break;

    default:
      return false;
  }
  Sim::Pending p;
  p.id = sim_msg_id;
  if (msg != nullptr && len != 0) p.payload.assign(msg, msg + len);
  g->queue.push_back(std::move(p));
  return true;
}

void* HandleImmediateMessage(int sim_msg_id, size_t len, const uint8_t* msg) {
  switch (static_cast<SimMessageHash>(sim_msg_id)) {
    case SimMessageHash::Elements_CreateTable:
      // msg_length overshoots for this message — the managed side sends the stream's
      // grown capacity, roughly twice the real payload. Parse the count and trust that,
      // never the length.
      if (!g->elements.Load(msg, len)) Report("element table rejected");
      return nullptr;

    case SimMessageHash::Disease_CreateTable:
      if (!g->diseases.Load(msg, len)) Report("disease table rejected");
      return nullptr;

    // `SimData::ResizeAndInitializeVacuumCells`. This is how a world is opened
    // in the grid: a rectangle of Vacuum, walled in by a one-cell ring of Unobtanium, with
    // the backwall of both set to Vacuum.
    //
    // The name only describes the second half. What it actually writes, measured field by
    // field against Klei on `vacrect` — the "240 tonnes" that made the first attempt look
    // wrong is the ring, and it is 24 cells of 9999 kg:
    //
    //   * the **ring**, one cell outside the rectangle on every side: element Unobtanium,
    //     mass a flat **9999 kg** (a constant in the function — Unobtanium's own table row
    //     carries 10000 and 20000 and neither of them lands here), temperature 0, and the
    //     two disease fields cleared. Its **radiation is left alone**;
    //   * the **rectangle**: element Vacuum, mass 0, temperature 0, the two disease fields
    //     cleared, and radiation zeroed as well. Not the infestation counter and not the
    //     growth accumulator, which is what separates this from `ClearCell`;
    //   * the **backwall** over ring and rectangle together: element Vacuum, mass 0,
    //     temperature 0.
    //
    // The radiation split is the load-bearing measurement and it is the one a guess gets
    // wrong: a ring cell keeps its rads and a rectangle cell loses them, which is only
    // visible because `vacrect` now seeds germs and rads in **three** zones — inside, on
    // the ring, and outside as the control.
    //
    // `gridSizeX`/`gridSizeY` are the resize the name promises. The game sends the grid it
    // already has whenever the grid is not actually changing size, which is every case
    // observed so far, so a differing pair is reported rather than acted on.
    case SimMessageHash::SimData_ResizeAndInitializeVacuumCells: {
      int32_t v[6] = {0, 0, 0, 0, 0, 0};
      if (len < sizeof(v)) return nullptr;
      memcpy(v, msg, sizeof(v));
      if (v[0] != g->world.GameWidth() || v[1] != g->world.GameHeight()) {
        Report("ResizeAndInitializeVacuumCells: grid resize is not implemented");
        return nullptr;
      }
      const int32_t rw = v[2], rh = v[3], rx = v[4], ry = v[5];
      if (rw <= 0 || rh <= 0) return nullptr;
      const uint16_t vacuum = g->elements.VacuumIndex();
      const uint16_t border = g->elements.UnobtaniumIndex();
      constexpr int32_t kVacuumHash = 758759285;
      const int32_t vacuum_hash = g->elements.HasHash(kVacuumHash) ? kVacuumHash : 0;

      auto each = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1, auto&& fn) {
        for (int32_t y = y0; y <= y1; ++y) {
          if (y < 0 || y >= g->world.GameHeight()) continue;
          for (int32_t x = x0; x <= x1; ++x) {
            if (x < 0 || x >= g->world.GameWidth()) continue;
            const size_t game = static_cast<size_t>(y) * g->world.GameWidth() +
                                static_cast<size_t>(x);
            fn(g->world.Padded(game), game);
          }
        }
      };

      // The ring and the rectangle in one walk, because the ring write is the rectangle
      // write with a different element and without the radiation clear — Klei fills the
      // whole expanded rectangle with the border and then clears the inside of it.
      each(rx - 1, ry - 1, rx + rw, ry + rh, [&](size_t cell, size_t game) {
        g->world.MarkProjectDirty(cell);
        g->world.TouchSubstance(cell);
        PhaseEntry& p = g->world.Phase(cell);
        p.element = border;
        p.mass = 9999.0f;
        p.temperature = 0.0f;
        g->world.MutableDiseaseIdx(cell) = 0xFF;
        SaveDisease& d = g->world.MutableDisease(cell);
        d.diseaseHash = 0;
        d.count = 0;
        SaveBackwall& b = g->world.MutableBackwall(cell);
        b.elementHash = vacuum_hash;
        b.mass = 0.0f;
        b.temperature = 0.0f;
        // The projection's memory of the cell moves with the cell. `Project` announces a
        // `solidInfo` when a cell's solidity differs from `buffers.previous`, and the ring
        // turns twenty-four gas cells into a solid border — yet Klei announces **none** of
        // them, on this frame or any later one. So the message updates the published side
        // as well as the live grid, and the ring is solid and massive in both before the
        // next projection ever looks at it.
        //
        // Only `previous` is written, not `buffers.element`: the substance announcements
        // *are* published for these cells (78 of them, matching Klei cell for cell), and
        // they are driven by the element diff against that buffer. Solidity is the one
        // memory the message resets.
        if (game < g->buffers.previous.size()) {
          g->buffers.previous[game] = static_cast<uint8_t>(kPrevSolid | kPrevHadMass);
        }
      });
      each(rx, ry, rx + rw - 1, ry + rh - 1, [&](size_t cell, size_t game) {
        PhaseEntry& p = g->world.Phase(cell);
        p.element = vacuum;
        p.mass = 0.0f;
        p.temperature = 0.0f;
        g->world.SetRadiation(cell, 0.0f);
        // Same reset, the other way up: the cleared rectangle is neither solid nor massive.
        if (game < g->buffers.previous.size()) g->buffers.previous[game] = 0;
      });
      // The rectangle is not only cleared, it is *registered*: from the next published frame
      // the sunlight sweep runs over it and it reads back fully lit, and a solid dropped into
      // it later shadows the column below itself. See `World::AddWorldOffset`.
      g->world.AddWorldOffset(World::WorldOffset{rx, ry, rw, rh});
      return nullptr;
    }

    case SimMessageHash::DefineWorldOffsets: {
      // `DefineWorldOffsets`: an int32 count, then four int32s per entry into
      // a 16-byte `SimData::WorldOffsetData`. The sim keeps them for one reason we know of —
      // the sunlight texture is computed per world, and stays zero until this arrives.
      int32_t count = 0;
      if (len >= 4) memcpy(&count, msg, 4);
      std::vector<World::WorldOffset> v;
      if (count > 0 && len >= static_cast<size_t>(4 + count * 16)) {
        v.resize(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i) {
          const uint8_t* p = msg + 4 + static_cast<size_t>(i) * 16;
          memcpy(&v[static_cast<size_t>(i)].x, p, 4);
          memcpy(&v[static_cast<size_t>(i)].y, p + 4, 4);
          memcpy(&v[static_cast<size_t>(i)].w, p + 8, 4);
          memcpy(&v[static_cast<size_t>(i)].h, p + 12, 4);
        }
      }
      g->world.SetWorldOffsets(std::move(v));
      return nullptr;
    }

    case SimMessageHash::AllocateCells: {
      int32_t w = 0, h = 0;
      if (len >= 8) {
        memcpy(&w, msg, 4);
        memcpy(&h, msg + 4, 4);
      }
      if (w > 0 && h > 0) {
        g->world.Allocate(w, h);
        g->buffers.Allocate(g->world.GameCount());
        g->textures.Allocate(g->world.GameCount());
        g->first_frame = true;
        g->pipelined = false;
        ResetVisibility();
        g->skip_physics_frames = 1;
        g->first_physics_substep = true;
        g->displace_rotation = 0;
        g->pressure_dir = -1;
        // This path seeds the random stream from the wall clock: `AllocateCells` passes
        // `_time64(NULL)` as the constructor's seed argument, where `InitializeFromCells`
        // passes the world seed. A save loaded twice therefore shuffles its gas
        // differently, which is Klei's behaviour and not something to "fix".
        g->world.SetRandomState(static_cast<uint32_t>(time(nullptr)));
        g->world.SetShuffleDir(-1);
        // An Alloc builds a fresh SimData in Klei's sim, and the components live in it. Every
        // building handle the game holds is stale after this and the game re-registers them.
        g->buildings.Clear();
        g->chunks.Clear();
        g->flow.Clear();
        g->radiation.Clear();
      }
      return nullptr;
    }

    case SimMessageHash::SimData_InitializeFromCells:
      if (!g->world.InitializeFromCells(msg, len, g->elements, g->diseases)) {
        Report("SimData_InitializeFromCells rejected");
        return nullptr;
      }
      g->buffers.Allocate(g->world.GameCount());
      g->textures.Allocate(g->world.GameCount());
      g->first_frame = true;
      g->pipelined = false;
      ResetVisibility();
      g->skip_physics_frames = 1;
      g->first_physics_substep = true;
      return nullptr;

    case SimMessageHash::Load: {
      SaveBlob blob;
      std::string error;
      if (!DecodeSaveBlob(msg, len, &blob, &error)) {
        Report(("save blob rejected: " + error).c_str());
        return nullptr;  // the game turns a null return into a load failure
      }
      // A blob that covers the whole allocation at (0,0) is a saved game: reload through
      // `FromBlob`, as always. A smaller one is one world of a fresh cluster and goes down at
      // its own header `x, y` (`World::LoadIntoCluster`). One that does not fit falls back to
      // `FromBlob`; what Klei does there is not measured.
      const bool whole = !g->world.Allocated() ||
                         (blob.x == 0 && blob.y == 0 &&
                          blob.GameWidth() == g->world.GameWidth() &&
                          blob.GameHeight() == g->world.GameHeight());
      const bool fits = g->world.Allocated() && blob.x >= 0 && blob.y >= 0 &&
                        blob.x + blob.GameWidth() <= g->world.GameWidth() &&
                        blob.y + blob.GameHeight() <= g->world.GameHeight();
      const bool ok = (!whole && fits)
                          ? g->world.LoadIntoCluster(blob, g->elements, g->diseases, &error)
                          : g->world.FromBlob(blob, g->elements, g->diseases, &error);
      if (!ok) {
        Report(("save blob rejected: " + error).c_str());
        return nullptr;
      }
      g->buffers.Allocate(g->world.GameCount());
      g->textures.Allocate(g->world.GameCount());
      g->first_frame = true;
      g->pipelined = false;
      ResetVisibility();
      g->skip_physics_frames = 1;
      g->first_physics_substep = true;
      // Non-null means accepted. The game only tests for null, and there is no frame to
      // hand it — this is the empty publication the next `PrepareGameData` will overwrite.
      return &g->pub[g->pub_slot].update;
    }

    case SimMessageHash::ClearUnoccupiedCells: {
      // A fresh cluster sends this before its per-world `Load`s, and the gaps between worlds
      // are never written by any of them. Klei leaves a gap cell as Vacuum with no mass or
      // heat, backwall included (driver/src/worldgen_test against Klei's DLL). Allocate's
      // zeroed cells would otherwise be element index 0, which is a real element.
      if (!g->world.Allocated()) return nullptr;
      const uint16_t vacuum = g->elements.VacuumIndex();
      const size_t n = g->world.PaddedCount();
      for (size_t i = 0; i < n; ++i) {
        g->world.Phase(i) = {vacuum, 0.0f, 0.0f};
        g->world.SetRadiation(i, 0.0f);
        g->world.MutableBackwall(i) = {World::kVacuumHash, 0.0f, 0.0f};
      }
      return nullptr;
    }

    case SimMessageHash::Start: {
      if (!g->world.Allocated()) {
        Report("Start before the world was allocated");
        return nullptr;
      }
      g->started = true;
      ClearFrameEvents();
      Project(g->world, g->elements, &g->buffers, &g->projection_events, true);
      // The property textures are deliberately *not* filled here. Klei's are still all
      // zero after Start and only get populated by PrepareGameData — filling them early
      // showed up as a tick-0 divergence in every liquid cell.
      //
      // The backwall is the same story and was found the same way, once a scenario finally
      // sent a non-empty `SimBackwall`: Klei's Start update carries 0xFFFF for every
      // backwall element and zero mass and temperature, and only the first real frame fills
      // them in. Undoing the copy `Project` just made is cheaper than teaching it a mode,
      // and the dirty-all makes frame 1 redo it.
      g->buffers.backwall_element.assign(g->buffers.backwall_element.size(), 0xFFFF);
      g->buffers.backwall_mass.assign(g->buffers.backwall_mass.size(), 0.0f);
      g->buffers.backwall_temperature.assign(g->buffers.backwall_temperature.size(), 0.0f);
      g->world.MarkStaticDirtyAll();
      g->first_frame = false;
      // Start publishes on this thread and puts nothing in flight, so the first
      // `PrepareGameData` after it primes the pipeline.
      g->pipelined = false;
      ResetVisibility();
      return BuildUpdate(0);
    }

    case SimMessageHash::SimFrameManager_NewGameFrame: {
      // The payload is `sizeof(NewGameFrame) * activeRegions.Count`, not one struct: the
      // game sends one region per active area and a multi-asteroid world sends several.
      // Reading only the first would leave every asteroid but one frozen.
      const size_t count = len / sizeof(NewGameFrame);
      if (count == 0) return nullptr;
      std::vector<int32_t> regions;
      std::vector<float> cosmic;
      regions.reserve(count * 4);
      cosmic.reserve(count);
      for (size_t i = 0; i < count; ++i) {
        NewGameFrame f{};
        memcpy(&f, msg + i * sizeof(NewGameFrame), sizeof(f));
        if (i == 0) g->elapsed_seconds = f.elapsedSeconds;
        regions.push_back(f.minX);
        regions.push_back(f.minY);
        regions.push_back(f.maxX);
        regions.push_back(f.maxY);
        // The sixth int of Klei's `activeRegions` record. Ignored until the radiation field
        // went in, because nothing read it.
        cosmic.push_back(f.currentCosmicRadiationIntensity);
      }
      g->world.SetActiveRegions(regions.data(), count, cosmic.data());
      return nullptr;
    }

    case SimMessageHash::PrepareGameData: {
      // The pipeline. `docs/THREADING.md`: the frame handed back here is the one the worker
      // started at the *previous* `PrepareGameData` — which is Klei's arrangement, and is why
      // the rotation below sits on this side of the boundary rather than inside the frame.
      // The first call after a Start, an Alloc or a Load has nothing in flight and runs its
      // frame here, on this thread, exactly as the whole sim used to.
      GameDataUpdate* out = nullptr;
      if (g_worker.enabled && g->pipelined) {
        out = g->last_published;
      } else {
        ReleaseConduitHandlesForNextFrame();
        out = RunFrame();
        g->pipelined = true;
      }
      // The payload is the game's visibility mask, one byte per game cell. It is copied into
      // the game-side buffer AFTER the frame, as `GameSync` does once the sim thread has
      // finished one, and the pair then trades places. See `Sim::visible`.
      if (msg != nullptr && len > 0) {
        std::vector<uint8_t>& dst = g->visible_game[1 - g->visible_sim_slot];
        dst.assign(msg, msg + static_cast<size_t>(len));
        g->visible_sim_slot = 1 - g->visible_sim_slot;
      }
      if (out == nullptr) return nullptr;
      // A message sent during this tick takes effect one frame later: the next frame drains
      // `active`, and `active` is what `queue` held when the frame before it was collected.
      // Same rotation as before, one thread further out.
      g->active.swap(g->queue);
      g->queue.clear();
      if (g_worker.enabled) {
        ReleaseConduitHandlesForNextFrame();
        Kick();
      }
      return out;
    }

    // Immediate, not queued: it changes nothing about the world, and a toggle that only took
    // effect on the next frame would time a frame the player did not ask for.
    case SimMessageHash::ToggleProfiler:
      ToggleProfiler();
      return nullptr;

    default:
      NoteUnknown(sim_msg_id);
      return nullptr;
  }
}

__declspec(dllexport) void* SIM_HandleMessage(int sim_msg_id, int msg_length,
                                              const uint8_t* msg) {
  if (!g) return nullptr;
  const size_t len = msg_length > 0 ? static_cast<size_t>(msg_length) : 0;
  // The fast path, and the only one that must not block: it touches the game thread's own
  // queue and nothing else.
  if (QueueDeferredMessage(sim_msg_id, len, msg)) return nullptr;
  WaitIdle();
  return HandleImmediateMessage(sim_msg_id, len, msg);
}

__declspec(dllexport) void* SIM_HandleMessages(int sim_msg_id, int msg_length,
                                               int msg_count, const uint8_t* msg) {
  if (!g || msg_count <= 0) return nullptr;
  void* last = nullptr;
  for (int i = 0; i < msg_count; ++i) {
    last = SIM_HandleMessage(sim_msg_id, msg_length,
                             msg + static_cast<size_t>(i) * msg_length);
  }
  return last;
}

__declspec(dllexport) uint8_t* SIM_BeginSave(int* size, int x, int y) {
  WaitIdle();
  if (!g || !g->world.Allocated()) {
    if (size) *size = 0;
    return nullptr;
  }
  g->save_blob = EncodeSaveBlob(g->world.ToBlob(x, y, g->elements));
  if (size) *size = static_cast<int>(g->save_blob.size());
  return g->save_blob.data();
}

__declspec(dllexport) void SIM_EndSave() {
  if (g) g->save_blob.clear();
}

// Not part of Klei's ABI — the game never calls it. `diffsim` uses it to read the random
// stream's position out of both sims and compare draw counts directly, which is the only
// way to tell "we take the wrong number of draws" apart from "we take the right number in
// the wrong places". Klei keeps the same word in its own sim state.
__declspec(dllexport) uint32_t SIM_DebugRandomState() {
  WaitIdle();
  return g ? g->world.RandomState() : 0u;
}

// Also not part of Klei's ABI, and there is no Klei-side equivalent to read: this is the
// conservation ledger, which is a check against the sim's
// own arithmetic rather than against the other sim.
//
// A flat array of doubles rather than the struct, so the driver does not have to include
// `world.h` to read it. The order below is the contract, and `kLedgerFields` in
// diffsim.cpp is the other half of it — append only, never reorder. Returns the number of
// fields the DLL knows about, so a driver built against a longer list can tell.
//
// Field 0 is the only one that costs anything: it walks the grid. The rest are counters
// the sim has been keeping all along.
__declspec(dllexport) int SIM_DebugLedger(double* out, int count) {
  WaitIdle();
  const int fields = 14;
  if (!out || !g) return fields;
  const World::Ledger& l = g->world.Books();
  const double v[fields] = {g->world.TotalGridMass(),
                            l.emitted,
                            l.modified,
                            l.consumed,
                            l.dug,
                            l.ore,
                            l.unstable,
                            l.sublimated,
                            l.wisp,
                            l.thin_liquid,
                            l.cleared,
                            l.component_consumed,
                            l.component_emitted,
                            l.emitter_ore};
  const int n = count < fields ? count : fields;
  for (int i = 0; i < n; ++i) out[i] = v[i];
  return fields;
}

__declspec(dllexport) void SIM_DebugCrash() {
  volatile int* p = nullptr;
  *p = 0;
}

__declspec(dllexport) char* SYSINFO_Acquire() {
  static char info[] = "oni-sim-vanilla";
  return info;
}

__declspec(dllexport) void SYSINFO_Release() {}

// kprofiler: the sixteen exports `Klei/KProfilerPlugin.cs` declares, and the profiler behind
// them, `sim/kprofiler.h`.
//
// Klei's SimDLL exports 32 names and the game declares all 32 as `[DllImport("SimDLL")]`.
// Sixteen are the sim ABI above. The other sixteen are these, and nothing in the SHIPPED game
// ever reaches them: every call site is inside a `KProfiler` method marked
// `[Conditional("ENABLE_KPROFILER")]` and gated on `KProfilerPlugin.Initialized`, which is
// never assigned anywhere in `Assembly-CSharp` or `-firstpass`, whose `InitModule()` is an
// empty body.
//
// THESE ARE REAL, NOT NO-OP STUBS, for the reason this repo exists: **Klei's DLL implements
// them**. A stub where vanilla has a working profiler is a divergence from vanilla, whatever
// the shipped game does or does not call, and this project's whole claim is that the
// replacement behaves as Klei's does.
//
// The obvious objection still holds and is answered: nothing may run
// inside the game's frame. While no capture is running, every recording call below returns on
// one relaxed atomic load — `kprof::Running()` — and the kernel scopes above cost exactly that
// load each. A capture only does work once something has both loaded the plugin and started
// profiling, which the shipped game cannot do without a mod that does it on purpose.
//
// The signatures are the managed declarations, which are what the marshaller actually pushes:
// `string` under the default `CharSet.Ansi` is `const char*`, `ulong` is `uint64_t`, `long` is
// `int64_t`.
__declspec(dllexport) void kprofiler_load_plugin() { kprof::LoadPlugin(); }
__declspec(dllexport) void kprofiler_unload_plugin() { kprof::UnloadPlugin(); }
__declspec(dllexport) void kprofiler_start_http_control_listener(int port) {
  kprof::StartHttpControlListener(port);
}
__declspec(dllexport) void kprofiler_start_http_data_sender(int port) {
  kprof::ReplaceBroadcaster(new kprof::HttpBroadcaster(port));
}
__declspec(dllexport) void kprofiler_start_file_data_sender(const char* filename) {
  kprof::ReplaceBroadcaster(new kprof::FileBroadcaster(filename));
}
__declspec(dllexport) void kprofiler_flush_data_sender() { kprof::FlushDataSender(); }
__declspec(dllexport) void kprofiler_stop_data_sender() { kprof::ReplaceBroadcasterQuietly(nullptr); }
__declspec(dllexport) void kprofiler_start_profiling() { kprof::StartProfiling(); }
__declspec(dllexport) void kprofiler_stop_profiling(int broadcast_info) {
  kprof::StopProfiling(broadcast_info != 0);
}
__declspec(dllexport) uint64_t kprofile_record_string(const char* str) {
  return str == nullptr ? 0 : kprof::G().strings.Record(str);
}
__declspec(dllexport) uint64_t kprofiler_get_thread_uid() { return kprof::ThreadUid(); }
__declspec(dllexport) void kprofiler_set_thread_info(uint64_t thread_id, uint64_t name,
                                                     uint64_t category) {
  kprof::SetThreadInfo(thread_id, name, category);
}
__declspec(dllexport) void kprofiler_begin_section(uint64_t name, uint64_t category,
                                                   int64_t gc_alloc_count) {
  kprof::BeginSection(name, category, gc_alloc_count);
}
__declspec(dllexport) void kprofiler_end_section(int64_t gc_alloc_count) {
  kprof::EndSection(gc_alloc_count);
}
__declspec(dllexport) void kprofiler_ping(uint64_t name, uint64_t category, double value) {
  kprof::Ping(name, category, value);
}
__declspec(dllexport) void kprofiler_counter(uint64_t name, double value) {
  kprof::Counter(name, value);
}

// ConduitTemperatureManager, `sim/conduits.h`. These run on the *game* thread between sim
// frames, not inside a frame, which is why they touch `g->conduits` directly instead of
// queueing a message the way every cell-facing export does.
__declspec(dllexport) void ConduitTemperatureManager_Initialize() {
  if (g) g->conduits.Clear();
}
__declspec(dllexport) void ConduitTemperatureManager_Shutdown() {
  if (g) g->conduits.Clear();
}

__declspec(dllexport) int ConduitTemperatureManager_Add(
    float contents_temperature, float contents_mass, int contents_element_hash,
    int conduit_structure_temperature_handle, float conduit_heat_capacity,
    float conduit_thermal_conductivity, int32_t conduit_insulated) {
  if (!g) return 0;
  return g->conduits.Add(g->elements, contents_temperature, contents_mass,
                         contents_element_hash, conduit_structure_temperature_handle,
                         conduit_heat_capacity, conduit_thermal_conductivity,
                         conduit_insulated != 0);
}

__declspec(dllexport) void ConduitTemperatureManager_Remove(int handle) {
  if (g) g->conduits.Remove(handle);
}

// Klei returns the handle it was given rather than a new one; the game ignores the result.
__declspec(dllexport) int ConduitTemperatureManager_Set(int handle, float contents_temperature,
                                                        float contents_mass,
                                                        int contents_element_hash) {
  if (g) {
    g->conduits.Set(g->elements, handle, contents_temperature, contents_mass,
                    contents_element_hash);
  }
  return handle;
}

__declspec(dllexport) void ConduitTemperatureManager_Clear() {
  if (g) g->conduits.Clear();
}

__declspec(dllexport) void* ConduitTemperatureManager_Update(float dt,
                                                             void* building_conductivity_data) {
  if (!g) return nullptr;
  // The energy owed to each building goes onto the same queue the game's own messages land
  // on, in the order the conduits are walked, and is drained by the next frame. Applying it
  // here would let a building be warmed in the middle of a frame it is already part of.
  const auto sink = [](void* ctx, const ModifyBuildingEnergyMessage& m) {
    Sim* s = static_cast<Sim*>(ctx);
    Sim::Pending p;
    p.id = static_cast<int32_t>(SimMessageHash::ModifyBuildingEnergy);
    p.payload.resize(sizeof(m));
    memcpy(p.payload.data(), &m, sizeof(m));
    s->queue.push_back(std::move(p));
  };
  const auto* temperatures = static_cast<const BuildingTemperatureInfo*>(building_conductivity_data);
  return const_cast<ConduitTemperatureUpdateData*>(
      g->conduits.Update(dt, temperatures, sink, g));
}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
