// Replay a shim-captured corpus into the real SimDLL, with no game process.
//
// The corpus is the exact bytes the game sent, so this removes hand-reconstruction of
// payloads from the loop entirely -- which is what was blocking driver.cpp's tick step.
//
// Record format (little-endian, concatenated), written by shim/src/shim.cpp:
//   int32 seq | int32 msg_id | int32 length | int32 count | length*count bytes
//
// Usage:
//   replay.exe <corpus.bin> [--dll <path>] [--stop-after <seq>] [--quiet]

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../abi/sim_abi.h"

using namespace oni_sim;

namespace {

HMODULE g_sim = nullptr;

using SIM_Initialize_t = void (*)(int (*)(int, void*));
using SIM_Shutdown_t = void (*)();
using SIM_HandleMessage_t = void* (*)(int, int, const uint8_t*);
using SIM_HandleMessages_t = void* (*)(int, int, int, const uint8_t*);

SIM_Initialize_t sim_initialize;
SIM_Shutdown_t sim_shutdown;
SIM_HandleMessage_t sim_handle_message;
SIM_HandleMessages_t sim_handle_messages;

int GameMessageHandler(int message_id, void* data) {
  const char* text = data ? *reinterpret_cast<const char* const*>(data) : nullptr;
  printf("  [sim->game] id=%d %s\n", message_id, text ? text : "(no text)");
  return 0;
}

bool Bind(const char* dll) {
  g_sim = LoadLibraryA(dll);
  if (!g_sim) {
    printf("LoadLibrary failed for %s (err %lu)\n", dll, GetLastError());
    return false;
  }
  sim_initialize = (SIM_Initialize_t)GetProcAddress(g_sim, "SIM_Initialize");
  sim_shutdown = (SIM_Shutdown_t)GetProcAddress(g_sim, "SIM_Shutdown");
  sim_handle_message = (SIM_HandleMessage_t)GetProcAddress(g_sim, "SIM_HandleMessage");
  sim_handle_messages = (SIM_HandleMessages_t)GetProcAddress(g_sim, "SIM_HandleMessages");
  return sim_initialize && sim_shutdown && sim_handle_message && sim_handle_messages;
}

constexpr int32_t kStart = -931446686;
constexpr int32_t kPrepareGameData = 1078620451;

const char* NameOf(int32_t id) {
  switch (id) {
    case 1108437482: return "Elements_CreateTable";
    case 825301935:  return "Disease_CreateTable";
    case 1092408308: return "AllocateCells";
    case -1836204275:return "ClearUnoccupiedCells";
    case -895846551: return "DefineWorldOffsets";
    case -457308393: return "SetWorldZones";
    case 2062421945: return "SimData_InitializeFromCells";
    case -672538170: return "Load";
    case kStart:     return "Start";
    case kPrepareGameData: return "PrepareGameData";
    case -775326397: return "NewGameFrame";
    case -1683118492:return "SetDebugProperties";
    default: return nullptr;
  }
}

struct Record {
  int32_t seq, id, length, count;
  std::vector<uint8_t> payload;
};

bool ReadCorpus(const char* path, std::vector<Record>* out) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    printf("cannot open corpus %s\n", path);
    return false;
  }
  for (;;) {
    int32_t header[4];
    if (fread(header, sizeof(header), 1, f) != 1) break;
    Record r{header[0], header[1], header[2], header[3], {}};
    const size_t bytes =
        static_cast<size_t>(r.length) * (r.count > 0 ? r.count : 1);
    r.payload.resize(bytes);
    if (bytes && fread(r.payload.data(), 1, bytes, f) != bytes) {
      printf("truncated record seq=%d id=%d, stopping\n", r.seq, r.id);
      break;
    }
    out->push_back(std::move(r));
  }
  fclose(f);
  return !out->empty();
}

