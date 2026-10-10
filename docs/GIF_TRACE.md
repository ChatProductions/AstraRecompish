# Serialized GIF capture and replay

This host diagnostic records Astra's own `Gif::submit` calls from a freshly
constructed BIOS tracer, after boot reset and before execution. It is **not a
PCSX2 GS dump, a hardware timeline, a savestate, or a correctness oracle**.
It separates inspection of the current graphics inputs from expensive EE/IOP
execution. A replay can reproduce an existing bug as faithfully as good output.

## Capture and inspect

Supply your own BIOS. Keep captures in an ignored build directory; firmware and
its derived streams/images must not be committed or redistributed.

```sh
ASTRA_TRACE_SECONDS=600 ASTRA_GIF_TRACE=build-release/cloud.agf \
  build-release/ps2bios_trace /path/to/your-bios.bin \
  0 300000000 1 0 8 0 0 0 - build-release/cloud.ppm

build-release/ps2gif_replay build-release/cloud.agf \
  --framebuffer build-release/cloud-replay.ppm \
  --triangles build-release/cloud-triangles.tsv
```

The BIOS tracer normally returns 1 when it reaches the requested instruction
limit while still running. Inspect its stop/deadline report and trace completion
line; this exit is not a crash. Replay returns 1 if any replayed GIF submission
was rejected, even if the trace container validated successfully.

Compare the tracer and replay's `framebuffer_hash`, draw/submission counts and
`logical_vram_word_hash`. The latter hashes every aligned word of Astra's full
4 MiB logical VRAM, not native GS swizzled memory. Matching previews alone are
not enough to establish matching source textures and off-screen buffers.

`--stop-packet N` replays a submission prefix. Alternatively, use
`--sprite N --sprite-framebuffer FILE --sprite-source FILE` to preserve a
selected supported textured sprite's post-draw preview and source grid. Sprite
sequences start at zero; submission ordinals start at one. Sprite-stop replay
finishes the submission containing that draw, so its final framebuffer can
differ from the frozen selected-sprite framebuffer. The entire container and
footer are validated even when replaying only a prefix. A stop beyond the trace
or an uncaptured/unsupported selected source fails explicitly.

Triangle TSV contains the actual emitted draw inputs, including raw guest XYZ,
ST/Q, RGBA, UV, selected GS context registers, submission ordinal and EE endpoint.
Inputs are observed before flat-shading replacement/winding changes. It includes
quarter-grid-degenerate triangles too; this is not a list of visible pixels.
It does not capture every GS register. A completion footer distinguishes a
successful export from a partial file.

## Explicit linear surfaces and full-pixel replay

The active drawing FRAME and the enabled display framebuffer can differ.
Select a logical surface explicitly, using the raw DISPFB and native region
dimensions obtained separately from your tracer. Privileged display writes are
not in this GIF trace; replay does not infer the current circuit or scanout.

```sh
# Example selection for the locally recorded 800M checkpoint, not a universal
# BIOS/display setting. Normal replay samples a 160x64 grid from this region.
build-release/ps2gif_replay build-release/cloud.agf \
  --linear-framebuffer build-release/region-quarter.ppm \
  --dispfb 1400 --linear-size 640x256

# Separate host-only graphics library: render one sample per native pixel.
build-release/ps2gif_replay_native build-release/cloud.agf \
  --linear-framebuffer build-release/region-native.ppm \
  --dispfb 1400 --linear-size 640x256
```

All three linear-selection flags are required together. DISPFB is strict unsigned
hex (optional `0x` prefix); dimensions are positive decimal `WIDTHxHEIGHT` and
must be divisible by the build's sample scale. Only PSMCT32/24, a nonzero stride,
an in-row origin/width and a complete unwrapped region within 4 MiB are accepted.
Output alias checks include this path. Only read-only logical-VRAM access is
used, after the entire trace/footer validates; no Gs target is rebound. The log
states raw selection, origin, stride, native/output dimensions, scale, sampled
raw-word hash and RGB count. Alpha participates in hashing, not the PPM.

