#!/usr/bin/env python3
"""rd_bench.py — rate/distortion benchmark runner for TCodec vs baselines.

Encodes one or more clips with TCodec and reference encoders (x264, x265,
SVT-AV1), decodes every bitstream back to raw YUV and measures PSNR-Y and
SSIM against the source. Results are appended to a CSV consumable by
tools/bd_rate.py and tools/plot_rd.py.

Usage:
  tools/rd_bench.py --clips park_joy,sintel_action --codecs tcodecv2,x264vf \\
                    --qps 22,27,32,37,42 --frames 120 --out results.csv

TCodec variants:
  tcodec   Legacy/default TCodec path (uses --tc-extra as supplied)
  tcodecv2 Bitstream-v2 quadtree path (adds --v2; entropy is forced by tcenc)

Clip sources come from benchmark_v2_real/CORPUS.tsv (FFV1 masters in
benchmark_v2_real/raw/).  --src/-w/-h/--fps can be used for an ad-hoc file.
"""
import argparse, csv, math, os, re, subprocess, sys, tempfile, time, shutil, hashlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAW = os.path.join(ROOT, "benchmark_v2_real", "raw")
SCRATCH = os.environ.get("TCODEC_SCRATCH", "/tmp/tcbench")
TCENC = os.path.join(ROOT, "build", "tcenc")
TCDEC = os.path.join(ROOT, "build", "tcdec")


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=isinstance(cmd, str),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          **kw)


def load_corpus():
    clips = {}
    path = os.path.join(ROOT, "benchmark_v2_real", "CORPUS.tsv")
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            p = line.split("\t")
            if len(p) < 9 or p[0] == "id":
                continue
            clips[p[0]] = dict(id=p[0], url=p[1], seek=p[3], dur=float(p[4]),
                               w=int(p[5]), h=int(p[6]), fps=int(p[7]), cls=p[8])
    return clips


def ensure_yuv(clip, frames):
    """Decode the FFV1 master to raw yuv420p in the scratch dir (cached)."""
    os.makedirs(SCRATCH, exist_ok=True)
    src = os.path.join(RAW, clip["id"] + ".mkv")
    if not os.path.exists(src):
        raise SystemExit("missing master: " + src)
    out = os.path.join(SCRATCH, "%s_%d.yuv" % (clip["id"], frames))
    if not os.path.exists(out):
        cmd = ["ffmpeg", "-nostdin", "-v", "error", "-i", src]
        if frames:
            cmd += ["-frames:v", str(frames)]
        cmd += ["-pix_fmt", "yuv420p", "-f", "rawvideo", "-y", out]
        r = sh(cmd)
        if r.returncode != 0:
            raise SystemExit(r.stdout.decode())
    fsz = os.path.getsize(out)
    nfr = fsz // (clip["w"] * clip["h"] * 3 // 2)
    return out, nfr


def _metric_mean(path, pattern):
    values = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            match = re.search(pattern, line)
            if match:
                value = float(match.group(1))
                if math.isinf(value) and value > 0:
                    # FFmpeg reports exact matches as +inf.  Keep CSV values
                    # finite and deterministic with the documented 8-bit
                    # reporting cap; NaN and negative infinity remain errors.
                    value = 100.0
                elif not math.isfinite(value):
                    raise RuntimeError("ffmpeg produced an invalid non-finite metric")
                values.append(value)
    if not values:
        raise RuntimeError("ffmpeg produced no metric samples")
    return sum(values) / len(values), len(values)


def psnr_ssim(ref, dist, w, h, nfr):
    """PSNR-Y / PSNR-avg / SSIM-all between raw yuv420p files.

    Exact 8-bit matches reported by FFmpeg as +inf are capped at 100 dB;
    NaN and negative infinity are rejected as invalid measurements.
    """
    common = ["-s", "%dx%d" % (w, h), "-pix_fmt", "yuv420p", "-f", "rawvideo"]
    with tempfile.TemporaryDirectory(prefix="tcbench-metrics-") as tmp:
        psnr_log = os.path.join(tmp, "psnr.log")
        ssim_log = os.path.join(tmp, "ssim.log")
        base = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error"]
        psnr_cmd = (base + common + ["-i", dist] + common + ["-i", ref,
                     "-lavfi", "[0:v][1:v]psnr=stats_file=" + psnr_log,
                     "-f", "null", "-"])
        ssim_cmd = (base + common + ["-i", dist] + common + ["-i", ref,
                    "-lavfi", "[0:v][1:v]ssim=stats_file=" + ssim_log,
                    "-f", "null", "-"])
        for cmd in (psnr_cmd, ssim_cmd):
            result = sh(cmd)
            if result.returncode != 0:
                raise RuntimeError(result.stdout.decode(errors="replace"))
        py, py_n = _metric_mean(psnr_log, r"\bpsnr_y:([0-9.+-]+|inf)\b")
        pa, pa_n = _metric_mean(psnr_log, r"\bpsnr_avg:([0-9.+-]+|inf)\b")
        ss, ss_n = _metric_mean(ssim_log, r"\bAll:([0-9.+-]+)\b")
    if py_n != nfr or pa_n != nfr or ss_n != nfr:
        raise RuntimeError("ffmpeg metric sample count mismatch")
    return py, pa, ss


