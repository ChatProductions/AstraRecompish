# Session 2026-10-10: exact contiguous monitor scaling and progress recording

The selected display-buffer diagnostic is saved on `main` as `cbdd527`. This
follow-up reduces work in the Vita monitor's software preview scaling. Audio
remains deferred. No new EE execution, VU endpoint, completed intro or verified
hardware FPS is established here.

## Retained path and rejected candidate

The old compiled Vita `Screen::blit_160x112` calls `Screen::rect` once for every
source pixel: 17,920 calls for one 160x112 preview. Each expands a source word
to the same six-by-four output rectangle.

An initial scratch-row-plus-`memcpy` candidate preserved every pixel, but its
five-trial PC median was 32.7865 ms per 256 copies versus 21.0938 ms for the
rectangle route: about 55% slower. That routing is **not retained**. Using a
framebuffer row as the copy source was also avoided because it would add reads
from the Vita graphics-memory allocation.

The retained portable helper writes visible rows contiguously, with the same
six copies of each source word. It has no scratch array, allocation, decoded
image cache, color/alpha conversion or framebuffer readback. It replaces only
monitor copying, not guest rasterization, scheduling or graphics-memory state.

Vertical clipping is widened to int64 before adding row offsets. Surface pitch
and capacity are checked before pointer arithmetic; zero/narrow pitch, short
capacity and overflow-sized products fail without writes. Source is required
to contain 160x112 readable words and not overlap the destination, as with the
actual separate GS preview and Vita screen allocation.

The final Vita ARM output contains no rectangle or copy-library call in the
blitter. At `0x416` / `0x41A` it duplicates the color into NEON registers, then
stores four and two words with `vst1.32` at `0x41E` / `0x422`. The function uses
a 32-byte register-save stack frame, not the rejected scratch row. This is
generated-code evidence, not a device throughput measurement.

## Owned regression and host timing

The 115-check pixel fixture passes in Release and UBSan, and separately under
AddressSanitizer plus UBSan. It compares every surface word across 17 vertical
offsets and three pitches, including INT_MIN/MAX, partially visible tiles,
padding and prefix/suffix canaries. Invalid/null/overflow-sized surfaces do not
write. Reused-surface tests check live source changes, retained outside rows
and a fully offscreen no-write success.

`monitor_blit_benchmark [FRAMES_PER_TRIAL]` accepts strictly decimal 1..5000
(default 256), performs one warmup per route, and alternates route order across
five trials. Each completed destination is independently checked in full;
source changes and an observable result prevent an unchanged final-frame-only
benchmark. Setup, allocation and the full-image oracle are outside timing.
Both routes use noinline wrappers; the old rectangle call is deliberately
preserved to measure the routing observed in the old ARM binary, not to predict
host/compiler heuristics. The host probe uses `-O2`, like the Vita frontend.

Final 1,024-copy pair, with no builds/tests/video generation alongside timing:

| Trial | Rectangle route (ns) | Contiguous row route (ns) |
| --- | ---: | ---: |
| 1 | 81,457,100 | 42,832,900 |
| 2 | 76,936,700 | 41,658,000 |
| 3 | 76,878,600 | 42,988,000 |
| 4 | 77,140,200 | 43,105,900 |
| 5 | 76,278,700 | 42,518,900 |

Median falls from 76.9367 ms to 42.8329 ms: **44.33% less host copy time** in
this owned microbenchmark. Host frequency/cache/load are uncontrolled. The
probe excludes screen clearing, CDRAM behavior, emulation, guest rendering,
VBlank, frame completion and presentation. Do not translate this result into
Vita FPS or combine it with the separately scoped graphics-submit percentages.

Malformed counts `0`, `5001`, `-1` and `1x` return exit 2. A one-copy smoke
benchmark and the dedicated pixel fixture are registered with CTest. Release
and UBSan each pass **47/47 targets**, and the Vita SDK produces the VPK.

## New local progress recording

The user's recording request is completed at:

`build-release/video-display-progress-2026-10-10/AstraRecomp-PS2-display-routing-progress.mp4`

The clip compares the old draw-target view with the new selected-buffer
diagnostic at ten actual 350M-800M checkpoints. It uses the production shared
display sampler, not an invented image. Every GIF prefix is followed by
validation of the entire container/footer; selected PMODE/DISPFB values are
checked against the matched external BIOS checkpoint log. Display state is
**not contained in the GIF stream**. All ten prefixes have zero rejected tags
and pending bytes.

The raw samples are 160x112 quarter-grid pixels. Panels use explicitly labelled
fourfold nearest-neighbor replication to show fixed 640x448 regions, which can
include adjacent buffer storage. Each checkpoint is held for 1.5 seconds, with
its actual instruction checkpoint in the label. There is no interpolated
motion, audio, complete animation claim or real-time/hardware FPS claim.

Verified MP4: H.264/yuv420p, 1280x544, 30 encoded frames/s, 450 frames, 15.000 s,
186,629 bytes, one video stream and no audio. Full ffmpeg decoding succeeds;
the 350M and 800M labelled images were visually inspected. SHA-256:

`6fd0c7bba438556578f5e4aedc665ae520b9fb1930a55e7389cc2091866fd789`

All ten labelled frames have distinct hashes. At 800M, both raw recording
panels match the independent `ps2gif_replay` exporter byte-for-byte. Draw-target
PPM SHA-256 is `77f5efaace353f75a1e896d89a8b5ef6fb63b20c3087c8b53def5a085db56c51`;
selected-buffer PPM SHA-256 is
`5e20358ce87204b9c4089b5bb15442375ba67f828a64cf8055a14b375624140e`.
The independent replay also preserves quarter preview / full logical-VRAM
word hashes `FB810EF9FC0E36C9` / `9ABC363A5342F462` and all saved draw counts.

The sharper, separately rendered native-grid checkpoint video remains at
`build-release/video-checkpoint-2026-10-10/AstraRecomp-PS2-startup-checkpoint-preview.mp4`.
It is also held-checkpoint playback, not real-time Vita. Firmware-derived
frames/videos and the local recording harness stay ignored; only code, owned
tests, methodology and session notes are published.

## Resume point

Continue the main startup goal from the saved 800M graphics evidence. The
selected-buffer view now distinguishes display-buffer content from the latest
draw target, but real scanout/fields/swizzling and native pixel coverage remain
open. VU1's normal 998-pair endpoint is unchanged. Keep measuring generated
work and actual runtime gates; do not add timing hacks or claim retail playback.
