// kprofiler: Klei's in-DLL profiler, the sixteen `kprofiler_*` / `kprofile_record_string`
// exports that `Klei/KProfilerPlugin.cs` declares.
//
// Klei's SimDLL carries a real profiler behind those exports -- per-thread event buffers, a
// string table, a broadcaster thread that streams to a file or to a local HTTP viewer, and an
// HTTP control listener -- and the shipped game never reaches any of it: every managed call
// site is `[Conditional("ENABLE_KPROFILER")]` and `KProfilerPlugin.Initialized` is never set.
// This is that profiler, reimplemented so the dev tooling can switch it on and the byte stream it writes is the one Klei's own
// viewer reads.
//
// WHAT IS KLEI'S AND WHAT IS NOT. Kept exactly: the state machine (disabled / idle / running /
// flushing / terminating), the session counter that invalidates stale per-thread buffers, the
// record layouts and type numbers, the string ids (FNV-1a 64 over the bytes), the string
// journal's 2 MiB pages, the stream header, the flush protocol, the HTTP routes and their
// content types. Changed, each for a stated reason:
//
//   - Threads and locks are Win32 (CreateThread, SRWLOCK) rather than std::thread/std::mutex.
//     simdll.cpp's frame worker already is, and the mingw toolchain this builds with does not
//     have to supply a C++ thread runtime for it.
//   - HTTP is a few dozen lines of Winsock rather than cpp-httplib. Four GET routes and one
//     POST need no third-party dependency in a DLL whose source is part of the trust story.
//   - The broadcaster thread sleeps 1 ms when it had nothing to do. Klei's yields in a loop
//     whenever no broadcaster is attached, which spins a core for the whole session once the
//     plugin is loaded -- a cost a dev-only feature has no business charging.
//
// Nothing here touches the simulation. Every recording call is one atomic load and a return
// while the profiler is not running.

#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kprof {

enum State : int {
  kDisabled = 0,
  kIdle = 1,
  kRunning = 2,
  kFlushing = 3,
  kTerminating = 4,
};

// The type field of each record. The size field before it counts the 8-byte header.
enum RecordType : uint32_t {
  kBeginSection = 0,
  kEndSection = 1,
  kPing = 4,
  kCounter = 5,
  kStringsBlock = 6,
  kThreadInfo = 8,
};

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001b3ull;

inline uint64_t Fnv1a(const uint8_t* bytes, size_t count) {
  uint64_t h = kFnvOffset;
  for (size_t i = 0; i < count; ++i) h = (h ^ bytes[i]) * kFnvPrime;
  return h;
}

inline uint64_t Fnv1a(const char* s) {
  return Fnv1a(reinterpret_cast<const uint8_t*>(s), strlen(s));
}

struct Lock {
  SRWLOCK srw = SRWLOCK_INIT;
};

struct Guard {
  Lock& lock;
  explicit Guard(Lock& l) : lock(l) { AcquireSRWLockExclusive(&lock.srw); }
  ~Guard() { ReleaseSRWLockExclusive(&lock.srw); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};

using Buffer = std::vector<uint8_t>;
using BufferPtr = std::unique_ptr<Buffer>;
using BufferQueue = std::deque<BufferPtr>;

inline void Put(Buffer& b, const void* data, size_t count) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  b.insert(b.end(), p, p + count);
}

inline void PutHeader(Buffer& b, uint32_t size, uint32_t type) {
  Put(b, &size, sizeof(size));
  Put(b, &type, sizeof(type));
}

// Filled thread buffers wait here for the broadcaster; emptied ones come back for reuse.
struct DataQueue {
  Lock mutex;
  BufferQueue queue;
  std::vector<BufferPtr> available;

  // A thread hands over its buffer and leaves with an empty one.
  void PushAndExchangeForEmptyBuffer(BufferPtr& buffer) {
    Guard g(mutex);
    queue.push_back(std::move(buffer));
    if (available.empty()) {
      buffer.reset(new Buffer());
    } else {
      buffer = std::move(available.back());
      available.pop_back();
    }
  }

