# 4×4 Prediction Sub-Leaves (Sub-8 Split) — Design (2026-09-08)

## 0. Motivation (re-measured on current code, see §7)

Detail content (`in_to_tree`) was ~10× the bits at matched quality on
pre-deblock code, diagnosed as prediction granularity (min 8×8 vs x264
4×4). Residual granularity is NOT the gap: 4×4 TUs already exist inside
8×8 leaves. The gap is independent MODE/MV per 4×4.

## 1. Key design decision: sub-split, NOT depth+1

Full depth+1 (4px cells everywhere: STRIDE 16, tc_qt_index rewrite,
RC_CTX_QT_SPLIT+4 colliding with SAO_BAND=66, 2×2 chroma syntax,
341 nodes, both decoders + encoder) touches everything. REJECTED.

Instead: an 8×8 (depth-3) leaf may sub-split its LUMA PREDICTION into
four 4×4 quarters. Everything else stays 8px-granular:

- **Cells/geometry**: unchanged (8px cells, `px=ctu+cx*8`, `<<(2-depth)`,
  `tc_qt_index`, 85 nodes). Four sub-leaves share the parent cell:
  same MVP for all four, last-writer-wins grid update — deterministic
  both sides (same raster order), zero grid-code changes. Intra-CTU MVP
  chaining between quarters comes free.
- **Chroma**: shared per 8×8 (ONE chroma decision/recon, existing
  functions unchanged — mirrors H.264, where chroma is coarser than
  4×4 luma). 2×2 chroma syntax explicitly NOT needed. Shared chroma MC
  MV = parent mvp (deterministic; sub-leaves may be intra).
- **Residual**: per sub-leaf = exactly ONE 4×4 TU (existing 4×4 TU
  kernels fwd+inv already exist). No new transform code.
- **Deblock/SAO/ratectl**: CTU/frame-level, untouched (deblock already
  filters every 4×4 edge).
- **Contexts**: zero new RC contexts (split@3 reuses RC_CTX_QT_SPLIT+3
  = index 65; SAO_BAND = 66, no collision).

## 2. Syntax (tool-gated, airtight compat)

New tool bit (15 free — FRESH_SKIP reverted): `TC_TOOL_SUB4`.
Depth-3 `split=1` (today: bitstream error, decoder rejects) means
"four 4×4 luma sub-leaves + one shared 8×8 chroma" IFF the flag is set;
without the flag the rejection stands. Old streams cannot contain
split@3 → fully backward compatible, no version bump.

Per sub-leaf luma syntax = today's leaf syntax MINUS split flag MINUS
outer DCT flag (single 4×4 TU implied): mode bit, MVD-or-merge/skip,
16 coeffs + lastpos. Chroma syntax after the four sub-leaves = today's
8×8 chroma, bit-identical handling.

## 3. Records

- Encoder: `qt_node_t sub[256]` (reuse type, `split` unused),
  indexed `(parent_cy*8+parent_cx)*4+q`. +4KB/encoder instance.
- Serial decoder: no records (reads syntax directly) — parse only.
- Parallel decoder: mirror `node[85]` with `sub[256]` command records.
  Cmd coeff buffer already fits worst case (256×16 luma + unchanged
  chroma = 6144 < V2_CMD_MAX_COEFFS 16384). Sub-leaf luma flags implied
  (0 stored).

## 4. Encoder RDO

Depth-3 leaf: run today's 8×8 decision → cost8. Then sub-split trial:
for q in 0..3 run sub-decision (cu=4, quarter offsets, shared parent
cell for MVP, luma-only RDO — chroma excluded exactly like today's
inter candidates), replay 4 luma quarters, run EXISTING shared-chroma
decision once (MC-vs-ch_intra full RDO, mvp MV), total = Σsub + chroma.
Keep min(cost8, costsub). Save/restore recon+grid around the trial
(existing split-trial pattern, §qt_split).

Refactor needed: sub-decision reuses qt_leaf's candidate code with
explicit (cu,px,py,node) instead of depth-derived geometry; pre-leaf
snapshots (e->pre_*) must be per-4×4 (same scratch buffers, cu-sized
rects — qt_copy_rect is generic). Plus: single-4×4-TU residual path
for cu==4 (today ntu=cu/8=0 yields free residual — MUST handle).

Preset gating: ultrafast/fast never (bounded-structure precedent);
medium+ tries (variance pre-gate only if encode time demands it —
measure first, gate second).

Scope: P/Key frames first; BIDIR sub-split later if P wins (imbalance
is legal, just unexploited).

## 5. Decoder changes (3 sites, mirrors)

- Serial `qt_dec_split`: split@3 + flag → 4× `qt_dec_subleaf` + one
  `qt_dec_chroma` (existing). No flag → error (unchanged).
- Parallel parse + recon: same shape with sub records.
- Encoder write pass: mirror.

## 6. Risks

1. Sub-leaf candidate refactor touching the hot decision path —
   mitigate by pure code motion first (suite + parity + goldens prove
   bit-exactness), sub-split second.
2. Encode time (+50–100% depth-3 cost at medium) — preset-gated.
3. RDO may rarely pick it (4× header overhead) — gates decide; the
   detail-class measurement (§7) sizes the prize first.
4. MV-grid sharing coarsens MVP for quarters — acceptable (mirrored).

## 7. Opportunity measurement (current code)

2026-09-08 qp32 (`in_to_tree` 720p50 30fr, med, entropy+profile1):

| Codec | Bytes | PSNR-Y | SSIM |
|---|---|---|---|
| tcodecv2 | 49,337 | 30.90 | 0.7755 |
| x264vf | 34,083 | 31.24 | 0.8010 |

**The 10× premise is STALE**: post-deblock the detail gap is +45%
bytes / −0.34dB at qp32 — smaller than nature's +99.6% BD-rate. The
deblock fix (which stopped blurring every real edge in-loop) closed
most of it. Full 27/37 curve running; sub-4×4 build decision gated on
it (needs a systematic ≥40% gap to justify the project cost).

## 8. Verdict 2026-09-08: DO NOT BUILD (three independent kills)

Full curves: tree BD-rate **+76%** (177428@33.04 / 49337@30.90 /
17067@28.85 vs x264 77871@33.48 / 34083@31.24 / 18262@29.32).
Passes the §7 ≥40% gate numerically — but three findings kill the
premise that prediction granularity is the cause:

1. **x264 partition oracle** (tree, CRF32): partitions=none (16×16
   only) 32945@31.20 vs all (incl. 4×4) 33761@31.23 — sub-8×8 buys
   x264 ~NOTHING (+2.5% bytes, +0.03dB, CRF re-targets). If 4×4
   doesn't help x264 here, it won't help us here.
2. **Intra/inter diagnostic** (tree qp32 10fr): all-intra 9.15KB/fr
   vs inter 1.64KB/fr — motion already captures 5.6×; the gap is
   inter-residual quality, not spatial detail.
3. **x264 ablation battery** (tree+park, CRF32, identical quality):
   only CABAC matters (+13.5% tree / +11.4% park over CAVLC);
   8×8DCT, dia-ME, ref>1, mixed-refs, deblock, sub-partitions ALL
   ≈0%. x264's lead is decision QUALITY (RDO/RDOQ/CABAC
   accumulation), not tool checklist — and we've been adding tools.

Tree (+76%) is now our second-BEST nature class, not the worst.
The design (§1–6) is kept current: if a future content class shows a
partition-shaped gap, sub-split (shared cells/chroma, §1) is the
cheapest correct form. Next experiment: OUR byte breakdown on park
(mode/residual/header shares) — one instrumented encode.
