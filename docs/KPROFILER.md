# KProfiler

The game declares an event profiler that lives inside the SimDLL: sixteen
`[DllImport("SimDLL")]` functions in `KProfilerPlugin`. The game's own build never turns it on,
because every managed call site is compiled out, but the native functions are part of the
library's interface. This library implements all sixteen, with the same byte stream and the same
HTTP control surface, so a tool that reads the game's profiler output reads this one's.

It also records its own kernels into the capture, so a profile taken in the game shows the
simulation's work nested inside the frame that ran it.

Implementation: `sim/kprofiler.h`. Test: `driver/src/kproftest.cpp`. Reader:
`tools/kprofile2chrome.py`.

## States

| value | name | `/ping` body | meaning |
|---|---|---|---|
| 0 | disabled | `disabled` | initial; `kprofiler_load_plugin` does not change it |
| 1 | idle | `idle` | after a stop |
| 2 | running | `running` | recording |
| 3 | flushing | `flushing` | a stop or flush is waiting on the broadcaster |
| 4 | terminating | `terminating` | `kprofiler_unload_plugin` |

Only state 2 records anything. In any other state, every recording export returns after one
atomic load.

## The exports

| export | does |
|---|---|
| `kprofiler_load_plugin` | starts the broadcaster thread |
| `kprofiler_unload_plugin` | state 4; closes the listener socket, joins both threads, drops the broadcaster |
| `kprofiler_start_file_data_sender(path)` | replaces the broadcaster with a file writer and writes the header |
| `kprofiler_start_http_data_sender(port)` | replaces it with a sender that POSTs to `127.0.0.1:port/datablock` |
| `kprofiler_stop_data_sender` | drops the broadcaster |
| `kprofiler_start_http_control_listener(port)` | serves `/start`, `/stop`, `/ping` and `/syncstrings` on 127.0.0.1 |
| `kprofiler_start_profiling` | session += 1; state 2 |
| `kprofiler_stop_profiling(broadcast_info)` | see [Stopping](#stopping); waits for the flush; state 1 |
| `kprofiler_flush_data_sender` | waits until the broadcaster has written everything queued, then restores the state |
| `kprofile_record_string(s)` | stores `s` once and returns its id |
| `kprofiler_get_thread_uid` | FNV-1a 64 of the 4 bytes of `GetCurrentThreadId` |
| `kprofiler_set_thread_info(thread, name, category)` | remembers a thread's name and category; thread 0 means the calling thread |
| `kprofiler_begin_section(name, category, gc)` | appends a begin record; depth += 1 |
| `kprofiler_end_section(gc)` | appends an end record; depth -= 1; at depth 0 the thread's buffer is queued |
| `kprofiler_ping(name, category, value)` | appends a ping |
| `kprofiler_counter(name, value)` | appends a counter |

A string id is FNV-1a 64 over the string's bytes, without the terminator. Readers recompute ids
from the strings, so the id itself is never sent.

An `end_section` at depth 0 is ignored. That happens when a begin arrived before the capture
started and its end arrived after.

## Buffers and sessions

Each thread appends to its own buffer. A buffer is handed to the broadcaster only when the
thread's outermost section closes, so pings and counters wait for the next section end. A
consequence: when a capture is stopped while a thread has a section open, that section and
everything inside it are dropped.

A thread remembers the session it last recorded under. When a new capture starts, the thread's
first record clears whatever the previous session left in its buffer.

## Stopping

`kprofiler_stop_profiling(1)` queues one more buffer before it waits for the flush. That buffer
holds:

- a thread-info record for every `kprofiler_set_thread_info` call
- a strings-block record for every page of the string journal

Journal pages are 2 MiB, hold strings with their terminators, and never move. A capture stopped
with `broadcast_info` 0, or cut off by the process ending, has no string table, and a reader can
show only hex ids.

## The stream

All values are little-endian. A file starts with a 12-byte header; POSTed bodies have none.

```
u64 FNV-1a("KPROFILER_EVENTSTREAM")   = 0x9ff6713edc9542e8
u32 version                           = 1
```

Every record has an 8-byte header followed by its payload:

```
u32 size    (header included)
u32 type
```

| type | record | size | payload |
|---|---|---|---|
| 0 | begin section | 48 | u64 name, u64 category, i64 time_ns, u64 thread, i64 gc |
| 1 | end section | 32 | i64 time_ns, u64 thread, i64 gc |
| 4 | ping | 48 | u64 name, u64 category, i64 time_ns, u64 thread, f64 value |
| 5 | counter | 40 | u64 name, i64 time_ns, u64 thread, f64 value |
| 6 | strings block | data + 24 | u64 data_size, then data_size bytes of nul-terminated strings |
| 8 | thread info | 32 | u64 name, u64 category, u64 thread |

**The strings block's `size` field is 8 more than the bytes the record occupies.** A reader must
step over this record by `data_size`, not by `size`. The game's library writes it this way,
and this one keeps the quirk so that existing readers of the stream read both.

`time_ns` is `QueryPerformanceCounter` converted to nanoseconds, split into whole and remainder
parts so that the conversion does not overflow. `gc` is whatever the caller passes; the game's
`getGCAllocCount` returns -1.

## HTTP

| route | response |
|---|---|
| `GET /start` | 200, empty; starts profiling |
| `GET /stop` | 200, empty, after `stop_profiling(1)` returns |
| `GET /ping` | 200, `Content-Type: ping`, body is the state name |
| `GET /syncstrings` | 200, `Content-Type: stringtable`, body is u64 `0x0011001100000001`, u64 byte count, then every string with its terminator |
| anything else | 404 |

The HTTP data sender batches blocks into 1 MiB. It POSTs a batch once 0xC8000 bytes have
accumulated, or on a flush. A block too big for a batch is POSTed on its own.

The listener and the sender bind to 127.0.0.1 only.

## The simulation's own sections

While a capture is running, every `PROF(slot)` scope in the simulation opens a section named
after its kernel (`Project`, `FillPropertyTextures`, `StepGasPressure`, and so on), and the frame
worker names its thread "SimDLL frame worker". The sections nest inside that thread's
`whole frame` section.

When no capture is running, each kernel scope costs one atomic load. The sections only record
times, so they never change what the simulation computes.

## Implementation notes

- **Win32 threads and `SRWLOCK`**, not `std::thread` and `std::mutex`, for the same reason as
  the frame worker (see `THREADING.md`): the mingw toolchain then needs no C++ thread runtime.
- **About 150 lines of Winsock** instead of an HTTP library. Four GET routes and one POST do not
  justify a third-party dependency in a DLL whose source is meant to be read.
- **The broadcaster sleeps 1 ms when it has no work**, rather than yielding in a loop, so a
  loaded plugin does not keep a core busy for the whole session.
- **The per-thread buffer is a `thread_local` pointer that is never freed**, not a `thread_local`
  object. A mingw DLL destroying a non-trivial `thread_local` at thread exit crashed the process
  on shutdown.
- **The state is a standalone atomic**, outside the lazily built profiler object. Kernel scopes
  test it on every call, and reaching it through the object would allocate the profiler, and its
  first 2 MiB string page, in every game.

## Taking a capture

From managed code, load the plugin once, attach a file sender, and start and stop:

```
kprofiler_load_plugin()
kprofiler_start_file_data_sender("capture.kprof")
kprofiler_set_thread_info(0, "Main thread", "main")
kprofiler_start_profiling()
    ... kprofiler_begin_section / kprofiler_end_section around the code to measure ...
kprofiler_stop_profiling(1)
```

Or start the control listener and drive it with `/start` and `/stop`. Every capture then goes to
whichever sender is attached, and each stop appends its own thread info and string table.

## Reading a capture

```
python3 tools/kprofile2chrome.py capture.kprof             # writes capture.json
python3 tools/kprofile2chrome.py capture.kprof --summary   # a table
```

The JSON opens in `chrome://tracing` or https://ui.perfetto.dev. Sections become duration slices
on their thread, named from the thread info; pings become instant events carrying their value;
counters become counter tracks. A file holding several captures is merged. A file cut off in the
middle of a record is read up to the last whole record.

`--summary` prints self and total milliseconds, call counts, and milliseconds per frame for every
section.

## Verification

`driver/build/kproftest.exe <SimDLL.dll>` loads a DLL into its own process, with no game running,
and runs one scripted sequence:

1. ids: string ids and the thread uid
2. HTTP control: `/ping`, `/start`, `/syncstrings`, a 404 and `/stop`
3. a file capture: nested sections, a ping and a counter on the main thread, one section on a
   second thread, and the thread info and strings block that `stop_profiling(1)` adds
4. a second session into a new file, to check that no stale buffer carries over
5. the same workload through the HTTP data sender, into a sink the test runs itself

It prints every record with its size field and payload, with times reduced to "went forward or
not" and threads to their order of appearance. Run it against the game's own `SimDLL.dll` and
against this build, and diff the two outputs. Apart from the listener's banner on stderr they
should be identical.

Against the game's DLL this build's output is identical line for line, with `failures=0` on each.
The profiler only records times, so adding it left the rest of the offline suite unmoved:
`diffsim --scenario all` gave the same output before and after.
