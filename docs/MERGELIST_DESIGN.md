# v2.1 #7: Merge Candidate List — Design (2026-09-09, scoped NOT implemented)

## 0. Motivation (measured, not hypothesized)

- mvp-MC SSE averages 550/cu² even on FROZEN content (trial 34/44:
  spatial-median MVP diverges where neighbors disagree).
- MVD avg 27 qpel on park (BRKMVD), 3.9 on tree — neighbors often beat
  the median on water/texture.
- Merge wins 19–52% of P-leaves (WINSHARE) with a MEDIAN predictor;
  every merge using a diverged mvp pays residual for the divergence.
- Zero-syntax alternatives exhausted: more median voters doesn't fix
  disagreement (trial 44 analysis); temporal MVP needs DPB history
  (bigger project). A signaled choice among spatial candidates is the
  remaining shape.

## 1. Syntax (tool-gated, v2.1 batch vehicle)

New `TC_TOOL_MERGELIST` (bit 15 is free — FRESH_SKIP reverted; coordinate
with batch). On P merge leaves only (Bimai/BIDIR untouched initially):
after MERGE_FLAG=1, a 1-bit `merge_alt` (0 = median mvp as today,
bit-identical legacy behavior; 1 = alternate) + 1-bit `merge_which`
(0 = left-cell MV, 1 = above-cell MV) present iff alt=1. Worst case +2
bits on a merge leaf (today: 2 header bits); typical +0/1.
Old streams never set the tool bit → parse skips the fields → identical.

## 2. Candidate construction (encoder + decoder identical)

Grid cells store MV-minus-position (+intra flag). Absolute MV of a
neighbor cell = stored + its own position (same math as qt_mvp).
List: {median (existing qt_mvp), left (cx-1,cy or nearest available),
above (cx,cy-1 or nearest)}. Unavailable (intra/out-of-frame) entries
are dropped; if fewer than 2 available, alt coding is skipped for that
leaf (encoder and decoder apply the same availability rule —
deterministic). MVP median itself unchanged (all other paths intact).

## 3. RDO (encoder-only)

For each available entry: MC from dpb[0] (P) with entry MV, zero MVD,
qt_code_luma, honest bits (merge headers + idx bits). Keep min.
Merge evaluation cost ×2–3 (MC + transform per entry) — gate to
preset ≥ MEDIUM (fast keeps median-only merge; bounded-structure
precedent). Decoder cost: one extra MC select (negligible).

## 4. Decoder changes (3 sites)

- Serial + parallel parse: read alt/which iff MERGELIST flag && merge==1
  && ≥2 available (same availability rule).
- Serial + parallel recon + encoder replay: MV = selected entry (not
  always mvp) for merge leaves.
- Grid updates unchanged (store actual used MV — same as today).

## 5. Risks / gates

1. Availability-rule mismatch = desync (parity + recon-consistency +
   suite + fuzz must all pass; add a merge-list roundtrip test).
2. Prize uncertainty: estimate −1 to −4% nature (helps exactly where
   mvp diverges). Gates: probe + screen + park + vidyo (static bg!) +
   full suite. If <1% everywhere → revert (syntax churn unjustified).
3. Encode time (+merge evals) — measure; preset-gate contains it.
4. Interacts with future skip work (skip uses mvp; list could extend —
   deferred).

## 6. Sequencing

After (not with) any v2.1 batch decision: needs tool-bit allocation +
golden capture BEFORE (goldens current as of hill-23). Standalone
tool-gate is acceptable per batch compat plan if the batch slips.
