# TCodec Master Plan

## Purpose

This document is the build plan for turning TCodec from a working prototype into a serious production-grade codec research program.

The target vision is aggressive:

- Visually transparent or near-transparent compression on real-world film, animation, live video, and mobile content
- Much lower delivery bitrate than typical H.264, H.265/HEVC, and AV1 deployments at comparable perceived quality
- Decoder operation on Raspberry Pi-class ARM devices and mid-range phones
- Material reduction in movie streaming bandwidth and mobile data usage

This plan is intentionally comprehensive and long-horizon. It is designed to be buildable, not easy.

---

## Non-Negotiable Reality Check

If the goal is to be "miles ahead" of H.264, H.265, and AV1, the project must be treated as a multi-year codec R&D program, not as a normal feature roadmap.

Important constraints:

- Beating mature AV1 and HEVC encoders on rate-distortion is extremely difficult.
- Decoder efficiency on small ARM devices sharply limits algorithm complexity.
- "10-20:1 with minute/none quality loss" is possible relative to raw YUV/RGB sources, but not automatically relative to already well-tuned H.264/H.265/AV1 encodes.
- Claims must be expressed against clearly defined baselines:
  - Raw source to coded bitrate ratio
  - Matched-VMAF bitrate reduction vs x264
  - Matched-VMAF bitrate reduction vs x265
  - Matched-VMAF bitrate reduction vs SVT-AV1/libaom

This plan therefore separates:

- Baseline engineering goals
- Competitive goals
- Stretch research goals

---

## Vision Statement

TCodec should evolve into:

- A low-power, ARM-first, perceptually optimized hybrid video codec
- A streaming-oriented codec with strong decoder simplicity
- A codec whose compression gains come from better decisions, better temporal modeling, better entropy coding, and better perceptual control, not from marketing language

The most credible strategic angle is:

- Aim to beat common deployed H.264 streams significantly
- Approach or selectively beat practical HEVC deployments
- Compete with fast AV1 deployment settings first
- Only then chase top-tier AV1 offline encoders

---

## Strategic Goal Hierarchy

### Tier 1: Must Achieve

- Stable, deterministic, fully tested bitstream
- Strong encode/decode correctness under fuzzing and long-run testing
- Meaningful gains over naive H.264 presets at equal perceptual quality
- Real-time or near-real-time decode on Raspberry Pi 4 / 5 and mid-range ARM phones
- Sensible containerization, streaming segmentation, and integration story

### Tier 2: Competitive Target

- 20-40% bitrate savings vs practical H.264 streaming encodes at equal VMAF
- 10-20% bitrate savings vs practical HEVC streaming encodes at equal VMAF
- Competitive decode power and memory footprint on ARM

### Tier 3: Stretch Target

- Competitive with fast or medium AV1 presets for streaming
- Better visual stability than common AV1 deployments on hard mobile/network budgets
- Niche dominance on ARM-constrained playback pipelines

### Tier 4: Moonshot

- Meaningful average BD-rate win over top-tier AV1/HEVC across diverse content while remaining cheaper to decode on ARM

This is not impossible, but it is a research outcome, not a schedule promise.

---

## Target Product Definitions

TCodec should eventually support three product modes:

### 1. Streaming Mode

- Primary commercial target
- Decoder optimized for phones, tablets, TVs, and Raspberry Pi-class clients
- Strong random access, recovery, and packaging support
- Good perceptual quality at bandwidth-constrained bitrates

### 2. Archive / Studio Mode

- Offline encoding
- Much higher encoder complexity budget
- Stronger lookahead, deeper search, better perceptual optimization

### 3. Low-Power Mobile Mode

- Decoder prioritizes battery, thermals, and bounded memory
- Reduced toolset subset if needed
- Optional layered bitstream/profile strategy

---

## Success Metrics

All work should be judged by hard metrics.

### Compression Metrics

