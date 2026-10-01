// Guard: the replacement's export table must cover Klei's, and every export must be
// reachable by name.
//
// This exists because a documented "16 exports, confirmed identical" stood for months
// while the DLL was missing sixteen of Klei's. The confirmation had been done against
// `Sim.cs` alone, and `Klei/KProfilerPlugin.cs` declares the rest; every later pass
// inherited the number instead of re-deriving it. A documented count is not a check.
//
// Two things are checked, and the second is not implied by the first:
//
//   1. **Coverage.** Every name Klei exports, we export. Read from the two PE export
//      directories, so the comparison is against the stock DLL on disk rather than
//      against a list in this file that could go stale the same way the doc did.
//   2. **Reachability.** Every `kprofiler` entry point resolves through `GetProcAddress`
//      and can actually be called, in the order `KProfiler.cs` would call them. Linking
//      is not evidence of this: a symbol can be built and still be absent from the export
//      table, which is exactly the failure being guarded against.
//
// Proved to fail on purpose: run it with `--mine ..\shim\build\SimDLL.dll`. The shim
// forwards the sixteen sim exports and has never had the `kprofiler` ones, so it reports
// all sixteen missing and exits 1. Note what that control depends on — if the shim is ever
// taught to forward all thirty-two (it has the same drop-in gap, and it is installed over
// the game's DLL the same way), this stops failing and the proof needs another subject.
//
// Usage:
//   exports.exe [--mine <dll>] [--klei <dll>]
//
// `--mine` defaults to the built library. `--klei` defaults to `../vendor-backup/SimDLL.dll`,
// a copy of the game's DLL kept outside the game directory so that it is the stock DLL
// whether or not the shim is installed; make that copy, or pass the path.

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// The sixteen the game declares in `Klei/KProfilerPlugin.cs`, with the signatures the
// marshaller pushes: `string` under the default `CharSet.Ansi` is `const char*`, `ulong`
// is `uint64_t`, `long` is `int64_t`.
struct Kprofiler {
  void (*load_plugin)();
  void (*unload_plugin)();
  void (*start_http_control_listener)(int);
  void (*start_http_data_sender)(int);
  void (*start_file_data_sender)(const char*);
  void (*flush_data_sender)();
  void (*stop_data_sender)();
  void (*start_profiling)();
  void (*stop_profiling)(int);
  uint64_t (*record_string)(const char*);
  uint64_t (*get_thread_uid)();
  void (*set_thread_info)(uint64_t, uint64_t, uint64_t);
  void (*begin_section)(uint64_t, uint64_t, int64_t);
  void (*end_section)(int64_t);
  void (*ping)(uint64_t, uint64_t, double);
  void (*counter)(uint64_t, double);
};

