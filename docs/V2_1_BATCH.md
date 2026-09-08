# TCodec v2.1 Syntax Batch — Design (2026-09-06)

These items all need bitstream additions. Batch them into ONE version bump
to avoid repeated compat breaks. Order by value/effort (item 0 first).

## 0. ~~Reference choice beats averaging~~ DONE 2026-09-08 (hill-climb 19)

B-frame selection mix on park water (1746 explicit inter leaves): fwd 30%,
bwd 40%, bi-average 3%, merge 27%. P multiref extended 2→4 refs (2-bit
`ref_idx`, same `MULTI_REF` flag): probe wins all QP, screen −9.4%/+0.61dB,
park −1.0%. Choice value confirmed; joint-search bi remains parked.

## 1. v2 B-frame emission (biggest: +5–10% typical)

State: frame machinery (GOP4 reorder, scheduler, DPB poc logic) and leaf
BIDIR syntax (ref_sel/bi bits, parse+recon both decoder paths, grid rules)
exist, but `b_mode` is blocked for v2 (`encoder.c`: `!use_v2`) and v2 leaf
decisions never evaluate B candidates. Legacy B measured ~parity with P
(+0.35%/−0.04dB; ladder fix −12.5%/−0.41dB diagonal) — v2-B needs B-RDO
maturity, not just emission. Audit 2026-09-07 found three latent v2-BIDIR
bugs (all unreachable today since emission is blocked):
- Encoder replay + both decoders resolve BIDIR refs by DPB SLOT
  (`ref_sel ? dpb[1] : dpb[0]`) instead of POC order (`dpb_find_poc_lt/gt`
  like legacy) — wrong under GOP reorder. Fix to poc-based first.
- The `bi` flag is written and parsed but IGNORED in replay/recon (always
  averages); single-dir B decodes as average. Must branch properly.
- BIDIR chroma is single-ref MC while luma averages — must average chroma
  like luma.
Plan: (1) poc-based refs everywhere, (2) honor bi flag + averaged chroma,
(3) qt_leaf BIDIR branch (fwd/bwd/bi RDO + merge-as-bi-average),
(4) unblock b_mode for v2 (hierarchical QP already fixed),
(5) test_v2_bframes (GOP roundtrip + order), validate park/screen/probe.

## 2. ~~WHT transform-type flag~~ REJECTED 2026-09-06

Legacy force-DCT is byte-identical to RDO (WHT never selected, even on
checkerboard) and forced-WHT measures +10%/-0.2dB: WHT is genuinely worse
here, not RDO blindness. No v2 work justified. Legacy keeps its harmless
RDO option (frozen path, not worth churning).

## 3. Per-leaf CfL alpha signaling (small)

Fixed `>>3` hurts negatively-correlated chroma (measured −0.41dB Cr).
2-bit alpha select per ch_intra leaf (off/>>4/>>3/>>2) or sign+mag.
Requires color-correlated validation content (probe has it now).

## 4. Chroma SAO + Edge Offset (medium)

Luma BO exists and won big (−17.5% screen). Chroma BO (same pattern,
separate flags) + luma EO (directional) are natural followers.

## 5. Skip prerequisites (unblocks +10–15% static)

Four failed trials documented in HILLCLIMB sessions 4–8. Needs BOTH:
(a) chroma-aware RDO (winner chroma coded in decision), (b) fresh (MC)
skip chroma instead of stale (syntax-neutral change, but alters old
streams — batch here).

Update 2026-09-07 (5th trial + static-content forensics): even fresh-chroma
skip loses (+10%/−0.14dB) because zeroing merge's epsilon starves future
references — frozen-content drift is −3.1dB/30fr (requant walk) vs x264
perfectly flat (skip exact-copies). Skip fundamentally needs:
(a) chroma-aware RDO, (b) fresh skip chroma, (c) PROPAGATION awareness
(don't zero residuals that future frames need as reference).

Update 2026-09-08 (6th trial, HILLCLIMB 34 — drift premise dead): the
deblock rewrite eliminated frozen drift (+0.30dB/30fr), but skip-6
(fresh chroma + chroma-honest RDO + ρ=1 guard + tool bit) still lost:
ρ=0 −30.7%/−5.06dB, ρ=1 −22.6%/−3.35dB, near-exact gate never fires.
Measured cause: mvp-MC SSE averages 550/cu² even on FROZEN content
(MVP divergence — zero-MVD skip predicts poorly), and lb crud inflates
alternatives. Skip needs (d) decent zero-MVD prediction (better MVP)
on top of (a–c). All reverted; 7th failure total. NOT next.

## 6. Per-CTU QP deltas + mb-tree-lite (unblocks adaptive allocation)

Frame-QP heuristics (bit-ratio ±, quality servo) all move diagonally —
adaptation without lookahead/propagation can't win. Concrete design
(scoped 2026-09-07):
- Syntax: 2-bit `ctu_qp_delta` at CTU start (before split flag), values
  {−1, 0, +1, +2} (00/01/10/11), tool-gated by new `TC_TOOL_QP_DELTA`
  (bit 15 free). Applies to all leaves in the CTU (eff tables already
  built per-CTU in all 3 decode paths). ~60B/frame overhead at 720p.
- Heuristic (no RDO, 1 pass): per-CTU stability from residual-energy
  history (240 int64s in enc state); stable background → −1 (protect
  references), changing → 0/+1. Key uses: static UI vs scroll regions
  (screen), outer-B +2 refinement.
- Must earn ~2% overhead; validate screen (static regions) + park
  (uniform-motion control). This unblocks skip (#5) and proper CRF.

## Compatibility plan

- v2.1 = new payload version OR tool-gated optional syntax per item.
  Preference: tool-gated bits (decoder reads iff flag set; rejects
  unknown flags) so old v2 streams keep decoding.
- Golden v2 streams + conformance vectors must be captured BEFORE the
  bump (none exist today — create them first).
- BITSTREAM.md §7.6 documents each addition; bump the version tables.