- BD-rate vs x264
- BD-rate vs x265
- BD-rate vs SVT-AV1 and/or libaom
- Average bitrate reduction at fixed VMAF targets: 90, 93, 95, 97
- Average bitrate reduction at fixed SSIM/MS-SSIM

### Visual Metrics

- VMAF
- VMAF NEG
- SSIM
- MS-SSIM
- PSNR Y, U, V
- Temporal flicker/stability metrics
- Subjective review on film grain, gradients, skin, subtitles, animation edges, dark scenes, motion blur

### Performance Metrics

- Decode FPS on Raspberry Pi 4, Pi 5, and representative Android phones
- Encode FPS by preset on ARM laptop/desktop and server hardware
- CPU utilization
- Memory bandwidth
- Peak RSS
- Battery/power use on mobile
- Decoder startup latency

### Streaming Metrics

- Segment boundary quality stability
- Seek latency
- Packet loss resilience
- Recovery time after corruption
- Bitrate variability

---

## Development Principles

1. Never claim gains without reproducible benchmarking.
2. Decoder complexity is a hard constraint, not an afterthought.
3. Every coding tool must earn its keep in:
   - BD-rate
   - ARM decode cost
   - memory cost
   - implementation complexity
4. The bitstream must be profile-driven so the decoder can remain small for mobile deployments.
5. Perceptual quality matters more than PSNR-chasing.
6. Prototype code and production code are separate maturity stages.

---

## Current State Summary

Current repository status, at a high level:

- Working research prototype with versioned v0/v1/v2 bitstreams, intra/inter modes,
  v2 quadtree coding, transforms, quantization, deblocking, and luma BO SAO
- NEON-oriented implementation direction with scalar/NEON parity coverage
- 51 codec regression tests plus native TCMX/TCMF and playable MP4/fMP4 bridge tests
- Range-coded entropy is implemented and selected by tool flag; deeper compression
  gains and broad competitive measurements remain open
- WPP infrastructure and B-frame reorder exist, but real-time ARM decode targets
  are not met by the current measured implementation
- Motion estimation, mode decision, rate control, and filtering remain
  research/prototype-grade despite the implemented baseline features
- The MP4 integration currently has two explicit paths: packet-preserving private
  TCMX/TCMF transport plus native private `tcv1` ISO-BMFF carriage, and a separate
  H.264-in-ISO-BMFF compatibility bridge. Native `tcv1` carriage round-trips
  byte-exactly; stock-player decoder registration remains future work.

This means the project needs both:

- Codec science work
- Systems engineering work

---

## Program Structure

The roadmap is divided into ten major workstreams:

1. Core correctness and infrastructure
2. Benchmark and dataset program
3. Bitstream and profile design
4. Transform, quantization, and entropy coding
5. Spatial prediction and partitioning
6. Temporal prediction and motion modeling
7. In-loop restoration and perceptual quality tools
8. Rate control and streaming behavior
9. ARM/mobile decoder optimization
10. Ecosystem, packaging, and deployment

Each workstream has phases, deliverables, and exit criteria.

---

## Phase 0: Reset the Ground Truth

### Objective

Convert the project from a promising prototype into a measurable engineering program.

### Tasks

- Freeze a documented reference bitstream version.
- Audit all README claims against the actual implementation.
- Define explicit codec terminology:
  - coding tree unit
  - transform unit
  - prediction unit
  - block mode
  - profile
  - level
- Add architecture diagrams for encoder, decoder, and bitstream syntax.
- Create a golden corpus of encoded samples and decoded outputs.
- Add deterministic encode/decode tests across architectures.
- Define failure policies for malformed bitstreams.

### Deliverables

- `SPEC.md`
- `BITSTREAM.md`
- `BENCHMARKS.md`
- `PROFILES.md`
- Golden sample directory and hash manifest

### Exit Criteria

- Clean, versioned bitstream spec
- Reproducible outputs across repeated runs
- No ambiguity about which tools are real versus planned

---

## Phase 1: Benchmark Harness Before Feature Work

### Objective

Build the evaluation system before trying to "beat" anything.

