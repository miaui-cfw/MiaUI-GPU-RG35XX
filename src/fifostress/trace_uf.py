#!/usr/bin/env python3
"""Analyses a trace-uf.sh capture on the PC: DDR bandwidth per frame (DMM counters), GPU gating and the timeline
around every DE fifo underflow.

    trace_uf.py CAPDIR [--window MS]

The DMM counters (pc0 = master0, pc1 = master1; cumulative 16-byte units, reset by the kernel's dmm hrtimer every
second) are sampled by the kprobes at every IRQ, flip and underflow. Frames are delimited by the DE IRQ (irq 33).
"""
import bisect
import collections
import statistics
import sys
from typing import Dict, List, Tuple

import trace_decode as td

DE_IRQ = 33          # asoc_de: DE pre-line interrupt, once per frame
SGX_IRQ = 35
UNIT = 16            # bytes per DMM count


def load(cap: str) -> Tuple[List[tuple], Dict[str, dict]]:
    """All events of the capture, sorted by time.

    Args:
        cap: Capture directory (raw0..raw3 + fmt_* files).

    Returns:
        (rows, fields by event name); a row is (ns, cpu, name, data bytes).
    """
    ev = td.read_formats(cap)
    rows = []
    for c in range(4):
        rows += list(td.decode_cpu(f"{cap}/raw{c}", c, ev))
    rows.sort(key=lambda r: r[0])
    return rows, {v[0]: v[1] for v in ev.values()}


def u32(data: bytes, f: dict, name: str) -> int:
    """An unsigned 32-bit field of an event."""
    return td.field(data, f, name, "<I")


def samples(rows: List[tuple], byn: Dict[str, dict]) -> List[Tuple[int, int, int]]:
    """DMM counter samples (ns, pc0, pc1) made monotonic across the kernel's periodic counter resets.

    Args:
        rows: Sorted events.
        byn: Event fields by name.

    Returns:
        Samples with cumulative counts since the start of the capture.
    """
    out = []
    c0 = c1 = 0
    last = None
    for ns, cpu, name, data in rows:
        f = byn.get(name, {})
        if "pc0" not in f:
            continue
        p0, p1 = u32(data, f, "pc0"), u32(data, f, "pc1")
        if last is not None:
            if p0 < last[0] // 2 or p1 < last[1] // 2:
                # counters reset between the two samples: count from 0 (the few counts before the reset are lost)
                c0, c1 = c0 + p0, c1 + p1
            else:
                # samples taken on two CPUs at the same instant can be a few counts out of order: never go back
                c0, c1 = c0 + max(0, p0 - last[0]), c1 + max(0, p1 - last[1])
        last = (max(p0, last[0]), max(p1, last[1])) if last and p0 >= last[0] // 2 else (p0, p1)
        out.append((ns, c0, c1))
    return out


def between(smp: List[Tuple[int, int, int]], t0: int, t1: int) -> Tuple[float, float, float]:
    """Bytes of master0 and master1 between two times, from the nearest samples inside [t0, t1].

    Returns:
        (master0 bytes, master1 bytes, covered ns); zeros when fewer than two samples fall inside.
    """
    ts = [s[0] for s in smp]
    i, j = bisect.bisect_left(ts, t0), bisect.bisect_right(ts, t1) - 1
    if j <= i:
        return 0.0, 0.0, 0.0
    return (smp[j][1] - smp[i][1]) * UNIT, (smp[j][2] - smp[i][2]) * UNIT, float(smp[j][0] - smp[i][0])


