# TCodec — ARM-first research video codec

TCodec is a deterministic C research codec for 8-bit planar YCbCr 4:2:0 video.
It targets ARM/NEON deployment, but the current implementation remains a
prototype: its real-content compression and ARM real-time decode targets are
not yet met.

## Verified feature set

| Feature | Status |
|---|---|
| Versioned TCV packets | v0, v1, and explicit v2 quadtree payload |
| Intra prediction | 18 luma modes |
| Legacy transforms | 4×4/8×8 WHT for v0/v1 |
| v2 transforms | Integer residual DCT path with shared quantization |
| Inter prediction | Median-MV, skip/merge, multi-reference, 6-tap luma fallback |
| B-frames | Hierarchical GOP4 reorder and bi-prediction infrastructure |
| Entropy | Legacy Exp-Golomb path and context-modeled range-coded path |
| Filtering | Deblocking plus v2 luma SAO Band Offset |
| Rate control | CQP, CBR, and VBR prototype modes |
| ARM path | NEON dispatch and scalar/NEON parity checks |
| Regression suite | 51 codec tests plus container/integration tests |

Implemented does not mean competitive: see `BENCHMARKS.md` and
`docs/FINISH_Tier1.md` for measured limitations and gate status.

## Build and test

```sh
make                         # release library and CLI tools
make clean && make -j2       # reproducible clean build
make test                    # bounded 50-test regression + containers
make test-full               # all 51 codec tests, including 300-frame long run (slow)
make soak-1080p              # external 300-frame exact-byte soak
make test-mp4                # only the FFmpeg compatibility bridge test
make cross-test              # AArch64/NEON compile-only checks
./tools/parity_check.sh      # scalar/NEON and entropy parity checks
```

The MP4 bridge tests require `ffmpeg`, `ffprobe`, and Python 3. The core codec
and native TCMX tests do not require FFmpeg.

## CLI usage

```sh
./build/tcenc -w 1280 -h 720 -q 32 --v2 -o stream.tcv input.yuv
./build/tcdec stream.tcv output.yuv
```

A `.tcv` file is a sequence of 4-byte little-endian packet sizes followed by
TCV frame packets. See `BITSTREAM.md` for the versioned syntax.

## Native packet transport and ISO-BMFF

`build/tcmux` provides a packet-preserving private transport:

```sh
./build/tcmux mux -i stream.tcv -o stream.tcmx -f 30
./build/tcmux demux -i stream.tcmx -o roundtrip.tcv
./build/tcmux segment -i stream.tcmx -o segments -s 60 -p playlist.m3u8
```

The native MP4 commands preserve the actual TCV access units in an ISO-BMFF
file using a private `tcv1` sample entry:

```sh
./build/tcmux mp4mux -i stream.tcv -o native.mp4 -f 30
./build/tcmux mp4demux -i native.mp4 -o recovered.tcv
cmp stream.tcv recovered.tcv
```

`native.mp4` is a standards-shaped carriage file, not a stock-player format:
FFmpeg/ffplay cannot decode the private `tcv1` codec without a TCodec decoder
integration. The native round trip is byte-exact and is covered by
`tools/test_tcmux.sh`.

For ordinary FFmpeg/ffplay playback, use the explicit compatibility bridge.
It decodes TCV and lossy re-encodes the frames as H.264 in MP4:

```sh
tools/tcmux_mp4.sh mux -i stream.tcv -o playable.mp4 -w 1280 -h 720 -f 30
tools/tcmux_mp4.sh demux -i playable.mp4 -o recovered.yuv
ffplay playable.mp4
tools/tcmux_mp4.sh segment -i stream.tcv -o hls -w 1280 -h 720 -f 30 -s 6
ffplay hls/playlist.m3u8
```

The bridge stages outputs and refuses to overwrite existing files/directories.
Its HLS output is fMP4 and is validated by FFmpeg in the integration test.

## 300-frame 1080p correctness soak

```sh
make soak-1080p
# short smoke: TCODEC_SOAK_FRAMES=2 make soak-1080p
```

The runner generates a 1920×1080 source, encodes v2 with range coding, decodes
it with the current serial `tcdec` CLI, and checks the exact decoded byte count.
It reports throughput but does not claim the unmet Tier-1 60-fps@720p or
30-fps@1080p target.

## Tests

`test/test_tcodec.c` currently registers 51 tests covering color conversion,
round trips, QP behavior, motion, multi-reference, v0/v1 compatibility, v2
quadtree raw/range paths, malformed streams, bit flips, B-frame reorder, WPP
parity, transforms, SAO, rate control, and deterministic output.

`make test` is the bounded default: it runs 50 tests and explicitly reports the
single skipped 300-frame in-process long run. `make test-full` runs all 51.
Both targets additionally run malformed-input and exact-round-trip tests for
TCMX/TCMF, native private `tcv1` MP4 carriage, and the H.264 MP4/fMP4
compatibility bridge. `make soak-1080p` is the separate reproducible 300-frame
1080p correctness runner.

## Documentation map

- `SPEC.md` — implemented codec behavior and limitations
- `BITSTREAM.md` — v0/v1/v2 syntax and error handling
- `PROFILES.md` — profiles, levels, tools, and presets
- `BENCHMARKS.md` — measured performance and benchmark methodology
- `TODO.md` — checked implementation work and explicitly deferred research
- `MASTER_PLAN.md` — long-term roadmap and current-state reality check
- `docs/FINISH_Tier1.md` — final gate assessment and reproducible evidence
- `AI_AGENT_PROMPT.md` — concise handoff for future coding sessions

## License

MIT