### Required Dataset Classes

- High-motion live action
- Low-motion drama
- Animation/anime
- Grain-heavy film scans
- Dark scenes
- HDR source set later
- Screen content and subtitles
- Sports
- Talking heads/mobile camera footage
- User-generated social video

### Tooling to Build

- Automated encode matrix runner
- Automatic decode verification
- VMAF/SSIM/MS-SSIM/PSNR extraction
- BD-rate calculation scripts
- Rate-distortion curve plotting
- Speed and power measurement harness
- ARM device benchmark runner

### Baselines to Include

- x264: veryfast, medium, slow
- x265: medium, slow
- SVT-AV1: fast, medium, slow
- libaom AV1: reference/offline
- Optional VP9 baseline for deployment comparisons

### Deliverables

- One-command benchmark suite
- CSV/JSON results output
- Plots and comparison reports
- Device-specific performance dashboards

### Exit Criteria

- Any codec change can be tested against standard baselines within hours, not weeks

---

## Phase 2: Rebuild the Bitstream for Longevity

### Objective

Design a bitstream that can support future tools without constant breakage.

### Required Capabilities

- Versioned syntax
- Profiles and levels
- Tool flags and optional syntax sections
- Random access points
- Recovery points
- Error detection and optional CRC
- Tile/slice structure
- Parallel-friendly syntax
- Optional metadata side data

### Design Decisions to Make

- Frame header redesign
- Tile and slice syntax
- Reference picture signaling
- Optional global tables
- Context reset points
- Packetization for transport
- Support for future layered coding

### Deliverables

- Syntax tables
- Bitstream examples
- Decoder conformance suite

### Exit Criteria

- New tools can be introduced without destabilizing old decoders

---

## Phase 3: Entropy Coding Overhaul

### Objective

Replace prototype-grade coding with a serious entropy layer.

### Why

Entropy coding is one of the largest gaps between prototype and competitive codec performance.

### Candidate Approaches

- Binary arithmetic coding
- Range coder
- rANS
- tANS
- Hybrid context-coded ANS

### Recommended Path

1. Implement a robust range coder or rANS baseline first.
2. Add context modeling around:
   - block mode
   - significance maps
   - last-nonzero position
   - coefficient level classes
   - motion vector components
   - skip/merge flags
3. Evaluate tANS only if:
   - it materially improves throughput
   - memory behavior remains mobile-friendly
   - context adaptation stays simple enough

### Specific Syntax Targets

- Coefficient significance maps
- Run-length/sigmap alternatives
- Context-coded last-position
- Separate DC/low/high-frequency models
- Adaptive MV residual coding
- CABAC-like gains without CABAC-level decoder pain

### Exit Criteria

- Major BD-rate drop versus current Exp-Golomb path
- Decoder still fits mobile complexity budget

---

## Phase 4: Transform and Quantization Research Program

### Objective

Replace the current prototype transform strategy with a production-capable transform and quantization stack.

### Work Areas

#### Transform Family

- True residual transforms, not just prototype-friendly transforms
- 4x4, 8x8, 16x16, possibly 32x32
- DCT-II, DST-like, identity, and directional transforms where useful
- Content-adaptive transform selection

#### Partition/Transform Coupling

- Transform size tied to residual structure, not just variance
- Rate-distortion-driven transform selection
- Separate luma and chroma decisions

#### Quantization

- Proper scaling matrices
- Frequency-dependent quantization
- Chroma perceptual tuning
- Activity masking
- Texture masking
- Grain preservation logic
- Quantization group deltas

#### Perceptual Models

- Block visibility thresholds
- Edge-aware masking
- Temporal masking
- Film grain retention versus re-synthesis

### Stretch Research

- Learned quantization guidance from lightweight models used encoder-side only

### Exit Criteria

- Significant gains on textured, grainy, and dark content
- No obvious ringing or over-smoothed gradients

---

## Phase 5: Partitioning and Spatial Prediction

### Objective

