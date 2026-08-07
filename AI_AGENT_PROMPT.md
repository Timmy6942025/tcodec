# TCodec — Current AI Agent Handoff

This repository contains a C reference codec for ARM-first 4:2:0 video coding.
Read `README.md`, `TODO.md`, `MASTER_PLAN.md`, `SPEC.md`, `BITSTREAM.md`,
`PROFILES.md`, `BENCHMARKS.md`, and `docs/FINISH_Tier1.md` before changing code.

## Current verified implementation

- Versioned TCV bitstreams: v0, v1, and explicit v2 quadtree payloads.
- v0/v1 compatibility, profiles/levels, RAP, CRC, range-coded entropy, B-frame
  reorder, DCT residual helpers, RDO-lite, luma SAO Band Offset, and malformed
  stream coverage are present in the source tree.
- The C harness registers 51 tests. `make test`/`make test-fast` run the bounded
  50-test regression with the 300-frame long-run explicitly skipped; `make test-full`
  runs all 51. Both test targets also run the native TCMX/TCMF packet-container
  checks and the FFmpeg H.264-in-MP4/fMP4 compatibility checks.
- `tools/tcmux_mp4.sh` is deliberately a compatibility bridge: it decodes TCV
  and re-encodes H.264 into ordinary ISO-BMFF so stock FFmpeg/ffplay can play it.
  It is not native TCV-in-MP4 carriage.
- `make soak-1080p` runs a generated 300-frame 1920×1080 correctness soak and
  reports the current serial decoder throughput honestly.
- Scalar/NEON parity and cross-build checks are available through the Makefile
  and `tools/parity_check.sh`.

## Do not claim without evidence

The current evidence does not establish the required real-content negative
BD-rate win versus x264, the ARM 60/30-fps decode targets, a sanitizer run on
this host, native TCV MP4 playback by stock FFmpeg, or a clean milestone commit
history. These are release gates, not documentation tasks.

## Useful commands

```sh
make clean && make -j2
make test
make test-mp4
make cross-test
make soak-1080p
./tools/parity_check.sh
```

Keep every change deterministic, integer-only in the codec path, backward
compatible unless the bitstream version changes explicitly, and covered by a
reproducible test. Update the final report only with measurements actually run.