void DumpCells(const GameDataUpdate* g, int width, int x, int rows) {
  printf("  column x=%d (bottom-up): ", x);
  for (int y = 0; y < rows; ++y) {
    const int cell = y * width + x;
    printf("[%u %.1fkg %.1fK] ", g->elementIdx[cell], g->mass[cell],
           g->temperature[cell]);
  }
  printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  const char* corpus_path = nullptr;
  const char* dll = "SimDLL_orig.dll";
  int32_t stop_after = -1;
  bool quiet = false;
  // Parse and summarise the corpus without loading the sim. Lets the record format be
  // verified independently of a game run, and makes a corpus inspectable on its own.
  bool dry_run = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--dll") && i + 1 < argc) dll = argv[++i];
    else if (!strcmp(argv[i], "--stop-after") && i + 1 < argc) stop_after = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--quiet")) quiet = true;
    else if (!strcmp(argv[i], "--dry-run")) dry_run = true;
    else corpus_path = argv[i];
  }
  if (!corpus_path) {
    printf("usage: replay.exe <corpus.bin> [--dll <path>] [--stop-after <seq>]"
           " [--quiet] [--dry-run]\n");
    return 2;
  }

  std::vector<Record> records;
  if (!ReadCorpus(corpus_path, &records)) return 1;
  printf("corpus: %zu records from %s\n", records.size(), corpus_path);

  if (dry_run) {
    uint64_t total = 0;
    for (const Record& r : records) {
      const char* name = NameOf(r.id);
      printf("[%4d] %-28s id=%-12d len=%-9d count=%d\n", r.seq,
             name ? name : "(unknown)", r.id, r.length, r.count);
      total += r.payload.size();
    }
    printf("\n%zu records, %llu payload bytes\n", records.size(),
           static_cast<unsigned long long>(total));
    return 0;
  }

  if (!Bind(dll)) return 1;
  printf("bound %s\n", dll);
  sim_initialize(&GameMessageHandler);

  // The grid width is needed to index cells for the dump; AllocateCells carries it as
  // the first int32 of its payload.
  int width = 0, height = 0;
  const GameDataUpdate* latest = nullptr;
  int prepares = 0;

  for (const Record& r : records) {
    if (stop_after >= 0 && r.seq > stop_after) break;

    // Both AllocateCells and SimData_InitializeFromCells open with int32 width,
    // int32 height. Worldgen sends only the latter -- there is no AllocateCells in a
    // worldgen boot -- so take the dimensions from whichever arrives.
    if ((r.id == 1092408308 || r.id == 2062421945) && r.payload.size() >= 8) {
      memcpy(&width, r.payload.data(), 4);
      memcpy(&height, r.payload.data() + 4, 4);
      printf("  grid %dx%d (%d cells) from %s\n", width, height, width * height,
             NameOf(r.id));
    }

    const char* name = NameOf(r.id);
    if (!quiet) {
      printf("[%4d] %-28s len=%-8d count=%d\n", r.seq,
             name ? name : "(id)", r.length, r.count);
      if (!name) printf("       id=%d\n", r.id);
    }

    const uint8_t* p = r.payload.empty() ? nullptr : r.payload.data();
    void* result = (r.count > 1)
        ? sim_handle_messages(r.id, r.length, r.count, p)
        : sim_handle_message(r.id, r.length, p);

    if (r.id == kStart || r.id == kPrepareGameData) {
      if (!result) {
        printf("  FAILED: seq %d (%s) returned null\n", r.seq,
               name ? name : "?");
        sim_shutdown();
        return 1;
      }
      latest = static_cast<const GameDataUpdate*>(result);
      if (r.id == kPrepareGameData) ++prepares;
      if (width > 0 && (r.id == kStart || prepares == 1)) {
        printf("  -> GameDataUpdate frames=%d\n", latest->numFramesProcessed);
        DumpCells(latest, width, width / 2, 8);
      }
    }
  }

  if (latest && width > 0) {
    printf("\nreplayed %zu records, %d PrepareGameData ticks survived\n",
           records.size(), prepares);
    printf("final numFramesProcessed=%d\n", latest->numFramesProcessed);
    DumpCells(latest, width, width / 2, 8);
  } else {
    printf("\nreplayed %zu records (no GameDataUpdate observed)\n", records.size());
  }

  sim_shutdown();
  printf("SIM_Shutdown ok\n");
  return 0;
}
