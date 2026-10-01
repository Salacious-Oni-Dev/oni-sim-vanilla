#!/usr/bin/env python3
"""Read a KProfiler capture and write it as a Chrome trace, or summarise it.

    tools/kprofile2chrome.py capture.kprof              # -> capture.json beside it
    tools/kprofile2chrome.py capture.kprof -o trace.json
    tools/kprofile2chrome.py capture.kprof --summary [--top 30]

The input is the byte stream Klei's in-DLL profiler writes (docs/KPROFILER.md): a file from
`kprofiler_start_file_data_sender`, which starts with a 12-byte header, or the bodies posted to
`/datablock` by `kprofiler_start_http_data_sender`, which do not. Both are accepted.

Klei's own viewer for this stream is not shipped. Chrome's chrome://tracing and
https://ui.perfetto.dev both open the JSON this writes: sections become duration slices on the
thread that recorded them, pings instant events carrying their value, counters counter tracks.

String ids are FNV-1a 64 of the string's bytes, so names are recovered from the StringsBlock
records `kprofiler_stop_profiling(1)` appends. A capture stopped without them has only ids,
printed as hex. A capture cut off mid-record (the game closed while it ran) is read up to the
last whole record.
"""

import argparse
import json
import os
import struct
import sys
from collections import defaultdict

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK = 0xFFFFFFFFFFFFFFFF

BEGIN, END, PING, COUNTER, STRINGS, THREAD_INFO = 0, 1, 4, 5, 6, 8


def fnv1a(data: bytes) -> int:
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK
    return h


HEADER_MAGIC = fnv1a(b"KPROFILER_EVENTSTREAM")


def records(data: bytes):
    """Yield (type, fields) for each record, skipping a stream header if present."""
    at = 0
    if len(data) >= 12:
        magic, version = struct.unpack_from("<QI", data, 0)
        if magic == HEADER_MAGIC:
            if version != 1:
                raise SystemExit(f"stream version {version}, this reader knows 1")
            at = 12
    while at + 8 <= len(data):
        size, rtype = struct.unpack_from("<II", data, at)
        body = at + 8
        # A capture the game quit in the middle of ends inside a record; keep what came before it.
        need = body + 8 + struct.unpack_from("<Q", data, body)[0] if rtype == STRINGS and body + 8 <= len(data) else at + size
        if rtype == STRINGS and body + 8 > len(data) or need > len(data):
            sys.stderr.write(f"stream ends inside a record at byte {at} of {len(data)}; the capture was cut off (reading what came before)\n")
            return
        if rtype == BEGIN:
            yield rtype, struct.unpack_from("<QQqQq", data, body)
            at += size
        elif rtype == END:
            yield rtype, struct.unpack_from("<qQq", data, body)
            at += size
        elif rtype == PING:
            yield rtype, struct.unpack_from("<QQqQd", data, body)
            at += size
        elif rtype == COUNTER:
            yield rtype, struct.unpack_from("<QqQd", data, body)
            at += size
        elif rtype == STRINGS:
            # The size field counts 8 bytes this record does not occupy (see sim/kprofiler.h):
            # step by the data length, not by the size.
            (data_size,) = struct.unpack_from("<Q", data, body)
            yield rtype, (data[body + 8 : body + 8 + data_size],)
            at = body + 8 + data_size
        elif rtype == THREAD_INFO:
            yield rtype, struct.unpack_from("<QQQ", data, body)
            at += size
        else:
            sys.stderr.write(f"unknown record type {rtype} at byte {at}, size {size}; stopping\n")
            return


def load(paths):
    data = b"".join(open(p, "rb").read() for p in paths)
    recs = list(records(data))
    names = {}
    threads = {}
    for rtype, fields in recs:
        if rtype == STRINGS:
            for s in fields[0].split(b"\0"):
                if s or True:
                    names[fnv1a(s)] = s.decode("utf-8", "replace")
    for rtype, fields in recs:
        if rtype == THREAD_INFO:
            name, category, thread = fields
            threads[thread] = (name, category)
    return recs, names, threads


def label(names, ident):
    return names.get(ident, f"0x{ident:016x}")


def thread_ids(recs):
    order = {}
    for rtype, fields in recs:
        if rtype == BEGIN:
            thread = fields[3]
        elif rtype == END:
            thread = fields[1]
        elif rtype == PING:
            thread = fields[3]
        elif rtype == COUNTER:
            thread = fields[2]
        else:
            continue
        order.setdefault(thread, len(order) + 1)
    return order