Go beyond simple block variance heuristics without blowing up decoder cost.

### Required Improvements

- Better block partition structures
- Intra mode expansion
- Better intra reference handling
- Chroma-from-luma-style prediction ideas
- Edge/line-based predictors
- Intra smoothing rules
- Block skip and zero-residual signaling

### Partitioning Options

- Quadtree
- Quadtree plus binary splits
- Restricted partition grammar for decoder simplicity
- Fast encoder pruning heuristics

### Encoder Strategy

- Full RD search only in slow presets
- Aggressive pruning in mobile/fast presets
- Cache-friendly candidate ordering

### Exit Criteria

- Material gain on high-detail still regions and low-motion scenes

---

## Phase 6: Temporal Prediction and Motion System Redesign

### Objective

Build an inter prediction engine that is genuinely competitive.

### Current Priority

Fix motion search correctness first, then scale sophistication.

### Required Capabilities

- Search around collocated block positions
- Multiple reference frames
- Better MV predictors
- Merge/skip modes
- Motion vector candidate lists
- Better sub-pel interpolation
- Hierarchical motion fields
- Long-term references
- Scene cut detection

### Interpolation Options

- Bilinear as baseline only
- 6-tap or 8-tap interpolation filters
- Separate luma/chroma interpolation rules
- SIMD-optimized filter kernels

### Temporal Tool Candidates

- Global motion compensation
- Affine motion for camera pans/zooms
- Bi-prediction if decoder budget allows
- Compound prediction
- Residual reuse ideas

### Encoder-Side Strategy

- Deep lookahead in slow presets
- Coarser lookahead in fast presets
- Motion field reuse
- Pyramid search

### Exit Criteria

- Strong gains on live action and sports
- Inter frames show real advantage over intra-heavy fallback behavior

---

## Phase 7: In-Loop Filtering and Restoration

### Objective

Match modern codec visual cleanliness without excessive decode cost.

### Required Layers

- Smarter deblocking
- Direction-aware deringing
- Sample adaptive offset-like correction
- Lightweight restoration filter
- Chroma artifact cleanup

### Design Goal

Keep the decoder simpler than AV1 while recovering a large share of the subjective quality benefit of:

- deblocking
- deringing
- local restoration

### Candidate Architecture

1. Deblock
2. Directional dering
3. Low-cost restoration
4. Optional grain synthesis pass

### Film Grain Strategy

Bandwidth savings for movies often depend on not coding natural grain literally.

Plan:

- Detect grain-heavy content
- Code a cleaner base signal
- Re-synthesize controlled grain at decode
- Preserve cinematic texture without paying full bitrate

This can be one of the most important bandwidth wins for movie streaming.

### Exit Criteria

- Fewer ringing/blocking artifacts
- Better dark-scene stability
- Better filmic texture retention

---

## Phase 8: Rate Control, Lookahead, and Streaming Behavior

### Objective

Move from prototype QP control to streaming-grade perceptual bitrate control.

### Core Features

- Real lookahead
- Shot and scene change detection
- VBV-constrained bitrate control
- Per-scene complexity modeling
- Perceptual target tracking
- Keyframe placement optimization
- Segment boundary awareness for HLS/DASH

### Metrics To Optimize

- VMAF consistency over time
- Fewer bitrate spikes
- Better startup quality
- Stable quality at constant network envelopes

### Product Modes

- CBR for constrained live/streaming
- capped VBR for VOD
- CQ/VMAF-targeted offline mode

### Stretch Features

- Per-title encoding
- Ladder generation for adaptive streaming
- Content-aware preset selection

### Exit Criteria

- Smooth bitrate behavior
- Reliable target adherence
- Better mobile data efficiency on real streaming workloads

---

## Phase 9: ARM Decoder and Mobile Systems Engineering

### Objective

Make the codec truly usable on Raspberry Pi and mid-range phones.

### Hard Constraints

- Small memory footprint
- Predictable cache behavior
- High SIMD occupancy
- Low branchiness
- Stable thermals
- Low wakeups and threading overhead