def main() -> None:
    """Prints the per-frame statistics and the underflow timelines."""
    cap = sys.argv[1]
    win = float(sys.argv[sys.argv.index("--window") + 1]) if "--window" in sys.argv else 20.0
    rows, byn = load(cap)
    cnt = collections.Counter(r[2] for r in rows)
    print("events:", dict(cnt.most_common()))
    smp = samples(rows, byn)
    de_irq = [ns for ns, cpu, name, data in rows if name == "hirq" and u32(data, byn["hirq"], "irq") == DE_IRQ]
    ufs = [(ns, u32(data, byn["uf"], "stat")) for ns, cpu, name, data in rows if name == "uf"]
    print(f"{len(smp)} DMM samples, {len(de_irq)} frames, {len(ufs)} underflows")

    # per frame (DE IRQ to DE IRQ): DE and GPU MB/s
    fr = []
    for a, b in zip(de_irq, de_irq[1:]):
        m0, m1, cov = between(smp, a, b)
        if cov > 0.8 * (b - a):
            fr.append((a, b - a, m0 / cov * 1e3, m1 / cov * 1e3))
    if fr:
        for k, lab in ((2, "master0 (DE) MB/s"), (3, "master1 (GPU) MB/s")):
            v = sorted(x[k] for x in fr)
            print(f"  per frame {lab}: min {v[0]:.0f} p50 {v[len(v)//2]:.0f} p99 {v[int(len(v)*.99)]:.0f} max {v[-1]:.0f}")
        per = sorted(x[1] / 1e6 for x in fr)
        print(f"  frame period ms: min {per[0]:.3f} p50 {per[len(per)//2]:.3f} max {per[-1]:.3f}")

    # short windows (consecutive samples 0.2-1.5 ms apart): peak GPU rate and lowest DE rate
    pk = []
    for (t0, a0, a1), (t1, b0, b1) in zip(smp, smp[1:]):
        dt = t1 - t0
        if 200_000 <= dt <= 1_500_000:
            pk.append((t0, dt, (b0 - a0) * UNIT / dt * 1e3, (b1 - a1) * UNIT / dt * 1e3))
    if pk:
        g = sorted(x[3] for x in pk)
        d = sorted(x[2] for x in pk)
        print(f"  <=1.5 ms windows ({len(pk)}): GPU MB/s p50 {g[len(g)//2]:.0f} p99 {g[int(len(g)*.99)]:.0f} "
              f"max {g[-1]:.0f}; DE MB/s min {d[0]:.0f} p1 {d[int(len(d)*.01)]:.0f} p50 {d[len(d)//2]:.0f}")

    # GPU gating, DE IRQ latency vs period
    for n in ("sgxon", "sgxoff", "gclkon", "gclkoff", "pgon", "pgoff", "gpgon", "gpgoff", "cpuup", "cpudown",
              "l2fa", "l1fa", "pvfl", "pvcl"):
        if cnt.get(n):
            print(f"  {n}: {cnt[n]}")

    # IRQ durations by irq number
    ent: Dict[Tuple[int, int], int] = {}
    dur = collections.defaultdict(list)
    for ns, cpu, name, data in rows:
        if name == "irq_handler_entry":
            ent[(cpu, td.field(data, byn[name], "irq", "<i"))] = ns
        elif name == "irq_handler_exit":
            k = (cpu, td.field(data, byn[name], "irq", "<i"))
            if k in ent:
                dur[k[1]].append(ns - ent.pop(k))
    for irq, v in sorted(dur.items()):
        v.sort()
        print(f"  irq {irq}: n={len(v)} p50 {v[len(v)//2]/1e3:.0f} us max {v[-1]/1e3:.0f} us")

    # timelines around underflows
    names = ("uf", "fcr", "pan", "owlfb_vsync", "sgxon", "sgxoff", "gclkon", "gclkoff", "pgon", "pgoff",
             "gpgon", "gpgoff", "l2fa", "l1fa", "l2fr", "l2cr", "l2ir", "pvfl", "pvcl", "cpuup", "cpudown")
    for ut, stat in ufs:
        print(f"\n=== underflow at {ut/1e9:.6f} stat 0x{stat:x}")
        lo = ut - int(win * 1e6)
        for ns, cpu, name, data in rows:
            if ns < lo:
                continue
            if ns > ut + 2_000_000:
                break
            f = byn.get(name, {})
            if name == "hirq":
                irq = u32(data, f, "irq")
                if irq not in (DE_IRQ, SGX_IRQ):
                    continue
                desc = f"irq {irq}"
            elif name == "sched_switch":
                continue
            elif name in names:
                desc = name
            else:
                continue
            m0, m1, cov = between(smp, ns - 1_000_000, ns)
            bw = f"DE {m0/cov*1e3:6.0f} GPU {m1/cov*1e3:6.0f} MB/s" if cov else ""
            print(f"  {(ns-ut)/1e6:+8.3f} ms cpu{cpu} {desc:12s} {bw}")


if __name__ == "__main__":
    main()
