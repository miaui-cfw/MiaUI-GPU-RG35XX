#!/usr/bin/env python3
"""Decodes /proc/desampler dumps (desampler.ko) on the PC: for every frame in which the DE FIFO underflowed, the
sample where DE_OUTPUT_STAT first went non-zero and the per-interval DE / GPU / CPU / ALL DDR rates around it, next
to the average of the reference (normal) frames.

    desampler.py DUMP [DUMP...] [--around US]
"""
import sys
from typing import List, Tuple

KINDS = {0: "normal", 1: "UNDERFLOW", 2: "before-underflow", 3: "reference"}


def parse(path: str) -> List[Tuple[list, list]]:
    """Frames of one dump: [(header fields, samples)], a sample = [t_ns, irq, out, path, pc0..pc3]."""
    frames = []
    for line in open(path):
        if line.startswith("#"):
            continue
        p = line.split()
        if p[0] == "F":
            frames.append(([int(x) for x in p[1:]], []))
        elif frames:
            frames[-1][1].append([int(p[0]), int(p[1], 16), int(p[2], 16), int(p[3], 16)] + [int(x) for x in p[4:8]])
    return frames


def rates(s: list) -> List[Tuple[float, float, list, list]]:
    """Per-interval (start us, end us, MB/s of the 4 counters, sample) from consecutive samples."""
    out = []
    for a, b in zip(s, s[1:]):
        dt = b[0] - a[0]
        if dt > 0:
            out.append((a[0] / 1e3, b[0] / 1e3, [(b[4 + i] - a[4 + i]) * 16 / dt * 1e3 for i in range(4)], b))
    return out


def main() -> None:
    """Prints the underflow frames and the reference profile."""
    around = float(sys.argv[sys.argv.index("--around") + 1]) if "--around" in sys.argv else 600.0
    paths = [p for p in sys.argv[1:] if not p.startswith("--") and not p.replace(".", "").isdigit()]
    refs = []
    for path in paths:
        for hdr, s in parse(path):
            seq, vb, kind, n = hdr
            if kind == 3:
                refs.append(s)
                continue
            if kind not in (1, 2):
                continue
            first = next((i for i, x in enumerate(s) if x[2]), None)
            tot = [(s[-1][4 + i] - s[0][4 + i]) * 16 / 1e3 for i in range(4)] if s else []
            print(f"\n== {path} frame {seq} {KINDS[kind]} vblank {vb/1e9:.6f} s, {n} samples, frame totals KB "
                  f"DE/GPU/CPU/ALL {[round(x) for x in tot]}")
            if first is None:
                print("   (no underflow bit in this frame's samples)")
                continue
            t_uf = s[first][0] / 1e3
            print(f"   underflow bit first seen at {t_uf:.0f} us after vblank (previous sample {s[first-1][0]/1e3:.0f} us)"
                  if first else f"   underflow bit already set at the first sample ({t_uf:.0f} us)")
            for a, b, r, x in rates(s):
                if t_uf - around <= b <= t_uf + 150:
                    mark = " <== UF" if x[2] and (x is s[first]) else ""
                    print(f"   {a:7.0f}-{b:7.0f} us  m0 {r[0]:6.0f}  m1 {r[1]:6.0f}  m2 {r[2]:6.0f}  m3 {r[3]:6.0f}"
                          f"  irq {x[1]:x} out {x[2]:x} fcr {(x[3] >> 29) & 1}{mark}")
    if refs:
        print(f"\n== reference frames: {len(refs)}; mean MB/s per 250 us bin (DE / GPU / CPU / ALL)")
        bins = {}
        for s in refs:
            for a, b, r, x in rates(s):
                k = int(a // 250)
                bins.setdefault(k, []).append(r)
        for k in sorted(bins)[:28]:
            v = bins[k]
            m = [sum(x[i] for x in v) / len(v) for i in range(4)]
            mx = max(x[1] for x in v)
            print(f"   {k*250:5d}-{k*250+250:5d} us  DE {m[0]:5.0f}  GPU {m[1]:5.0f} (max {mx:5.0f})  CPU {m[2]:5.0f}  ALL {m[3]:5.0f}")


if __name__ == "__main__":
    main()
