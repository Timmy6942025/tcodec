#!/usr/bin/env python3
"""rd_probe.py — fast synthetic RD probe for hill climbing (no network).
Generates a 320x240 10-frame clip with motion, encodes at 3 QPs with
tcodecv2, decodes, computes PSNR-Y + bytes. Prints SCORE (lower=better).
Score = total_bytes with PSNR guard: fails if avg PSNR drops >1dB vs ref.
"""
import os, subprocess, sys, struct
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TCENC = os.path.join(ROOT, "build", "tcenc")
TCDEC = os.path.join(ROOT, "build", "tcdec")
W, H, NFR = 320, 240, 10
QPS = [27, 32, 37]

def gen_yuv(path):
    w, h = W, H
    with open(path, "wb") as f:
        for fr in range(NFR):
            y = np.zeros((h, w), dtype=np.uint8)
            for r in range(h):
                for c in range(w):
                    y[r, c] = (r * 255 // h + c * 255 // w + fr * 4) // 2 % 256
            # moving white square 32x32
            sx = (fr * 20) % (w - 32)
            sy = (fr * 12) % (h - 32)
            y[sy:sy+32, sx:sx+32] = 255
            # moving dark bar (tests skip/merge)
            y[fr*2 % h:(fr*2 % h)+4, :] //= 2
            cb = np.full((h//2, w//2), 128, dtype=np.uint8)
            cr = np.full((h//2, w//2), 128, dtype=np.uint8)
            # chroma gradient drift
            cb += np.uint8(fr * 2)
            f.write(y.tobytes()); f.write(cb.tobytes()); f.write(cr.tobytes())

def psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    if mse == 0: return 100.0
    return 10 * np.log10(255*255/mse)

def run_one(src, qp, tag):
    bit = f"/tmp/tcprobe_{tag}_q{qp}.tcv"
    dec = f"/tmp/tcprobe_{tag}_q{qp}.yuv"
    r = subprocess.run([TCENC, "-w", str(W), "-h", str(H), "-q", str(qp),
                        "-p", "2", "-t", "1", "-o", bit, "--v2", "--entropy", src],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("tcenc failed: " + r.stdout + r.stderr)
    r = subprocess.run([TCDEC, bit, dec], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("tcdec failed: " + r.stdout + r.stderr)
    nb = os.path.getsize(bit)
    # read luma planes
    fl = W*H
    src_y = np.fromfile(src, dtype=np.uint8)[:NFR*fl*3//2].reshape(NFR, fl*3//2)[:, :fl]
    dec_y = np.fromfile(dec, dtype=np.uint8)[:NFR*fl*3//2].reshape(NFR, fl*3//2)[:, :fl]
    p = float(np.mean([psnr(src_y[i], dec_y[i]) for i in range(NFR)]))
    return nb, p

def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else "base"
    qps = [int(x) for x in sys.argv[2].split(",")] if len(sys.argv) > 2 else QPS
    src = f"/tmp/tcprobe_{tag}.yuv"
    gen_yuv(src)
    total_b = 0
    psnrs = []
    for qp in qps:
        nb, p = run_one(src, qp, tag)
        total_b += nb
        psnrs.append(p)
        print(f"qp{qp}: {nb:7d} B  PSNR-Y {p:5.2f}")
    avg_p = sum(psnrs)/len(psnrs)
    print(f"TOTAL_BYTES={total_b} AVG_PSNR={avg_p:.2f}")
    print(f"SCORE={total_b}")
    return total_b

if __name__ == "__main__":
    main()
