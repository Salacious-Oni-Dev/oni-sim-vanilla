// kproftest: the kprofiler exports (`sim/kprofiler.h`), exercised against any SimDLL and
// printed in a form that does not depend on time or thread ids, so Klei's DLL and ours can be
// diffed line for line.
//
//   build/kproftest.exe <path to the game's SimDLL.dll> > klei.txt
//   build/kproftest.exe ..\sim\build\SimDLL.dll     > ours.txt
//   diff klei.txt ours.txt
//
// Three captures, each a scripted sequence of sections, pings and counters on two threads:
//
//   file    kprofiler_start_file_data_sender -> the stream file, header and records
//   http    the control listener's /ping /start /syncstrings /stop, status and body
//   post    kprofiler_start_http_data_sender -> the bodies POSTed to /datablock, received by
//           a listener this program runs
//
// Every record is printed with its type, its size field, the bytes it occupies and its
// payload, with timestamps reduced to "increasing or not" and thread uids replaced by the
// order they first appear. The strings a StringsBlock carries are printed sorted. Checks that
// hold for any correct implementation (the header hash, the string ids being FNV-1a, sections
// balancing, timestamps not going backwards on a thread) print FAIL lines, and the exit code
// counts them.
//
// Nothing here launches the game: the DLL is loaded into this process and none of the sim is
// initialised, which the kprofiler exports do not need.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct Api {
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

Api api;
int failures = 0;

void Fail(const std::string& what) {
  printf("FAIL %s\n", what.c_str());
  ++failures;
}

uint64_t Fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