The default/Vita grid remains 160x112, with a 4x4 native tile per sample. The
separate `ps2gif_replay_native` executable links only the scale-1 Gif/Gs library
and has a 640x448 viewport. Its selected linear 640x256 output excludes the
known adjacent-buffer over-read at this checkpoint. It re-rasterizes guest
geometry and feedback at full pixel density; it is **not an upscale, sharpening
filter, native GS swizzled-memory implementation or reconstructed scanout**.
Integer-vertex truncation, filtering and other existing GS approximations remain.
Different raster grids intentionally have different output/VRAM hashes; compare
matching versions/grids when checking exact replay reproducibility.

Native triangle TSV X/Y use that build's native pixel grid, while raw XYZ/ST/Q
remain unchanged. The current cloud comparator validates quarter-grid X/Y and
will reject native-grid TSV; use the default executable for that comparison.
Selected-sprite source probes deliberately remain the documented **160x64 grid
at native 4-pixel spacing**, in both builds. Only the selected-sprite framebuffer
changes to the native viewport dimensions. These distinctions prevent a grid
change from masquerading as an architectural or reference-input difference.

## Compare a reference draw

Given locally obtained PCSX2 software-renderer context and expanded vertex dumps:

```sh
python3 tools/compare_cloud_vertices.py \
  --context /path/to/00081_context.txt \
  --vertices /path/to/00081_vertex.txt \
  --triangles build-release/cloud-triangles.tsv --pcsx2-gs-precision
```

The default expected reference count is 244 triangles; use
`--expected-triangles N` for another draw. Native decimal X/Y plus XYOFFSET
reconstruct raw 12.4 XYZ; printed `Si/Ti/Qi` provide authoritative **saved
post-flush GSVertex** float bits, not original incoming GIF words. For textured
STQ triangles with equal-Z tracing, the pinned PCSX2 implementation masks
mantissa bits before printing. `--pcsx2-gs-precision` adds a clearly derived
precision-adjusted view, with source/gates, exceptional-input skips and separate
raw/adjusted counters. It never rewrites captured data, normalizes RGB, or changes
`--require-equal`. Agreement there means compatibility with saved GS precision,
not proof of identical original floats. See the
[primary precision and dump order](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/GSState.cpp#L2333-L2467).
The comparator first searches for an exact raw-XYZ triangle anchor. Ambiguous
anchors require `--candidate-index N` rather than silently picking a frame.
It then aligns ordered XYZ triples monotonically, comparing attributes only for
pairs present in every maximum exact alignment. Extra candidate triangles and
unmatched/ambiguous reference triangles are reported, not assigned a cause or
paired by array index. A bounded sparse alignment prevents duplicate geometry
from creating an unbounded match graph; near coordinates are not exact matches.
An opt-in `--allow-z-fallback` is explicitly approximate and never establishes
equality. Read the reported geometry/color/state differences and count basis:
a match in one field is not an entire-frame or timeline match. Exit 0 normally
means a comparison was produced, not that inputs agree; `--require-equal` makes
observed mismatches fail.

## Bounds and scope

The versioned little-endian `ASTRAGF1` container preserves submission boundaries,
including empty/fragmented submissions. Records carry an ordinal, EE cycle,
next EE PC and raw payload; these are host functional-service observations,
not GIF bus-cycle/arbitration events. A mandatory terminal record/footer checks
counts and a rolling corruption checksum. It is not cryptographic authentication.
Malformed, truncated, nonmonotonic, oversized and trailing-data containers fail.

Limits are 16 MiB per submission, 64 MiB total payload, 100,000 submissions and
100,000 exported triangles. These bound the container, not all existing GIF
diagnostic retention or parsing work: many small IMAGE tags can amplify retained
history. This is a local research tool, not an arbitrary-untrusted-input sandbox.
Failed capture I/O/capacity latches without changing
an in-flight guest submission; the tracer reports failure at its safe endpoint.
Failed exports may leave partial diagnostics on disk; only successful container
validation and TSV completion establish a completed capture/export.

Replay relies on a **fresh `Gs`/`Gif` instance** and on every later graphics
mutation arriving through the recorded submissions. Reusing `Gif::reset` alone
is insufficient because host raster/depth state is not serialized. Arbitrary
frontend GS clears, direct state edits, and mutable preview-cache reads are
outside the format's fidelity contract. The current BIOS tracer avoids them.
Path-alias controls are regression-tested on Linux/WSL. Native Windows
case-only aliases between nonexistent outputs remain an unverified limitation;
use genuinely distinct paths, not spellings differing only by letter case.
The owned synthetic tests require neither Sony firmware nor reference assets.