  void Push(BufferPtr buffer) {
    Guard g(mutex);
    queue.push_back(std::move(buffer));
  }

  void ReturnEmptyBuffer(BufferPtr buffer) {
    buffer->clear();
    Guard g(mutex);
    available.push_back(std::move(buffer));
  }

  void Swap(BufferQueue& other) {
    Guard g(mutex);
    queue.swap(other);
  }

  bool IsEmpty() {
    Guard g(mutex);
    return queue.empty();
  }
};

// Every string ever recorded, by id, stored nul-terminated in 2 MiB pages that never move --
// the pages are what `kprofiler_stop_profiling` ships as StringsBlock records, and the id is
// the FNV-1a of the bytes, so a reader rebuilds the table without it being sent.
struct StringTable {
  static constexpr uint32_t kPageBytes = 0x200000;
  struct Page {
    uint32_t used = 0;
    char data[kPageBytes];
  };

  Lock mutex;
  std::unordered_map<uint64_t, const char*> strings;
  std::vector<const char*> order;  // insertion order, which is the order serialize walks
  std::vector<std::unique_ptr<Page>> pages;

  StringTable() { pages.emplace_back(new Page()); }

  // Caller holds `mutex`. A string longer than a page is not stored, as in Klei's.
  const char* Store(const char* s) {
    const uint32_t n = static_cast<uint32_t>(strlen(s)) + 1;
    if (n > kPageBytes) return nullptr;
    if (pages.back()->used + n > kPageBytes) pages.emplace_back(new Page());
    Page& page = *pages.back();
    char* out = page.data + page.used;
    memcpy(out, s, n);
    page.used += n;
    return out;
  }

  uint64_t Record(const char* s) {
    const uint64_t id = Fnv1a(s);
    Guard g(mutex);
    if (strings.find(id) == strings.end()) {
      const char* stored = Store(s);
      strings[id] = stored;
      if (stored != nullptr) order.push_back(stored);
    }
    return id;
  }

  // The /syncstrings body: a u64 tag, the u64 byte count of what follows, then every string
  // with its terminator.
  std::string Serialize() {
    Guard g(mutex);
    std::string out;
    const uint64_t tag = 0x0011001100000001ull;
    uint64_t total = 0;
    for (const char* s : order) total += strlen(s) + 1;
    out.append(reinterpret_cast<const char*>(&tag), sizeof(tag));
    out.append(reinterpret_cast<const char*>(&total), sizeof(total));
    for (const char* s : order) out.append(s, strlen(s) + 1);
    return out;
  }
};

struct Broadcaster {
  virtual ~Broadcaster() = default;
  // Moves some data on; true when there was nothing to move.
  virtual bool Tick(DataQueue& dq) = 0;
  // Moves everything queued now.
  virtual void Flush(DataQueue& dq) = 0;
};

struct FileBroadcaster : Broadcaster {
  static constexpr uint64_t kFlushBoundary = 0x100000;
  FILE* file = nullptr;
  BufferQueue to_send;
  uint64_t bytes_written = 0;

  explicit FileBroadcaster(const char* path) {
    file = fopen(path, "wb");
    if (file == nullptr) {
      fprintf(stderr, "Error opening profile write file: %s", path);
      return;
    }
    const uint64_t magic = Fnv1a("KPROFILER_EVENTSTREAM");
    const uint32_t version = 1;
    fwrite(&magic, sizeof(magic), 1, file);
    fwrite(&version, sizeof(version), 1, file);
  }

  ~FileBroadcaster() override {
    if (file != nullptr) fclose(file);
  }

  void Write(const Buffer& b) {
    if (file == nullptr || b.empty()) return;
    const size_t n = fwrite(b.data(), 1, b.size(), file);
    if (n != b.size()) {
      fprintf(stderr, "Error writing to profile file, %i of %i bytes written", static_cast<int>(n),
              static_cast<int>(b.size()));
    }
  }

