#!/usr/bin/env python3
"""Decodes a capture from trace-capture.sh (raw ftrace ring-buffer pages + event formats) and, if present, the GL
shim records (gl.bin), on the PC.

    trace_decode.py CAPDIR [--frames N]

Prints, for every fifo underflow logged during the capture, the frames before it: vsync times, GPU/DE interrupt
times, and the GL calls of the app (aligned through the shim's trace_marker lines), next to the same statistics for
clean frames.
"""
import bisect
import collections
import os
import re
import struct
import sys

PAGE = 4096


def read_formats(cap):
    """Event id -> (name, {field: (offset, size)}) from the fmt_* files."""
    events = {}
    for f in os.listdir(cap):
        if not f.startswith("fmt_"):
            continue
        text = open(os.path.join(cap, f)).read()
        eid = int(re.search(r"^ID: (\d+)", text, re.M)[1])
        fields = {m[2]: (int(m[3]), int(m[4])) for m in
                  re.finditer(r"field:(.*?)\s*([\w\[\]]+);\s*offset:(\d+);\s*size:(\d+);", text)}
        fields = {re.sub(r"\[.*", "", k): v for k, v in fields.items()}
        events[eid] = (f[4:], fields)
    return events


def decode_cpu(path, cpu, events):
    """Yields (ns, cpu, name, data bytes) for one CPU's raw pages."""
    raw = open(path, "rb").read()
    for p in range(0, len(raw) - PAGE + 1, PAGE):
        page = raw[p:p + PAGE]
        ts, commit = struct.unpack_from("<QI", page, 0)
        size = commit & 0x0FFFFFFF
        off, end = 12, 12 + size
        while off + 4 <= end:
            hdr = struct.unpack_from("<I", page, off)[0]
            tlen, delta = hdr & 0x1F, hdr >> 5
            off += 4
            if tlen == 29:      # padding
                if delta == 0:
                    break
                length = struct.unpack_from("<I", page, off)[0]
                off += length
                ts += delta
                continue
            if tlen == 30:      # time extend
                ext = struct.unpack_from("<I", page, off)[0]
                ts += delta + (ext << 27)
                off += 4
                continue
            if tlen == 31:      # absolute time stamp (not used by 3.10)
                off += 8
                continue
            ts += delta
            if tlen == 0:
                length = struct.unpack_from("<I", page, off)[0] - 4
                off += 4
            else:
                length = tlen * 4
            data = page[off:off + length]
            off += length
            if len(data) < 2:
                continue
            eid = struct.unpack_from("<H", data, 0)[0]
            if eid in events:
                yield ts, cpu, events[eid][0], data


def field(data, fields, name, fmt):
    """One field of an event record."""
    o, _ = fields[name]
    return struct.unpack_from(fmt, data, o)[0]


def data_loc_str(data, fields, name):
    """A __data_loc char[] field."""
    o, _ = fields[name]
    v = struct.unpack_from("<I", data, o)[0]
    start, length = v & 0xFFFF, v >> 16
    return data[start:start + length].split(b"\0")[0].decode(errors="replace")


GL_NAMES = {1: "draw", 2: "drawEl", 3: "clear", 4: "texSub", 5: "texImg", 6: "copyTex", 7: "bindFB", 8: "finish",
            9: "flush", 10: "swapIn", 11: "swapOut", 12: "readPix", 13: "discard", 14: "bindTex", 15: "useProg"}