# ── encoders ───────────────────────────────────────────────────────────

def enc_tcodec(src, out, clip, qp, nfr, opts, v2=False):
    cmd = [TCENC, "-w", str(clip["w"]), "-h", str(clip["h"]),
           "-f", str(clip["fps"]), "-q", str(qp), "-t", str(opts.threads),
           "-k", str(opts.keyint), "-p", str(opts.tc_preset), "-o", out]
    if nfr:
        cmd += ["-n", str(nfr)]
    cmd += opts.tc_extra
    if v2:
        cmd += ["--v2"]
    cmd += [src]
    t0 = time.time()
    r = sh(cmd)
    dt = time.time() - t0
    if r.returncode != 0:
        raise SystemExit("tcenc failed: " + r.stdout.decode())
    return dt


def dec_tcodec(bit, out, clip, threads):
    t0 = time.time()
    r = sh([TCDEC, "-t", str(threads), bit, out])
    dt = time.time() - t0
    if r.returncode != 0:
        # older tcdec has no -t
        t0 = time.time()
        r = sh([TCDEC, bit, out])
        dt = time.time() - t0
        if r.returncode != 0:
            raise SystemExit("tcdec failed: " + r.stdout.decode())
    return dt


def ff_encode(src, out, clip, args, nfr, keyint):
    cmd = ["ffmpeg", "-nostdin", "-v", "error",
           "-s", "%dx%d" % (clip["w"], clip["h"]), "-r", str(clip["fps"]),
           "-pix_fmt", "yuv420p", "-f", "rawvideo", "-i", src] + args
    if nfr:
        cmd += ["-frames:v", str(nfr)]
    cmd += ["-y", out]
    t0 = time.time()
    r = sh(cmd)
    dt = time.time() - t0
    if r.returncode != 0:
        raise SystemExit(" ".join(cmd) + "\n" + r.stdout.decode())
    return dt


def ff_decode(bit, out, clip):
    t0 = time.time()
    r = sh(["ffmpeg", "-nostdin", "-v", "error", "-i", bit,
            "-pix_fmt", "yuv420p", "-f", "rawvideo", "-y", out])
    dt = time.time() - t0
    if r.returncode != 0:
        raise SystemExit(r.stdout.decode())
    return dt