  bool Tick(DataQueue& dq) override {
    if (to_send.empty() && !dq.IsEmpty()) dq.Swap(to_send);
    if (to_send.empty()) return true;
    BufferPtr b = std::move(to_send.front());
    to_send.pop_front();
    Write(*b);
    bytes_written += b->size();
    dq.ReturnEmptyBuffer(std::move(b));
    if (bytes_written >= kFlushBoundary) {
      if (file != nullptr) fflush(file);
      bytes_written = 0;
    }
    return false;
  }

  void Flush(DataQueue& dq) override {
    for (;;) {
      while (!to_send.empty()) {
        Write(*to_send.front());
        to_send.pop_front();
      }
      dq.Swap(to_send);
      if (to_send.empty()) break;
    }
    if (file != nullptr) fflush(file);
  }
};

inline bool EnsureWinsock() {
  static std::atomic<int> started{0};
  if (started.load() == 1) return true;
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
  started.store(1);
  return true;
}

// One POST to 127.0.0.1:port/datablock, connection closed after the reply.
inline void PostDataBlock(int port, const char* data, size_t count) {
  if (!EnsureWinsock()) return;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return;
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
    char head[256];
    const int n = snprintf(head, sizeof(head),
                           "POST /datablock HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
                           "Content-Type: application/octet-stream\r\nContent-Length: %zu\r\n"
                           "Connection: close\r\n\r\n",
                           port, count);
    bool ok = send(s, head, n, 0) == n;
    size_t sent = 0;
    while (ok && sent < count) {
      const int chunk = send(s, data + sent, static_cast<int>(count - sent), 0);
      if (chunk <= 0) ok = false;
      else sent += static_cast<size_t>(chunk);
    }
    if (ok) {
      shutdown(s, SD_SEND);
      char reply[512];
      while (recv(s, reply, sizeof(reply), 0) > 0) {
      }
    }
  }
  closesocket(s);
}

struct HttpBroadcaster : Broadcaster {
  static constexpr size_t kBatchBytes = 0x100000;
  static constexpr size_t kBatchSendAt = 0xc8000;
  int port;
  BufferQueue to_send;
  std::unique_ptr<char[]> batched{new char[kBatchBytes]};
  size_t batched_used = 0;

  explicit HttpBroadcaster(int p) : port(p) {}

  void FlushBatchedData() {
    if (batched_used == 0) return;
    PostDataBlock(port, batched.get(), batched_used);
    batched_used = 0;
  }

  // Small blocks share a POST; one that would overflow the batch goes on its own.
  void SendOrBatchData(const Buffer& b) {
    if (b.size() + batched_used < kBatchBytes) {
      if (!b.empty()) memcpy(batched.get() + batched_used, b.data(), b.size());
      batched_used += b.size();
      if (batched_used >= kBatchSendAt) FlushBatchedData();
    } else {
      PostDataBlock(port, reinterpret_cast<const char*>(b.data()), b.size());
    }
  }

  bool Tick(DataQueue& dq) override {
    if (to_send.empty() && !dq.IsEmpty()) dq.Swap(to_send);
    if (to_send.empty()) return true;
    BufferPtr b = std::move(to_send.front());
    to_send.pop_front();
    SendOrBatchData(*b);
    dq.ReturnEmptyBuffer(std::move(b));
    return false;
  }

  void Flush(DataQueue& dq) override {
    for (;;) {
      while (!to_send.empty()) {
        SendOrBatchData(*to_send.front());
        to_send.pop_front();
      }
      dq.Swap(to_send);
      if (to_send.empty()) break;
    }
    FlushBatchedData();
  }
};

struct ThreadInfo {
  uint64_t name;
  uint64_t category;
  uint64_t thread;
};

// Outside Globals on purpose: the sim's kernel scopes test it on every call, and reaching it
// through G() would allocate the profiler (and its first 2 MiB string page) in every game,
// profiled or not.
inline std::atomic<int> g_state{kDisabled};