def main():
    cap = sys.argv[1]
    nframes = int(sys.argv[sys.argv.index("--frames") + 1]) if "--frames" in sys.argv else 3
    events = read_formats(cap)
    byname = {v[0]: v[1] for v in events.values()}
    ev = []
    for c in range(4):
        path = os.path.join(cap, "raw%d" % c)
        if os.path.exists(path):
            ev.extend(decode_cpu(path, c, events))
    ev.sort(key=lambda e: e[0])
    t0 = float(open(os.path.join(cap, "t0")).read())
    print("events %d, %.3f .. %.3f s" % (len(ev), ev[0][0] / 1e9, ev[-1][0] / 1e9))

    vsync, irqs, markers, opened = [], [], [], {}
    for ns, cpu, name, data in ev:
        if name == "owlfb_vsync":
            vsync.append(ns)
        elif name == "irq_handler_entry":
            opened[cpu] = (ns, data_loc_str(data, byname[name], "name"))
        elif name == "irq_handler_exit" and cpu in opened:
            s, n = opened.pop(cpu)
            irqs.append((s, ns, n))
        elif name == "print":
            o, _ = byname["print"]["buf"]
            markers.append((ns, data[o:].split(b"\0")[0].decode(errors="replace").strip()))

    uf = []
    for line in open(os.path.join(cap, "dmesg_uf")):
        m = re.search(r"\[\s*([\d.]+)\]", line)
        if m and float(m[1]) > t0:
            uf.append(int(float(m[1]) * 1e9))
    print("vsyncs %d, underflows %d at %s s after start" % (len(vsync), len(uf), [round(u / 1e9 - t0, 1) for u in uf]))

    # GL records, aligned: the shim's start marker holds its CLOCK_MONOTONIC; the trace time of that marker is known
    gl = []
    glpath = os.path.join(cap, "gl.bin")
    off = None
    for ns, text in markers:
        m = re.match(r"GLSHIM start mono (\d+)", text)
        if m:
            off = ns - int(m[1])
    if os.path.exists(glpath) and off is not None:
        raw = open(glpath, "rb").read()
        for i in range(0, len(raw) - 15, 16):
            mono, typ, a, b, c = struct.unpack_from("<QHHHH", raw, i)
            gl.append((mono + off, typ, a, b, c))
        print("gl records %d (offset trace-mono %.3f ms)" % (len(gl), off / 1e6))
    glt = [g[0] for g in gl]

    def frame_report(k):
        s, e = vsync[k], vsync[k + 1] if k + 1 < len(vsync) else vsync[k] + 16_540_000
        sgx = [(a - s) / 1e6 for a, b, n in irqs if s <= a < e and n == "SGX ISR"]
        other = [(n, round((a - s) / 1e6, 2), round((b - a) / 1e3)) for a, b, n in irqs
                 if s <= a < e and n not in ("SGX ISR", "twd", "asoc_de")]
        i, j = bisect.bisect_left(glt, s), bisect.bisect_left(glt, e)
        calls = collections.Counter(GL_NAMES.get(g[1], g[1]) for g in gl[i:j])
        uploads = [(g[2], g[3], round((g[0] - s) / 1e6, 2)) for g in gl[i:j] if g[1] == 4 and g[2] != 0xFFFF]
        swaps = [round((g[0] - s) / 1e6, 2) for g in gl[i:j] if g[1] in (10, 11)]
        return sgx, other, calls, uploads, swaps

    vs = vsync
    for u in uf:
        k = bisect.bisect_right(vs, u) - 1
        print("\n=== underflow printk %.6f: %.2f ms after vsync #%d" % (u / 1e9, (u - vs[k]) / 1e6, k))
        for f in range(k - nframes, k + 1):
            sgx, other, calls, uploads, swaps = frame_report(f)
            print(" #%d sgx %2d [%s]" % (f, len(sgx), " ".join("%.1f" % x for x in sgx)))
            print("     swap in/out %s  uploads %s  other irq %s" % (swaps, uploads, other))
            print("     gl %s" % dict(calls))

    # clean-frame statistics for comparison
    bad = set()
    for u in uf:
        k = bisect.bisect_right(vs, u) - 1
        bad.update(range(k - 2, k + 1))
    clean = [f for f in range(1, len(vs) - 1) if f not in bad]
    up = collections.Counter()
    for f in clean[::7]:
        sgx, other, calls, uploads, swaps = frame_report(f)
        up[(len(sgx), len(uploads))] += 1
    print("\nclean frames (every 7th): (sgx irqs, uploads) -> count:", dict(up.most_common(12)))


if __name__ == "__main__":
    main()