CODECS = {
    # name: (kind, ffmpeg args builder)
    "x264vf": ("ff", lambda qp, ki: ["-c:v", "libx264", "-preset", "veryfast",
                                     "-crf", str(qp), "-g", str(ki),
                                     "-keyint_min", str(ki), "-sc_threshold", "0",
                                     "-pix_fmt", "yuv420p", "-f", "matroska"]),
    "x264med": ("ff", lambda qp, ki: ["-c:v", "libx264", "-preset", "medium",
                                      "-crf", str(qp), "-g", str(ki),
                                      "-keyint_min", str(ki), "-sc_threshold", "0",
                                      "-pix_fmt", "yuv420p", "-f", "matroska"]),
    "x265": ("ff", lambda qp, ki: ["-c:v", "libx265", "-preset", "medium",
                                   "-crf", str(qp),
                                   "-x265-params",
                                   "keyint=%d:min-keyint=%d:scenecut=0:log-level=none" % (ki, ki),
                                   "-pix_fmt", "yuv420p", "-f", "matroska"]),
    "svtav1p6": ("ff", lambda qp, ki: ["-c:v", "libsvtav1", "-preset", "6",
                                       "-crf", str(qp), "-g", str(ki),
                                       "-pix_fmt", "yuv420p", "-f", "matroska"]),
    "svtav1p4": ("ff", lambda qp, ki: ["-c:v", "libsvtav1", "-preset", "4",
                                       "-crf", str(qp), "-g", str(ki),
                                       "-pix_fmt", "yuv420p", "-f", "matroska"]),
    "tcodec": ("tc", False),
    "tcodecv2": ("tc", True),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clips", default="")
    ap.add_argument("--src", default="")
    ap.add_argument("-w", type=int, default=0)
    ap.add_argument("-H", "--height", type=int, default=0)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--codecs", default="tcodec")
    ap.add_argument("--qps", default="22,27,32,37,42")
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--keyint", type=int, default=48)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--tc-preset", type=int, default=2)
    ap.add_argument("--tc-extra", default="--entropy")
    ap.add_argument("--tag", default="")
    ap.add_argument("--out", default="rd.csv")
    ap.add_argument("--keep", action="store_true")
    opts = ap.parse_args()
    opts.tc_extra = opts.tc_extra.split() if opts.tc_extra else []

    corpus = load_corpus()
    jobs = []
    if opts.src:
        cid = os.path.splitext(os.path.basename(opts.src))[0]
        clip = dict(id=cid, w=opts.w, h=opts.height, fps=opts.fps, cls="adhoc")
        jobs.append((clip, opts.src))
    for cid in [c for c in opts.clips.split(",") if c]:
        clip = corpus[cid]
        jobs.append((clip, None))

    os.makedirs(SCRATCH, exist_ok=True)
    out_parent = os.path.dirname(os.path.abspath(opts.out))
    os.makedirs(out_parent, exist_ok=True)
    new = not os.path.exists(opts.out)
    fout = open(opts.out, "a", newline="")
    wr = csv.writer(fout)
    if new:
        wr.writerow(["clip", "codec", "tag", "qp", "frames", "bytes",
                     "bitrate_kbps", "psnr_y", "psnr_avg", "ssim",
                     "enc_s", "dec_s", "enc_fps", "dec_fps"])

    for clip, srcpath in jobs:
        if clip["w"] <= 0 or clip["h"] <= 0:
            raise SystemExit("width and height are required and must be positive")
        if srcpath is None:
            srcpath, nfr = ensure_yuv(clip, opts.frames)
        else:
            fsz = os.path.getsize(srcpath)
            nfr = fsz // (clip["w"] * clip["h"] * 3 // 2)
            if opts.frames:
                nfr = min(nfr, opts.frames)
        if nfr <= 0 or clip["fps"] <= 0:
            raise SystemExit("source contains no complete frames or has invalid fps")
        dur = nfr / float(clip["fps"])
        for codec in opts.codecs.split(","):
            kind, mk = CODECS[codec]
            for qp in [int(q) for q in opts.qps.split(",")]:
                base = os.path.join(SCRATCH, "%s_%s_%s_q%d" %
                                    (clip["id"], codec, opts.tag or "d", qp))
                dec = base + ".yuv"
                if kind == "tc":
                    bit = base + ".tcv"
                    et = enc_tcodec(srcpath, bit, clip, qp, nfr, opts, v2=bool(mk))
                    dt = dec_tcodec(bit, dec, clip, opts.threads)
                else:
                    bit = base + ".mkv"
                    et = ff_encode(srcpath, bit, clip, mk(qp, opts.keyint), nfr,
                                   opts.keyint)
                    dt = ff_decode(bit, dec, clip)
                expected_dec_bytes = nfr * clip["w"] * clip["h"] * 3 // 2
                actual_dec_bytes = os.path.getsize(dec) if os.path.exists(dec) else 0
                if actual_dec_bytes != expected_dec_bytes:
                    raise SystemExit("decoded frame count mismatch: expected %d bytes, got %d" %
                                     (expected_dec_bytes, actual_dec_bytes))
                nb = os.path.getsize(bit)
                try:
                    py, pa, ss = psnr_ssim(srcpath, dec, clip["w"], clip["h"], nfr)
                except RuntimeError as exc:
                    raise SystemExit("metric calculation failed: " + str(exc))
                kbps = nb * 8 / dur / 1000.0
                wr.writerow([clip["id"], codec, opts.tag, qp, nfr, nb,
                             "%.1f" % kbps, "%.4f" % py, "%.4f" % pa, "%.5f" % ss,
                             "%.2f" % et, "%.2f" % dt,
                             "%.2f" % (nfr / et if et else 0),
                             "%.2f" % (nfr / dt if dt else 0)])
                fout.flush()
                print("%-16s %-9s qp%-3d %9d B  %8.1f kbps  PSNR-Y %6.2f  SSIM %.4f"
                      " enc %.1ffps dec %.1ffps" %
                      (clip["id"], codec, qp, nb, kbps, py, ss,
                       nfr / et if et else 0, nfr / dt if dt else 0))
                if not opts.keep:
                    for f in (dec,):
                        if os.path.exists(f):
                            os.remove(f)
    fout.close()


if __name__ == "__main__":
    main()