uint64_t Fnv1a(const std::string& s) { return Fnv1a(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

bool Bind(HMODULE dll) {
  bool ok = true;
  auto get = [&](const char* name) -> FARPROC {
    FARPROC f = GetProcAddress(dll, name);
    if (f == nullptr) {
      printf("missing export %s\n", name);
      ok = false;
    }
    return f;
  };
  api.load_plugin = reinterpret_cast<void (*)()>(get("kprofiler_load_plugin"));
  api.unload_plugin = reinterpret_cast<void (*)()>(get("kprofiler_unload_plugin"));
  api.start_http_control_listener = reinterpret_cast<void (*)(int)>(get("kprofiler_start_http_control_listener"));
  api.start_http_data_sender = reinterpret_cast<void (*)(int)>(get("kprofiler_start_http_data_sender"));
  api.start_file_data_sender = reinterpret_cast<void (*)(const char*)>(get("kprofiler_start_file_data_sender"));
  api.flush_data_sender = reinterpret_cast<void (*)()>(get("kprofiler_flush_data_sender"));
  api.stop_data_sender = reinterpret_cast<void (*)()>(get("kprofiler_stop_data_sender"));
  api.start_profiling = reinterpret_cast<void (*)()>(get("kprofiler_start_profiling"));
  api.stop_profiling = reinterpret_cast<void (*)(int)>(get("kprofiler_stop_profiling"));
  api.record_string = reinterpret_cast<uint64_t (*)(const char*)>(get("kprofile_record_string"));
  api.get_thread_uid = reinterpret_cast<uint64_t (*)()>(get("kprofiler_get_thread_uid"));
  api.set_thread_info = reinterpret_cast<void (*)(uint64_t, uint64_t, uint64_t)>(get("kprofiler_set_thread_info"));
  api.begin_section = reinterpret_cast<void (*)(uint64_t, uint64_t, int64_t)>(get("kprofiler_begin_section"));
  api.end_section = reinterpret_cast<void (*)(int64_t)>(get("kprofiler_end_section"));
  api.ping = reinterpret_cast<void (*)(uint64_t, uint64_t, double)>(get("kprofiler_ping"));
  api.counter = reinterpret_cast<void (*)(uint64_t, double)>(get("kprofiler_counter"));
  return ok;
}

// ---------------------------------------------------------------- the scripted workload

DWORD WINAPI SecondThread(LPVOID) {
  api.set_thread_info(0, api.record_string("second thread"), api.record_string("test"));
  api.begin_section(api.record_string("Worker"), api.record_string("test"), 7);
  api.counter(api.record_string("items"), 3.0);
  api.end_section(8);
  return 0;
}

// The same sequence for every capture: a named main thread with nested sections, a ping and
// a counter, and a second thread with one section of its own.
void Workload() {
  api.set_thread_info(0, api.record_string("main thread"), api.record_string("test"));
  const uint64_t frame = api.record_string("Frame");
  const uint64_t category = api.record_string("test");
  api.begin_section(frame, category, 1);
  api.begin_section(api.record_string("Update"), category, 2);
  api.ping(api.record_string("marker"), category, 42.5);
  api.counter(api.record_string("cells"), 1234.0);
  api.end_section(3);
  api.begin_section(api.record_string("LateUpdate"), category, 4);
  api.end_section(5);
  api.end_section(6);
  HANDLE t = CreateThread(nullptr, 0, SecondThread, nullptr, 0, nullptr);
  WaitForSingleObject(t, INFINITE);
  CloseHandle(t);
}

// ---------------------------------------------------------------- reading a stream

struct Reader {
  const std::vector<uint8_t>& bytes;
  size_t at = 0;
  bool Has(size_t n) const { return at + n <= bytes.size(); }
  uint64_t U64() {
    uint64_t v = 0;
    memcpy(&v, bytes.data() + at, 8);
    at += 8;
    return v;
  }
  uint32_t U32() {
    uint32_t v = 0;
    memcpy(&v, bytes.data() + at, 4);
    at += 4;
    return v;
  }
  int64_t I64() { return static_cast<int64_t>(U64()); }
  double F64() {
    double v = 0;
    memcpy(&v, bytes.data() + at, 8);
    at += 8;
    return v;
  }
};

// Prints records until the bytes run out. Names are printed as the string their id is the
// FNV-1a of, when the workload recorded it.
void PrintRecords(const std::vector<uint8_t>& bytes, size_t start, const std::map<uint64_t, std::string>& names) {
  Reader r{bytes};
  r.at = start;
  std::map<uint64_t, int> threads;
  std::map<uint64_t, int64_t> last_time;
  std::map<uint64_t, int> depth;
  auto thread = [&](uint64_t uid) {
    auto it = threads.find(uid);
    if (it != threads.end()) return it->second;
    int n = static_cast<int>(threads.size());
    threads[uid] = n;
    return n;
  };
  auto name = [&](uint64_t id) {
    auto it = names.find(id);
    return it == names.end() ? std::string("?") : it->second;
  };
  auto time = [&](uint64_t uid, int64_t t) {
    auto it = last_time.find(uid);
    const bool ok = it == last_time.end() || t >= it->second;
    last_time[uid] = t;
    if (!ok) Fail("timestamp went backwards on thread " + std::to_string(thread(uid)));
    return ok ? "t+" : "t-";
  };
  while (r.Has(8)) {
    const size_t record_start = r.at;
    const uint32_t size = r.U32();
    const uint32_t type = r.U32();
    switch (type) {
      case 0: {
        const uint64_t n = r.U64(), c = r.U64();
        const int64_t t = r.I64();
        const uint64_t th = r.U64();
        const int64_t gc = r.I64();
        printf("  begin   size=%u name=%s category=%s thread=%d gc=%lld %s\n", size, name(n).c_str(),
               name(c).c_str(), thread(th), static_cast<long long>(gc), time(th, t));
        ++depth[th];
        break;
      }
      case 1: {
        const int64_t t = r.I64();
        const uint64_t th = r.U64();
        const int64_t gc = r.I64();
        printf("  end     size=%u thread=%d gc=%lld %s\n", size, thread(th), static_cast<long long>(gc), time(th, t));
        if (--depth[th] < 0) Fail("end without begin on thread " + std::to_string(thread(th)));
        break;
      }
      case 4: {
        const uint64_t n = r.U64(), c = r.U64();
        const int64_t t = r.I64();
        const uint64_t th = r.U64();
        const double v = r.F64();
        printf("  ping    size=%u name=%s category=%s thread=%d value=%g %s\n", size, name(n).c_str(),
               name(c).c_str(), thread(th), v, time(th, t));
        break;
      }
      case 5: {
        const uint64_t n = r.U64();
        const int64_t t = r.I64();
        const uint64_t th = r.U64();
        const double v = r.F64();
        printf("  counter size=%u name=%s thread=%d value=%g %s\n", size, name(n).c_str(), thread(th), v, time(th, t));
        break;
      }
      case 6: {
        const uint64_t data_size = r.U64();
        std::vector<std::string> strings;
        size_t i = r.at;
        const size_t end = r.at + data_size;
        while (i < end && i < bytes.size()) {
          std::string s(reinterpret_cast<const char*>(bytes.data() + i));
          strings.push_back(s);
          i += s.size() + 1;
        }
        r.at = end;
        std::sort(strings.begin(), strings.end());
        printf("  strings size=%u data=%llu occupies=%zu count=%zu\n", size, static_cast<unsigned long long>(data_size),
               r.at - record_start, strings.size());
        for (const std::string& s : strings) printf("    \"%s\"\n", s.c_str());
        break;
      }
      case 8: {
        const uint64_t n = r.U64(), c = r.U64(), th = r.U64();
        printf("  thread  size=%u name=%s category=%s thread=%d\n", size, name(n).c_str(), name(c).c_str(), thread(th));
        break;
      }
      default:
        printf("  unknown type=%u size=%u\n", type, size);
        Fail("unknown record type");
        r.at = record_start + (size >= 8 ? size : 8);
    }
  }
  for (const auto& d : depth) {
    if (d.second != 0) Fail("unbalanced sections on thread " + std::to_string(thread(d.first)));
  }
}

std::map<uint64_t, std::string> Names() {
  std::map<uint64_t, std::string> names;
  for (const char* s : {"Frame", "Update", "LateUpdate", "Worker", "marker", "cells", "items", "test",
                        "main thread", "second thread"}) {
    names[Fnv1a(s)] = s;
  }
  return names;
}

std::vector<uint8_t> ReadFile(const char* path) {
  std::vector<uint8_t> bytes;
  FILE* f = fopen(path, "rb");
  if (f == nullptr) return bytes;
  uint8_t chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) bytes.insert(bytes.end(), chunk, chunk + n);
  fclose(f);
  return bytes;
}

// ---------------------------------------------------------------- HTTP

struct HttpReply {
  int status = 0;
  std::string content_type;
  std::string body;
};

HttpReply Get(int port, const char* path) {
  HttpReply reply;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    closesocket(s);
    return reply;
  }
  char request[256];
  const int n = snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n\r\n", path, port);
  send(s, request, n, 0);
  std::string raw;
  char chunk[4096];
  int got;
  while ((got = recv(s, chunk, sizeof(chunk), 0)) > 0) raw.append(chunk, static_cast<size_t>(got));
  closesocket(s);
  const size_t head_end = raw.find("\r\n\r\n");
  if (raw.size() < 12 || head_end == std::string::npos) return reply;
  reply.status = atoi(raw.c_str() + 9);
  std::string head = raw.substr(0, head_end);
  std::string lower = head;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
  const size_t ct = lower.find("content-type:");
  if (ct != std::string::npos) {
    size_t v = ct + 13;
    while (v < head.size() && head[v] == ' ') ++v;
    reply.content_type = head.substr(v, head.find("\r\n", v) - v);
  }
  reply.body = raw.substr(head_end + 4);
  return reply;
}