// Read the export name table straight out of the mapped image. `DONT_RESOLVE_DLL_REFERENCES`
// maps the module with its section layout applied — so RVAs are offsets from the base — but
// does not run `DllMain` or bind imports, which matters because one of the DLLs this is
// pointed at is the shim, whose `DllMain` would go looking for `SimDLL_orig.dll`.
bool ReadExportNames(const char* path, std::vector<std::string>* out) {
  HMODULE h = LoadLibraryExA(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (!h) {
    printf("  LoadLibraryEx failed for %s (err %lu)\n", path, GetLastError());
    return false;
  }
  const auto* base = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(h) &
                                                      ~static_cast<uintptr_t>(3));
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    printf("  %s: not a PE image\n", path);
    FreeLibrary(h);
    return false;
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    printf("  %s: bad NT signature\n", path);
    FreeLibrary(h);
    return false;
  }
  const IMAGE_DATA_DIRECTORY& dir =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (dir.VirtualAddress == 0) {
    printf("  %s: no export directory\n", path);
    FreeLibrary(h);
    return false;
  }
  const auto* exp =
      reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
  const auto* names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
  for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
    out->push_back(reinterpret_cast<const char*>(base + names[i]));
  }
  std::sort(out->begin(), out->end());
  FreeLibrary(h);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const char* mine_dll = "..\\sim\\build\\SimDLL.dll";
  const char* klei_dll = "..\\vendor-backup\\SimDLL.dll";
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--mine") && i + 1 < argc) mine_dll = argv[++i];
    else if (!strcmp(argv[i], "--klei") && i + 1 < argc) klei_dll = argv[++i];
    else {
      printf("usage: exports.exe [--mine <dll>] [--klei <dll>]\n");
      return 2;
    }
  }

  printf("mine: %s\nklei: %s\n\n", mine_dll, klei_dll);

  std::vector<std::string> mine, klei;
  if (!ReadExportNames(mine_dll, &mine) || !ReadExportNames(klei_dll, &klei)) return 2;
  printf("exports: mine %zu, klei %zu\n", mine.size(), klei.size());

  // 1. Coverage.
  std::vector<std::string> missing, extra;
  std::set_difference(klei.begin(), klei.end(), mine.begin(), mine.end(),
                      std::back_inserter(missing));
  std::set_difference(mine.begin(), mine.end(), klei.begin(), klei.end(),
                      std::back_inserter(extra));
  for (const std::string& n : extra) printf("  ours only: %s\n", n.c_str());
  for (const std::string& n : missing) printf("  MISSING:   %s\n", n.c_str());
  if (!missing.empty()) {
    printf("\nFAILED: %zu of Klei's exports are not in ours\n", missing.size());
    return 1;
  }
  printf("  coverage: every one of Klei's %zu exports is present\n", klei.size());

  // 2. Reachability. A real load this time: the point is to call them.
  HMODULE h = LoadLibraryA(mine_dll);
  if (!h) {
    printf("\nLoadLibrary failed for %s (err %lu)\n", mine_dll, GetLastError());
    return 2;
  }
  int unresolved = 0;
  auto get = [&](const char* n) -> FARPROC {
    FARPROC p = GetProcAddress(h, n);
    if (!p) {
      printf("  UNRESOLVED: %s\n", n);
      ++unresolved;
    }
    return p;
  };
  Kprofiler k{};
  k.load_plugin = reinterpret_cast<void (*)()>(get("kprofiler_load_plugin"));
  k.unload_plugin = reinterpret_cast<void (*)()>(get("kprofiler_unload_plugin"));
  k.start_http_control_listener =
      reinterpret_cast<void (*)(int)>(get("kprofiler_start_http_control_listener"));
  k.start_http_data_sender =
      reinterpret_cast<void (*)(int)>(get("kprofiler_start_http_data_sender"));
  k.start_file_data_sender =
      reinterpret_cast<void (*)(const char*)>(get("kprofiler_start_file_data_sender"));
  k.flush_data_sender = reinterpret_cast<void (*)()>(get("kprofiler_flush_data_sender"));
  k.stop_data_sender = reinterpret_cast<void (*)()>(get("kprofiler_stop_data_sender"));
  k.start_profiling = reinterpret_cast<void (*)()>(get("kprofiler_start_profiling"));
  k.stop_profiling = reinterpret_cast<void (*)(int)>(get("kprofiler_stop_profiling"));
  k.record_string =
      reinterpret_cast<uint64_t (*)(const char*)>(get("kprofile_record_string"));
  k.get_thread_uid = reinterpret_cast<uint64_t (*)()>(get("kprofiler_get_thread_uid"));
  k.set_thread_info = reinterpret_cast<void (*)(uint64_t, uint64_t, uint64_t)>(
      get("kprofiler_set_thread_info"));
  k.begin_section = reinterpret_cast<void (*)(uint64_t, uint64_t, int64_t)>(
      get("kprofiler_begin_section"));
  k.end_section = reinterpret_cast<void (*)(int64_t)>(get("kprofiler_end_section"));
  k.ping =
      reinterpret_cast<void (*)(uint64_t, uint64_t, double)>(get("kprofiler_ping"));
  k.counter = reinterpret_cast<void (*)(uint64_t, double)>(get("kprofiler_counter"));
  if (unresolved) {
    printf("\nFAILED: %d kprofiler entry points did not resolve\n", unresolved);
    FreeLibrary(h);
    return 1;
  }

  // The order `KProfiler.cs` would use them in.
  k.load_plugin();
  k.start_http_control_listener(9420);
  k.start_http_data_sender(9421);
  k.start_file_data_sender("KProfiler.log");
  k.start_profiling();
  const uint64_t name = k.record_string("SimUpdate");
  const uint64_t category = k.record_string("Sim");
  const uint64_t tid = k.get_thread_uid();
  k.set_thread_info(tid, name, category);
  k.begin_section(name, category, -1);
  k.counter(name, 3.5);
  k.ping(name, category, 1.25);
  k.end_section(-1);
  k.flush_data_sender();
  k.stop_data_sender();
  k.stop_profiling(1);
  k.unload_plugin();
  printf("  reachability: all 16 kprofiler entry points resolved and called"
         " (record_string -> %llu, thread_uid -> %llu)\n",
         static_cast<unsigned long long>(name), static_cast<unsigned long long>(tid));

  FreeLibrary(h);
  printf("\nOK\n");
  return 0;
}
