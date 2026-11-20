#!/usr/bin/env python3
"""Aligns a d19.sh capture on the PC: PowerVR driver kprobe events (ftrace raw buffers) against desampler's 50 us
DE / GPU DDR samples (both local_clock), for every underflow frame and for the normal frames.

    d19_align.py CAPDIR
"""
import collections
import statistics
import sys
from typing import Dict, List, Tuple

import desampler as ds
import trace_decode as td

SHORT = {"SGXDoKickKM": "KICK", "SGXSubmitTransferKM": "XFER", "PVRSRVSwapToDCBufferKM": "SWAP",
         "PVRSRVSwapToDCBuffer2KM": "SWAP2", "ProcessFlip": "PFLIP", "OMAPLFBSwapHandler": "SWAPH",
         "PVRSRVCommandCompleteKM": "CMDDONE", "PVRSRVProcessQueues": "PROCQ",
         "SGXScheduleProcessQueuesKM": "SCHEDQ", "SGX_ISRHandler": "ISR", "SGX_MISRHandler": "MISR",
         "SGXTestActivePowerEvent": "APMTEST", "SGXPrePowerState": "PREPWR", "SGXPostPowerState": "POSTPWR",
         "PVRSRVSetDevicePowerStateKM": "SETPWR"}


def events(cap: str) -> List[Tuple[int, int, str]]:
    """All kprobe events (ns, cpu, short name), sorted by time."""
    ev = td.read_formats(cap)
    rows = []
    for c in range(4):
        rows += [(r[0], r[1], SHORT.get(r[2], r[2])) for r in td.decode_cpu(f"{cap}/raw{c}", c, ev)]
    rows.sort()
    return rows


def main() -> None:
    """Prints the per-frame timelines and the event timing statistics."""
    cap = sys.argv[1]
    ev = events(cap)
    ts = [e[0] for e in ev]
    frames = ds.parse(f"{cap}/ds.txt")
    print(f"{len(ev)} events {dict(collections.Counter(e[2] for e in ev))}; {len(frames)} kept frames")
    if ev:
        print(f"event span {ev[0][0]/1e9:.3f}..{ev[-1][0]/1e9:.3f} s; frames {frames[0][0][1]/1e9:.3f}.."
              f"{frames[-1][0][1]/1e9:.3f} s")
    import bisect
    phase: Dict[str, List[float]] = collections.defaultdict(list)
    for hdr, smp in frames:
        vb, kind = hdr[1], hdr[2]
        r = ds.rates(smp)
        i0, i1 = bisect.bisect_left(ts, vb), bisect.bisect_left(ts, vb + 17_000_000)
        fev = [(e[0] - vb) / 1e3 for e in ev[i0:i1]]
        for e in ev[i0:i1]:
            phase[e[2]].append((e[0] - vb) / 1e3)
        if kind != 1:
            continue
        uf = next((x for x in r if x[3][2] != 0), None)
        if uf is None:
            continue
        t_uf = uf[0]
        print(f"\n== UNDERFLOW frame {hdr[0]}: bit seen in {uf[0]:.0f}-{uf[1]:.0f} us after vblank")
        rows = [(x[0], f"DE {x[2][0]:5.0f} GPU {x[2][1]:5.0f}" + (" <== UF" if x is uf else "")) for x in r
                if t_uf - 1500 <= x[0] <= t_uf + 100]
        rows += [((e[0] - vb) / 1e3, f"      cpu{e[1]} {e[2]}") for e in ev[i0:i1]
                 if t_uf - 1500 <= (e[0] - vb) / 1e3 <= t_uf + 100]
        for t, s in sorted(rows):
            print(f"   {t:8.0f} us  {s}")
    print("\nevent times after vblank, all kept frames (us): name n median p10 p90")
    for k, v in sorted(phase.items()):
        v.sort()
        print(f"  {k:8s} {len(v):5d} {v[len(v)//2]:8.0f} {v[len(v)//10]:8.0f} {v[len(v)*9//10]:8.0f}")


if __name__ == "__main__":
    main()