def to_chrome(recs, names, threads):
    tid = thread_ids(recs)
    t0 = None
    for rtype, fields in recs:
        t = {BEGIN: 2, END: 0, PING: 2, COUNTER: 1}.get(rtype)
        if t is not None:
            t0 = fields[t] if t0 is None else min(t0, fields[t])
    t0 = t0 or 0
    events = []
    stacks = defaultdict(list)
    for thread, index in tid.items():
        if thread in threads:
            name, category = threads[thread]
            thread_name = f"{label(names, name)} ({label(names, category)})"
        else:
            thread_name = f"thread {index}"
        events.append({"name": "thread_name", "ph": "M", "pid": 1, "tid": index, "ts": 0,
                       "cat": "__metadata", "args": {"name": thread_name}})
    for rtype, fields in recs:
        if rtype == BEGIN:
            name, category, t, thread, gc = fields
            stacks[thread].append(name)
            events.append({"name": label(names, name), "cat": label(names, category), "ph": "B",
                           "pid": 1, "tid": tid[thread], "ts": (t - t0) / 1000.0})
        elif rtype == END:
            t, thread, gc = fields
            name = stacks[thread].pop() if stacks[thread] else None
            event = {"ph": "E", "pid": 1, "tid": tid[thread], "ts": (t - t0) / 1000.0}
            if name is not None:
                event["name"] = label(names, name)
            events.append(event)
        elif rtype == PING:
            name, category, t, thread, value = fields
            events.append({"name": label(names, name), "cat": label(names, category), "ph": "i",
                           "s": "t", "pid": 1, "tid": tid[thread], "ts": (t - t0) / 1000.0,
                           "args": {"value": value}})
        elif rtype == COUNTER:
            name, t, thread, value = fields
            events.append({"name": label(names, name), "ph": "C", "pid": 1, "tid": tid[thread],
                           "ts": (t - t0) / 1000.0, "args": {label(names, name): value}})
    return {"traceEvents": events, "displayTimeUnit": "ms"}


def summary(recs, names, threads, top):
    tid = thread_ids(recs)
    total = defaultdict(float)
    self_time = defaultdict(float)
    calls = defaultdict(int)
    stacks = defaultdict(list)  # per thread: [name, start, child_ns]
    span = [None, None]
    for rtype, fields in recs:
        if rtype == BEGIN:
            name, category, t, thread, gc = fields
            stacks[thread].append([name, t, 0])
            span[0] = t if span[0] is None else min(span[0], t)
        elif rtype == END:
            t, thread, gc = fields
            span[1] = t if span[1] is None else max(span[1], t)
            if not stacks[thread]:
                continue
            name, start, child = stacks[thread].pop()
            elapsed = t - start
            key = (thread, name)
            total[key] += elapsed
            self_time[key] += elapsed - child
            calls[key] += 1
            if stacks[thread]:
                stacks[thread][-1][2] += elapsed
    seconds = ((span[1] or 0) - (span[0] or 0)) / 1e9
    frames = sum(c for (thread, name), c in calls.items() if label(names, name) == "Frame")
    print(f"span {seconds:.2f} s, {sum(calls.values())} sections, {len(tid)} threads, {frames} frames")
    rows = sorted(total, key=lambda k: self_time[k], reverse=True)[:top]
    print(f"{'self ms':>10} {'total ms':>10} {'calls':>7} {'ms/frame':>9}  thread  section")
    for key in rows:
        thread, name = key
        tname = label(names, threads[thread][0]) if thread in threads else f"thread {tid.get(thread)}"
        per_frame = total[key] / 1e6 / frames if frames else 0.0
        print(f"{self_time[key] / 1e6:10.2f} {total[key] / 1e6:10.2f} {calls[key]:7d} {per_frame:9.3f}  "
              f"{tname[:20]:20s}  {label(names, name)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", help="capture file(s); several are concatenated in order")
    ap.add_argument("-o", "--output", help="JSON path (default: first input with .json)")
    ap.add_argument("--summary", action="store_true", help="print a per-section table instead")
    ap.add_argument("--top", type=int, default=30)
    args = ap.parse_args()
    recs, names, threads = load(args.inputs)
    if args.summary:
        summary(recs, names, threads, args.top)
        return
    out = args.output or os.path.splitext(args.inputs[0])[0] + ".json"
    with open(out, "w") as f:
        json.dump(to_chrome(recs, names, threads), f)
    print(f"wrote {out}: {len(recs)} records, {len(names)} strings, {len(threads)} named threads")


if __name__ == "__main__":
    main()
