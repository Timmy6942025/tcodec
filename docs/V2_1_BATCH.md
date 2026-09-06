# TCodec v2.1 Syntax Batch — Design (2026-09-06)

These items all need bitstream additions. Batch them into ONE version bump
to avoid repeated compat breaks. None is started; this doc scopes them from
hill-climb audits. Order by value/effort.

## 1. v2 B-frame emission (biggest: +5–10% typical)

State: frame machinery (GOP4 reorder, scheduler, DPB poc logic) and leaf
BIDIR syntax (ref_sel/bi bits, parse+recon both decoder paths, grid rules)
exist, but `b_mode` is blocked for v2 (`encoder.c`: `!use_v2`) and v2 leaf
decisions never evaluate B candidates. Latent issues found by audit:
- Encoder replay chroma for BIDIR uses single ref-indexed MC, while luma
  averages fwd+bwd — decoder serial BIDIR chroma uses `dpb[0]` always.
  These three must be reconciled (recommend: average chroma like luma).
- Grid stores one MV (mvp+mvd); mirrored second MV derived, never stored —
  consistent on both sides, no change needed.
- `bf_sched_qp_off` returns 0; hierarchical QP lift (+1 mid, +2 outer)
  wanted with hill validation.
- Decision design: fwd (`dpb_find_poc_lt`), bwd (`dpb_find_poc_gt`),
  bi-average candidates with honest RDO; chroma follows luma choice.
- Tests: v2 GOP encode/decode roundtrip + display-order check (extend
  `test_b_frames` pattern), both entropy paths.

## 2. WHT transform-type flag (small: +1–3% on edges)

v2 `qt_code_luma` hardcodes DCT-II; legacy RDO picks WHT vs DCT per block.
Add 1 bit per TU (same pattern as `ref_sel`: unconditional when a new
`TC_TOOL_TRANSFORM_TYPE`-style gate is set... or fold into a v2.1 header
version bump). Encoder evaluates both (2× transform cost in decision,
medium+ only). Decoders already contain both kernels (parity-tested).

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

## Compatibility plan

- v2.1 = new payload version OR tool-gated optional syntax per item.
  Preference: tool-gated bits (decoder reads iff flag set; rejects
  unknown flags) so old v2 streams keep decoding.
- Golden v2 streams + conformance vectors must be captured BEFORE the
  bump (none exist today — create them first).
- BITSTREAM.md §7.6 documents each addition; bump the version tables.