void PrintReply(const char* path, const HttpReply& r) {
  printf("  GET %-12s status=%d content-type=\"%s\" bytes=%zu", path, r.status, r.content_type.c_str(), r.body.size());
  if (std::string(path) == "/ping") printf(" body=\"%s\"", r.body.c_str());
  printf("\n");
}

// A one-route server that keeps the body of every POST it is sent.
struct Sink {
  SOCKET server = INVALID_SOCKET;
  int port = 0;
  std::vector<uint8_t> received;
  int posts = 0;
  std::vector<std::string> paths;
  CRITICAL_SECTION lock;
};

DWORD WINAPI SinkMain(LPVOID arg) {
  Sink* sink = static_cast<Sink*>(arg);
  for (;;) {
    SOCKET c = accept(sink->server, nullptr, nullptr);
    if (c == INVALID_SOCKET) return 0;
    std::string raw;
    char chunk[65536];
    size_t head_end = std::string::npos;
    size_t length = 0;
    for (;;) {
      const int got = recv(c, chunk, sizeof(chunk), 0);
      if (got <= 0) break;
      raw.append(chunk, static_cast<size_t>(got));
      if (head_end == std::string::npos) {
        head_end = raw.find("\r\n\r\n");
        if (head_end != std::string::npos) {
          std::string lower = raw.substr(0, head_end);
          std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
          const size_t cl = lower.find("content-length:");
          if (cl != std::string::npos) length = static_cast<size_t>(atoll(lower.c_str() + cl + 15));
        }
      }
      if (head_end != std::string::npos && raw.size() >= head_end + 4 + length) break;
    }
    if (head_end != std::string::npos) {
      EnterCriticalSection(&sink->lock);
      const size_t sp = raw.find(' ');
      sink->paths.push_back(raw.substr(0, raw.find(' ', sp + 1)));
      sink->received.insert(sink->received.end(), raw.begin() + static_cast<long>(head_end) + 4, raw.end());
      ++sink->posts;
      LeaveCriticalSection(&sink->lock);
    }
    const char* ok = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send(c, ok, static_cast<int>(strlen(ok)), 0);
    shutdown(c, SD_SEND);
    closesocket(c);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: kproftest <SimDLL.dll> [port-base]\n");
    return 2;
  }
  setvbuf(stdout, nullptr, _IONBF, 0);  // a crash inside the DLL must not swallow what came before
  const int port_base = argc > 2 ? atoi(argv[2]) : 47110;
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  HMODULE dll = LoadLibraryA(argv[1]);
  if (dll == nullptr) {
    printf("cannot load %s (error %lu)\n", argv[1], GetLastError());
    return 2;
  }
  if (!Bind(dll)) return 2;
  const std::map<uint64_t, std::string> names = Names();

  printf("ids\n");
  for (const char* s : {"Frame", ""}) {
    const uint64_t id = api.record_string(s);
    printf("  record_string(\"%s\") %s FNV-1a\n", s, id == Fnv1a(s) ? "==" : "!=");
    if (id != Fnv1a(s)) Fail("string id is not FNV-1a");
  }
  {
    const DWORD tid = GetCurrentThreadId();
    const uint8_t b[4] = {static_cast<uint8_t>(tid), static_cast<uint8_t>(tid >> 8), static_cast<uint8_t>(tid >> 16),
                          static_cast<uint8_t>(tid >> 24)};
    const bool same = api.get_thread_uid() == Fnv1a(b, 4);
    printf("  get_thread_uid %s FNV-1a(GetCurrentThreadId)\n", same ? "==" : "!=");
    if (!same) Fail("thread uid is not FNV-1a of the thread id");
  }

  // Recording before load/start must write nothing and must not crash.
  api.begin_section(api.record_string("Frame"), 0, 0);
  api.end_section(0);

  api.load_plugin();

  // ------------------------------------------------------------ http control listener
  printf("http\n");
  const int control_port = port_base;
  api.start_http_control_listener(control_port);
  HttpReply ping0;
  for (int i = 0; i < 100 && ping0.status == 0; ++i) {
    Sleep(20);
    ping0 = Get(control_port, "/ping");
  }
  PrintReply("/ping", ping0);
  PrintReply("/start", Get(control_port, "/start"));
  PrintReply("/ping", Get(control_port, "/ping"));
  HttpReply strings = Get(control_port, "/syncstrings");
  PrintReply("/syncstrings", strings);
  if (strings.body.size() >= 16) {
    uint64_t tag = 0, total = 0;
    memcpy(&tag, strings.body.data(), 8);
    memcpy(&total, strings.body.data() + 8, 8);
    std::vector<std::string> list;
    for (size_t i = 16; i < strings.body.size();) {
      std::string s(strings.body.c_str() + i);
      list.push_back(s);
      i += s.size() + 1;
    }
    std::sort(list.begin(), list.end());
    printf("    tag=%016llx total=%llu matches-body=%s strings:", static_cast<unsigned long long>(tag),
           static_cast<unsigned long long>(total), total + 16 == strings.body.size() ? "yes" : "no");
    for (const std::string& s : list) printf(" \"%s\"", s.c_str());
    printf("\n");
  }
  PrintReply("/nope", Get(control_port, "/nope"));
  // /stop with no broadcaster attached returns at once in both.
  PrintReply("/stop", Get(control_port, "/stop"));
  PrintReply("/ping", Get(control_port, "/ping"));

  // ------------------------------------------------------------ file capture
  printf("file\n");
  char path[MAX_PATH];
  GetTempPathA(sizeof(path), path);
  strcat(path, "kproftest.kprof");
  api.start_file_data_sender(path);
  api.start_profiling();
  Workload();
  api.stop_profiling(1);
  api.stop_data_sender();
  {
    const std::vector<uint8_t> bytes = ReadFile(path);
    if (bytes.size() < 12) {
      Fail("stream file shorter than its header");
    } else {
      uint64_t magic = 0;
      uint32_t version = 0;
      memcpy(&magic, bytes.data(), 8);
      memcpy(&version, bytes.data() + 8, 4);
      const bool magic_ok = magic == Fnv1a("KPROFILER_EVENTSTREAM");
      printf("  header magic %s FNV-1a(\"KPROFILER_EVENTSTREAM\") version=%u\n", magic_ok ? "==" : "!=", version);
      if (!magic_ok) Fail("stream header");
      PrintRecords(bytes, 12, names);
    }
  }

  // A second session on the same file sender must not carry the first session's buffers.
  printf("file-second-session\n");
  // A separate file, so the full capture above is left for a trace converter to read.
  strcpy(strrchr(path, '.'), "-second.kprof");
  api.start_file_data_sender(path);
  api.start_profiling();
  api.begin_section(api.record_string("Frame"), api.record_string("test"), 9);
  api.end_section(10);
  api.flush_data_sender();
  api.stop_profiling(0);
  api.stop_data_sender();
  PrintRecords(ReadFile(path), 12, names);

  // ------------------------------------------------------------ http data sender
  printf("post\n");
  Sink sink;
  InitializeCriticalSection(&sink.lock);
  sink.port = port_base + 1;
  sink.server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(sink.port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(sink.server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(sink.server, SOMAXCONN);
  }
  HANDLE sink_thread = CreateThread(nullptr, 0, SinkMain, &sink, 0, nullptr);
  api.start_http_data_sender(sink.port);
  api.start_profiling();
  Workload();
  api.stop_profiling(1);
  api.stop_data_sender();
  Sleep(200);
  EnterCriticalSection(&sink.lock);
  printf("  posts=%d", sink.posts);
  std::sort(sink.paths.begin(), sink.paths.end());
  sink.paths.erase(std::unique(sink.paths.begin(), sink.paths.end()), sink.paths.end());
  for (const std::string& p : sink.paths) printf(" \"%s\"", p.c_str());
  printf("\n");
  const std::vector<uint8_t> posted = sink.received;
  LeaveCriticalSection(&sink.lock);
  PrintRecords(posted, 0, names);

  api.unload_plugin();
  closesocket(sink.server);
  WaitForSingleObject(sink_thread, 2000);

  printf("failures=%d\n", failures);
  return failures;
}