struct Globals {
  std::atomic<int>& state = g_state;
  std::atomic<int> session{0};
  std::atomic<bool> flush_completed{false};
  DataQueue dataqueue;
  StringTable strings;

  Lock thread_id_mutex;
  std::vector<ThreadInfo> thread_ids;

  Lock broadcaster_mutex;
  std::unique_ptr<Broadcaster> broadcaster;
  std::atomic<bool> has_broadcaster{false};

  HANDLE broadcaster_thread = nullptr;
  HANDLE listener_thread = nullptr;
  std::atomic<SOCKET> server_socket{INVALID_SOCKET};
  int listener_port = 0;
};

// Never destroyed: its threads can outlive static destruction at DLL unload.
inline Globals& G() {
  static Globals* g = new Globals();
  return *g;
}

struct ThreadState {
  int depth = 0;
  int session = 0;
  BufferPtr buffer{new Buffer()};
};

// A pointer, never freed, rather than a thread_local object. A thread_local with a destructor
// in a mingw-built DLL is destroyed through emutls after the DLL has begun tearing down, and
// the process died with an access violation on exit (kproftest). One small buffer
// per thread that ever recorded is the whole cost of leaking it.
inline ThreadState& ThisThread() {
  static thread_local ThreadState* t = nullptr;
  if (t == nullptr) t = new ThreadState();
  return *t;
}

// A buffer filled under an earlier capture is discarded rather than mixed into this one.
inline ThreadState& Enter() {
  ThreadState& t = ThisThread();
  const int s = G().session.load();
  if (t.session != s) {
    t.session = s;
    t.depth = 0;
    t.buffer->clear();
  }
  return t;
}

inline int64_t NowNs() {
  LARGE_INTEGER counter;
  LARGE_INTEGER frequency;
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  const int64_t c = counter.QuadPart;
  const int64_t f = frequency.QuadPart;
  return (c / f) * 1000000000LL + ((c % f) * 1000000000LL) / f;
}

inline uint64_t ThreadUid() {
  const DWORD id = GetCurrentThreadId();
  const uint8_t bytes[4] = {static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8),
                            static_cast<uint8_t>(id >> 16), static_cast<uint8_t>(id >> 24)};
  return Fnv1a(bytes, sizeof(bytes));
}

inline bool Running() { return g_state.load(std::memory_order_relaxed) == kRunning; }

inline void BeginSection(uint64_t name, uint64_t category, int64_t gc_alloc_count) {
  if (!Running()) return;
  ThreadState& t = Enter();
  struct {
    uint64_t name;
    uint64_t category;
    int64_t time;
    uint64_t thread;
    int64_t gc;
  } r = {name, category, NowNs(), ThreadUid(), gc_alloc_count};
  PutHeader(*t.buffer, 8 + sizeof(r), kBeginSection);
  Put(*t.buffer, &r, sizeof(r));
  ++t.depth;
}

// The buffer is handed to the broadcaster when a thread's outermost section closes.
inline void EndSection(int64_t gc_alloc_count) {
  if (!Running()) return;
  ThreadState& t = Enter();
  if (t.depth <= 0) return;
  --t.depth;
  struct {
    int64_t time;
    uint64_t thread;
    int64_t gc;
  } r = {NowNs(), ThreadUid(), gc_alloc_count};
  PutHeader(*t.buffer, 8 + sizeof(r), kEndSection);
  Put(*t.buffer, &r, sizeof(r));
  if (t.depth == 0) G().dataqueue.PushAndExchangeForEmptyBuffer(t.buffer);
}

inline void Ping(uint64_t name, uint64_t category, double value) {
  if (!Running()) return;
  ThreadState& t = Enter();
  struct {
    uint64_t name;
    uint64_t category;
    int64_t time;
    uint64_t thread;
    double value;
  } r = {name, category, NowNs(), ThreadUid(), value};
  PutHeader(*t.buffer, 8 + sizeof(r), kPing);
  Put(*t.buffer, &r, sizeof(r));
}

