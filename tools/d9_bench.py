#!/usr/bin/env python3
"""d9_bench.py — honest D9 decode benchmark (best-of-N, thread sweep).

Decodes EXISING .tcv streams (no encode) with the current build/tcdec,
reporting best-of-N internal FPS per thread count. Best-of filters the
foreign-load noise on shared boxes; medians are also shown.

Usage: python3 tools/d9_bench.py <stream.tcv> [frames] [reps]
"""
import re, subprocess, sys

TCDEC = "build/tcdec"

def bench(tcv, threads, reps):
    best, vals = 0.0, []
    for _ in range(reps):
        r = subprocess.run([TCDEC, "-t", str(threads), tcv, "/tmp/d9_bench.yuv"],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return None, f"rc={r.returncode} {(r.stdout + r.stderr)[-200:]}"
        m = re.search(r"FPS:\s+([\d.]+)", r.stdout + r.stderr)
        f = float(m.group(1))
        vals.append(f)
        best = max(best, f)
    vals.sort()
    med = vals[len(vals) // 2]
    return best, f"median={med:.1f} runs={','.join(f'{v:.1f}' for v in vals)}"

if __name__ == "__main__":
    tcv = sys.argv[1]
    reps = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    print(f"stream={tcv} reps={reps}")
    for t in [1, 2, 4]:
        best, info = bench(tcv, t, reps)
        print(f"  t={t}: best={best}fps {info}")