### Work Areas

#### SIMD

- Full NEON kernels for hot paths
- Measured scalar fallback behavior
- Alignment-aware frame layouts
- Prefetch strategy

#### Threading

- Real WPP or tile-row parallel decode
- Thread pool integration
- Avoid false sharing
- Small-core friendly scheduling

#### Memory

- Scratch buffer reuse
- DPB minimization
- Bounded temporary allocations
- Bandwidth-aware layout

#### Platform Integration

- Android NDK decoder path
- Raspberry Pi benchmark and demo player
- Optional GPU interop for display path

### Device Targets

- Raspberry Pi 4
- Raspberry Pi 5
- Mid-range Android phone with ARMv8 cores
- Older low-power ARM target for regression tracking

### Exit Criteria

- Real-world decode targets hit on target devices
- No thermal collapse after long playback

---

## Phase 10: Packaging, Ecosystem, and Deployment

### Objective

Make TCodec usable outside the lab.

### Required Deliverables

- Packet-preserving TCMX/TCMF transport plus tested H.264-in-MP4 compatibility bridge
- Keyframe-aligned TCMF segments and a tested HLS/fMP4 compatibility playlist bridge
- FFmpeg integration path through the compatibility bridge; native TCV sample-entry
  and libavcodec wrapper remain future work
- Reference encoder/decoder CLI
- Decoder library API
- Player integration demo
- Conformance bitstreams

### Ecosystem Tasks

- Browser story
- Native mobile SDK
- Server-side packager/transcoder integration
- Monitoring and telemetry
- Documentation for deployment teams

### Business Relevance

If the target is reducing streaming bandwidth, deployment practicality matters as much as compression gains.

---

## Preset and Profile Strategy

Do not build one monolithic codec mode.

### Profiles

- `baseline-mobile`
- `streaming-main`
- `archive-high`
- `grain-cinema`

### Presets

- `ultrafast`
- `fast`
- `medium`
- `slow`
- `veryslow-research`

### Rules

- Decoder profiles decide supported syntax/tools
- Encoder presets decide search effort and tool exploration depth

This separation is essential to stay deployable on constrained devices.

---

## Research Bets Worth Pursuing

These are optional but potentially high upside.

### 1. Encoder-Side Learned Decision Support

Use small ML models only on the encoder side for:

- partition prediction
- transform candidate ranking
- mode pruning
- perceptual bit allocation

This can improve compression without increasing decoder cost.

### 2. Grain Modeling and Synthesis

Very high leverage for movies and prestige TV content.

### 3. Content-Adaptive Tool Activation

Turn expensive tools on only when content benefits enough.

### 4. Hierarchical Perceptual Rate Allocation

Use scene, CTU, and block-level saliency/activity models.

### 5. Hybrid Classical + Lightweight Neural Restoration

Only if decoder budget still works on ARM.
This should be explored cautiously.

---

## Research Bets To Treat Cautiously

These can easily destroy feasibility.

- Heavy decoder-side neural tools
- Very large transform vocabularies
- Overly complex partition trees
- CABAC-level context complexity without enough gain
- B-frames and compound tools that overwhelm ARM decode budgets
- Feature creep without benchmarking discipline

---

## Validation and Quality Program

### Automated Validation

- Unit tests
- Bitstream syntax tests
- Decoder mismatch tests
- Cross-platform determinism tests
- Fuzzing
- Differential encode/decode tests
- Long-run soak tests

### Visual QA

- Artifact gallery
- Side-by-side frame viewer
- Temporal playback comparisons
- Hard-content review playlist

### Failure Cases To Track Explicitly

- Banding
- Blocking
- Ringing
- Flicker
- Mosquito noise
- Subtitle damage
- Skin tone instability
- Film grain destruction
- Motion wobble
- Error propagation after packet corruption

---

## Benchmark Governance

Every major codec change should answer:

- What content classes improved?
- What content classes regressed?
- How much decoder cost increased?
- How much memory increased?
- Did mobile thermal behavior worsen?
- Is the gain still present against strong baselines?

Any tool that does not justify itself should be removable behind a flag or deleted.

---

## Concrete Milestone Roadmap

## Milestone A: Prototype to Trustworthy Reference

### Deliverables

- Correct motion search
- Real spec docs
- Benchmark harness
- Conformance tests
- Bitstream versioning

### Outcome

You have a trustworthy reference codec.

## Milestone B: First Competitive Streaming Codec

### Deliverables

- Better entropy coding
- Better inter prediction
- Better partitioning
- Better rate control
- Real ARM decode measurements

### Outcome

You can credibly target beating common H.264 deployments.

## Milestone C: HEVC-Class Competitor

### Deliverables

- Stronger transform/quant stack
- Multi-ref inter
- restoration pipeline
- perceptual control
- mature presets

### Outcome

You can target selective wins versus practical HEVC use cases.

## Milestone D: AV1 Challenger

### Deliverables

- Deep lookahead
- advanced temporal tools
- grain strategy
- encoder-side learned guidance
- full mobile deployment profiles

### Outcome

You can attempt serious AV1-class comparisons.

## Milestone E: Commercial Readiness

### Deliverables

- Packaging
- FFmpeg integration
- mobile SDK
- playback demos
- deployment docs

### Outcome

The codec becomes usable for real streaming trials.

---

## Suggested Execution Order

This is the order that gives the best chance of success:

1. Fix correctness and benchmarking
2. Redesign bitstream for extensibility
3. Replace entropy coder
4. Fix and deepen motion system
5. Improve partitioning and RD decisions
6. Improve transforms and quantization
7. Add in-loop restoration and grain tools
8. Mature rate control and streaming behavior
9. Fully optimize ARM decode
10. Package and integrate

Do not invert this order.

If benchmarking comes late, the entire program will drift.

---

## Resource Estimate

This is realistically a multi-person, multi-phase effort.

### Minimal Serious Team

- 1 codec architect
- 1 systems/bitstream engineer
- 1 ARM SIMD performance engineer
- 1 tooling/benchmark engineer
- 1 subjective quality / streaming integration engineer

### Timeline Reality

- Prototype stabilization: months
- First real H.264 competition: 6-18 months
- Serious HEVC competition: 1-3 years
- Credible AV1 challenge: multi-year research effort

One strong engineer can move it far, but "miles ahead of AV1" is not a solo short-term milestone.

---

## What "Unbelievable Bandwidth Reduction" Would Actually Look Like

A credible commercial success story does not need magic.

Examples:

- 30-50% lower bitrate than deployed H.264 ladders at same perceived quality
- 15-25% lower bitrate than practical HEVC ladders in some content classes
- Similar perceptual quality to common AV1 deployment presets at lower decode cost
- Film-grain-aware movie streams that preserve look while cutting bitrate substantially

That alone would already be extremely valuable.

If the codec eventually exceeds that, the benchmark data will prove it.

---

## Immediate Next Actions

The next implementation work should be:

1. Create formal spec documents for current syntax and profiles.
2. Build a proper benchmark harness against x264, x265, and SVT-AV1.
3. Fix motion estimation so search is centered correctly and frame bounds are real.
4. Replace coefficient Exp-Golomb coding with a serious entropy coder.
5. Wire actual threaded decode/encode execution and measure ARM scaling.
6. Add objective quality reporting and artifact review workflows.

---

## Final Guidance

The way to make TCodec "miles ahead" is not to add random complexity. It is to be ruthless about:

- measurement
- decoder cost discipline
- perceptual quality
- ARM-first implementation
- removing weak tools
- doubling down on tools that buy real BD-rate

The fastest path to a great codec is:

- fix the foundation
- benchmark constantly
- evolve the bitstream carefully
- attack the biggest compression gaps first

If executed well, TCodec can become a serious low-power streaming codec program. Whether it beats AV1 broadly will be decided by data, not intention.