inline void Counter(uint64_t name, double value) {
  if (!Running()) return;
  ThreadState& t = Enter();
  struct {
    uint64_t name;
    int64_t time;
    uint64_t thread;
    double value;
  } r = {name, NowNs(), ThreadUid(), value};
  PutHeader(*t.buffer, 8 + sizeof(r), kCounter);
  Put(*t.buffer, &r, sizeof(r));
}

inline void SetThreadInfo(uint64_t thread, uint64_t name, uint64_t category) {
  if (thread == 0) thread = ThreadUid();
  Guard g(G().thread_id_mutex);
  G().thread_ids.push_back(ThreadInfo{name, category, thread});
}

inline void StartProfiling() {
  G().session.fetch_add(1);
  G().state.store(kRunning);
}

// Blocks until the broadcaster has moved everything queued, or there is no broadcaster.
inline void WaitForFlush() {
  Globals& g = G();
  g.flush_completed.store(false);
  if (g.state.load() != kTerminating) g.state.store(kFlushing);
  while (g.has_broadcaster.load() && g.state.load() == kFlushing && !g.flush_completed.load()) {
    SwitchToThread();
  }
}

inline void FlushDataSender() {
  const int previous = G().state.load();
  WaitForFlush();
  G().state.store(previous);
}

// With `broadcast_info`, the stream is closed out with every thread's name and every string
// page, so a reader can name what it has just read.
inline void StopProfiling(bool broadcast_info) {
  Globals& g = G();
  if (broadcast_info) {
    BufferPtr b(new Buffer());
    {
      Guard lock(g.thread_id_mutex);
      for (const ThreadInfo& info : g.thread_ids) {
        PutHeader(*b, 8 + sizeof(info), kThreadInfo);
        Put(*b, &info, sizeof(info));
      }
    }
    {
      Guard lock(g.strings.mutex);
      for (const auto& page : g.strings.pages) {
        // Klei's size field counts the in-memory struct (a size and a pointer), not the 8
        // bytes of it written, so it reads 8 more than the record occupies. Kept, because it
        // is what a reader of Klei's stream expects.
        const uint64_t data_size = page->used;
        PutHeader(*b, static_cast<uint32_t>(data_size + 0x18), kStringsBlock);
        Put(*b, &data_size, sizeof(data_size));
        Put(*b, page->data, page->used);
      }
    }
    g.dataqueue.Push(std::move(b));
  }
  WaitForFlush();
  g.state.store(kIdle);
}

inline void ReplaceBroadcaster(Broadcaster* next) {
  Globals& g = G();
  if (g.has_broadcaster.load()) fprintf(stderr, "StartHTTPDataBroadcaster called with active broadcaster");
  Guard lock(g.broadcaster_mutex);
  g.broadcaster.reset(next);
  g.has_broadcaster.store(next != nullptr);
}

// stop_data_sender detaches without the "active broadcaster" warning, as Klei's does.
inline void ReplaceBroadcasterQuietly(Broadcaster* next) {
  Globals& g = G();
  Guard lock(g.broadcaster_mutex);
  g.broadcaster.reset(next);
  g.has_broadcaster.store(next != nullptr);
}

inline DWORD WINAPI BroadcasterMain(LPVOID) {
  Globals& g = G();
  while (g.state.load() != kTerminating) {
    bool idle = true;
    {
      Guard lock(g.broadcaster_mutex);
      const int state = g.state.load();
      if (g.broadcaster && state > kDisabled) {
        if (state < kFlushing) {
          idle = g.broadcaster->Tick(g.dataqueue);
        } else if (state == kFlushing && !g.flush_completed.load()) {
          g.broadcaster->Flush(g.dataqueue);
          g.flush_completed.store(true);
          idle = false;
        }
      }
    }
    if (idle) Sleep(1);
    else SwitchToThread();
  }
  Guard lock(g.broadcaster_mutex);
  if (g.broadcaster) g.broadcaster->Flush(g.dataqueue);
  return 0;
}

inline void LoadPlugin() {
  Globals& g = G();
  if (g.broadcaster_thread != nullptr) return;
  g.broadcaster_thread = CreateThread(nullptr, 0, BroadcasterMain, nullptr, 0, nullptr);
}

