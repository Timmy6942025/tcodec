#!/usr/bin/env python3
"""hill_climb.py — greedy single-knob hill climber for TCodec v2 encoder.
Score = TOTAL_BYTES subject to AVG_PSNR within 0.5dB of baseline (else reject).
Usage: python3 tools/hill_climb.py
Baseline after RDO fix: ~16022 B @ 32.06 dB on synthetic probe.
"""
import os, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# v2-path mutations only (legacy knobs have no effect on --v2 probe)
MUTATIONS = [
    ("lambda-137to100", "src/quantize.c",
     "int64_t l = (s * s * 137) >> 10;",
     "int64_t l = (s * s * 100) >> 10;",
     "lower lambda -> favor quality"),
    ("lambda-137to200", "src/quantize.c",
     "int64_t l = (s * s * 137) >> 10;",
     "int64_t l = (s * s * 200) >> 10;",
     "higher lambda -> favor bits"),
    ("sao-bits-10to4", "src/encoder.c",
     "if (sao_cost + (int64_t)tc_lambda(qp) * 10 >= no_sao_cost)",
     "if (sao_cost + (int64_t)tc_lambda(qp) * 4 >= no_sao_cost)",
     "cheaper SAO model -> more SAO"),
    ("interbits-plus2", "src/encoder.c",
     "int bits_inter = 1 + 1 + 1 + 1 + 2 + (tc_bs_se_bits(disp.x)+tc_bs_se_bits(disp.y)) + lb;",
     "int bits_inter = 1 + 1 + 1 + 1 + 4 + (tc_bs_se_bits(disp.x)+tc_bs_se_bits(disp.y)) + lb;",
     "honester inter header cost"),
    ("mergebits-plus2", "src/encoder.c",
     "int bits_merge = 1+1+1+1+1+1+lb;",
     "int bits_merge = 1+1+1+1+1+3+lb;",
     "honester merge header cost"),
]

def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT, shell=True)

def probe(tag):
    r = sh(f"python3 tools/rd_probe.py {tag}")
    if r.returncode != 0:
        raise RuntimeError("probe failed: " + r.stdout[-1500:] + r.stderr[-1500:])
    total = avg = None
    for line in r.stdout.splitlines():
        if line.startswith("TOTAL_BYTES="):
            total = int(line.split("=")[1].split()[0])
        if "AVG_PSNR=" in line:
            avg = float(line.split("AVG_PSNR=")[1])
    return total, avg

def rebuild():
    return sh("make release -j4 2>&1 | tail -2")

def apply_patch(path, old, new):
    p = os.path.join(ROOT, path)
    with open(p) as f: src = f.read()
    assert src.count(old) == 1, f"patch not unique in {path}: count={src.count(old)}"
    with open(p, "w") as f: f.write(src.replace(old, new))

def main():
    base_bytes, base_psnr = probe("base")
    print(f"BASELINE bytes={base_bytes} psnr={base_psnr:.2f}")
    log = open(os.path.join(ROOT, "hill_log.csv"), "w")
    log.write("step,id,bytes,psnr,delta_pct,kept\n")
    step = 0
    for mid, path, old, new, desc in MUTATIONS:
        step += 1
        print(f"\n--- step {step}: {mid} ({desc}) ---")
        p = os.path.join(ROOT, path)
        with open(p) as f: orig = f.read()
        try:
            apply_patch(path, old, new)
        except AssertionError as e:
            print(f"SKIP: {e}"); log.write(f"{step},{mid},0,0,0,skip\n"); continue
        rebuild()
        try:
            b, ps = probe(f"mut{step}")
        except RuntimeError as e:
            print(f"FAIL, revert: {e}")
            with open(p, "w") as f: f.write(orig)
            rebuild(); log.write(f"{step},{mid},0,0,0,fail\n"); continue
        dbytes = (b - base_bytes) / base_bytes * 100
        dpsnr = ps - base_psnr
        keep = (b < base_bytes * 0.995) and (dpsnr > -0.5)
        print(f"bytes={b} ({dbytes:+.2f}%) psnr={ps:.2f} ({dpsnr:+.2f}dB) -> {'KEEP' if keep else 'REVERT'}")
        log.write(f"{step},{mid},{b},{ps:.2f},{dbytes:.2f},{int(keep)}\n"); log.flush()
        if keep:
            base_bytes, base_psnr = b, ps
            print(f"NEW BASELINE bytes={base_bytes}")
        else:
            with open(p, "w") as f: f.write(orig)
            rebuild()
    log.close()
    print(f"\nDONE final bytes={base_bytes} psnr={base_psnr:.2f}")

if __name__ == "__main__":
    main()