inline const char* StateName(int state) {
  switch (state) {
    case kDisabled: return "disabled";
    case kIdle: return "idle";
    case kRunning: return "running";
    case kFlushing: return "flushing";
    default: return "terminating";
  }
}

inline void Respond(SOCKET client, const char* status, const char* content_type,
                    const std::string& body) {
  char head[256];
  const int n = snprintf(head, sizeof(head),
                         "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                         "Connection: close\r\n\r\n",
                         status, content_type, body.size());
  send(client, head, n, 0);
  size_t sent = 0;
  while (sent < body.size()) {
    const int chunk = send(client, body.data() + sent, static_cast<int>(body.size() - sent), 0);
    if (chunk <= 0) break;
    sent += static_cast<size_t>(chunk);
  }
}

// The four routes Klei's listener serves: /start, /stop, /ping (the state's name) and
// /syncstrings (the string table).
inline void Serve(SOCKET client) {
  std::string request;
  char chunk[1024];
  while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
    const int n = recv(client, chunk, sizeof(chunk), 0);
    if (n <= 0) break;
    request.append(chunk, static_cast<size_t>(n));
  }
  std::string path;
  if (request.compare(0, 4, "GET ") == 0) {
    const size_t end = request.find(' ', 4);
    path = request.substr(4, end == std::string::npos ? std::string::npos : end - 4);
    const size_t query = path.find('?');
    if (query != std::string::npos) path.resize(query);
  }
  if (path == "/start") {
    StartProfiling();
    Respond(client, "200 OK", "text/plain", "");
  } else if (path == "/stop") {
    StopProfiling(true);
    Respond(client, "200 OK", "text/plain", "");
  } else if (path == "/ping") {
    Respond(client, "200 OK", "ping", StateName(G().state.load()));
  } else if (path == "/syncstrings") {
    Respond(client, "200 OK", "stringtable", G().strings.Serialize());
  } else {
    Respond(client, "404 Not Found", "text/plain", "");
  }
  shutdown(client, SD_SEND);
  closesocket(client);
}

inline DWORD WINAPI ListenerMain(LPVOID) {
  Globals& g = G();
  if (!EnsureWinsock()) return 0;
  fprintf(stderr, "Klei / Profiler / DLL / attempting to listen on 127.0.0.1 port %i ...\n",
          g.listener_port);
  SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server == INVALID_SOCKET) return 0;
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(g.listener_port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      listen(server, SOMAXCONN) != 0) {
    closesocket(server);
    return 0;
  }
  g.server_socket.store(server);
  for (;;) {
    SOCKET client = accept(server, nullptr, nullptr);
    if (client == INVALID_SOCKET) break;  // the socket was closed by UnloadPlugin
    Serve(client);
  }
  return 0;
}

inline void StartHttpControlListener(int port) {
  Globals& g = G();
  if (g.listener_thread != nullptr) return;
  g.listener_port = port;
  g.listener_thread = CreateThread(nullptr, 0, ListenerMain, nullptr, 0, nullptr);
}

inline void UnloadPlugin() {
  Globals& g = G();
  g.state.store(kTerminating);
  const SOCKET server = g.server_socket.exchange(INVALID_SOCKET);
  if (server != INVALID_SOCKET) {
    shutdown(server, SD_BOTH);
    closesocket(server);
  }
  if (g.listener_thread != nullptr) {
    WaitForSingleObject(g.listener_thread, INFINITE);
    CloseHandle(g.listener_thread);
    g.listener_thread = nullptr;
  }
  if (g.broadcaster_thread != nullptr) {
    WaitForSingleObject(g.broadcaster_thread, INFINITE);
    CloseHandle(g.broadcaster_thread);
    g.broadcaster_thread = nullptr;
  }
  Guard lock(g.broadcaster_mutex);
  g.broadcaster.reset();
  g.has_broadcaster.store(false);
}

}  // namespace kprof
