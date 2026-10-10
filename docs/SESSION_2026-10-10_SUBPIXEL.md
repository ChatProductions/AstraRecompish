# Session 2026-10-10: fractional triangle geometry

The actual saved guest stream remains recognizable, with readable Sony Computer
Entertainment lettering. This milestone reduces a measured cloud source-texture
error by preserving fractional triangle positions; it does not establish a
completed animation, correct scanout, retail-game playability or Vita speed.
Audio remains deferred.

## Cause and bounded implementation

Every one of the 34,810 exported 300M triangles has a fractional post-XYOFFSET
coordinate. Raw-area degenerates number 3,486, versus 3,571 on the old quarter
vertex grid: 85 genuinely nondegenerate triangles collapse after projection.
Native integer snapping adds no degenerate triangles in this capture, but flips
50 winding signs; the quarter projection flips 130. Counts describe inputs,
not newly visible fragments. First-cloud candidates are fractional without
collapses, so interpolation can improve even when no primitive disappears.

`GsVertex` now appends optional `fixed_xy`, `fixed_x` and `fixed_y`. Gif latches
signed raw XYZ minus XYOFFSET at each kick. Its legacy x/y fields remain in the
existing integer trace/point/line grid. Triangle geometry uses the checked fixed
positions, preserving all 12.4 fractions for edge coverage, winding, barycentric
color/depth and FST/STQ interpolation. Standalone triangles without the flag
retain their original host-grid units; mixed fixed/legacy inputs promote before
range checking. The new fixed route rejects positions outside the guest
subtraction range [-65535,65535], avoiding overflow from arbitrary public ints.
It does not harden all pre-existing unflagged extreme-int geometry cases.

Non-AA samples are integer native anchors, not half-pixel centers. Fractional
bounding limits use signed ceil/floor, with the existing top-left ownership
rule. Viewport/SCISSOR clipping bounds loops before sampling. SCISSOR minima
now round up to the first available anchor; maxima stay inclusive. That shared
state correction also affects point/line clipping for unaligned minima, though
their geometry remains integer. Quarter tiles still replicate accepted samples;
this is not a native per-pixel scissor mask for partial tiles.

The pinned reference preserves fractional positions and uses integer scanline
anchors with fractional prestep. See [vertex conversion](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/Renderers/SW/GSRendererSW.cpp#L202-L257),
[scanline setup](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/Renderers/SW/GSRasterizer.cpp#L974-L1018)
and [inclusive scissor](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/GSDrawingContext.cpp#L80-L91).
Our independent integer-edge/rational implementation is not a reproduction of
all reference floating stepping/attribute precision, AA or hardware behavior.
Existing offset-latching semantics remain; changing offsets in a software test
is not a new physical-hardware oracle.

Changing latched offsets can produce area 17,179,344,900: multiplying full 32-bit
Z by such weights can overflow uint64. A portable exact decomposition splits
each Z into high/low 16 bits before weighted accumulation. The residual is recombined
with addition, preserving carries; common areas at most UINT_MAX retain the
simple fast path. No host-only __int128 or floating approximation is needed on
Vita. Tests use an independent minimum-Z affine identity, not the same formula.

Replay TSV and BIOS triangle logs now export each latched fixed position in
addition to unchanged legacy coordinates/raw attributes. A single draw-time
XYOFFSET cannot otherwise reconstruct earlier vertices if the offset changed.
The nondegenerate diagnostic selection now uses raw fixed area; it is still not
a visibility test. Matching pre-change headers/libraries/executables were saved
under ignored `baseline-subpixel/include` / `*-before-subpixel` paths.

## Same-input reference and replay checks

The sprite-50 frozen source still samples 10,240 anchors `(4*x,4*y)` against the
same local draw-99 input texture. Reference nonblack count is 4,031.

| Source probe | Quarter filtered/snapped | Quarter fractional | Native filtered/snapped | Native fractional |
| --- | ---: | ---: | ---: | ---: |
| Nonblack RGB samples | 3,944 | 3,943 | 3,952 | 3,943 |
| Exact RGB samples | 6,730 | 7,730 | 7,322 | 7,786 |
| Mean RGB channel error | 0.5982096354 | 0.1506184896 | 0.2513997396 | 0.1417317708 |
| Black-mask disagreements | 205 | 88 | 103 | 88 |

This is a local source-surface comparison, not full-draw or full-frame equality.
Native source PPM SHA256 is
`e668193a644e386c65ab9d941321ff1f1640a6a16299dc7bbe2087722423b40c`.
Native 800M selected 640x256 region has 70,188 nonblack RGB pixels, word hash
`A00871954FFAFFBF`, PPM SHA256
`1e1f2a1144a04c1a53a9e57b2b9e8a648d3fa55e71d5c8692eef334ab7139257`.
Its active preview hash is `EB315B0A3A414B7C` (74,668/286,720 nonblack), full
logical-VRAM hash `C0F2910304BAA56B`.

Quarter 300M preview/VRAM hashes are `E8DA2455A6BF2B3B` / `4FB5DD667DA8D422`;
quarter 800M `FB810EF9FC0E36C9` / `9ABC363A5342F462`. The latter's selected 1400
640x256 grid hash is `2894897C1D73BFEC`, with 4,383/10,240 nonblack samples.
All 800M inputs/counts remain unchanged: 59,964 submissions/42,686,400 payload
bytes, 2,157 sprites/384,780 triangles/2,808 lines, no rejected/pending GIF data.
Last/terminal EE endpoints remain 799999901/00272B00 and 800000000/002728B0.
This is re-rasterization, not further execution or a fix to VU1's known endpoint.
Artifacts `cloud-subpixel-*.ppm/.png/.txt` remain local and ignored.

## Regression gate

Release and UBSan pass **41/41 CTest targets**, and the unchanged-scale Vita VPK
cross-build passes. The new owned subpixel suite has **203 checks** at each
scale, all passing. Matched pre-fix libraries fail 43 checks at scale 4 and 36 at
scale 1. Coverage includes fractional/negative geometry, winding/shared edges,
tiny native-only and quarter-anchor triangles, signed clipping, fractional
Gouraud/depth/FST/nonconstant-Q STQ, trace compatibility, observer neutrality,
legacy API controls and fixed/mixed coordinate guards. Changing-offset large
area cases check constant UINT_MAX depth and the independent varying-Z hand
value 4294934495 at native (4,4).

Two new owned serialized-replay methods bring that process suite to **24/24**.
They verify exact PPM/RGBA hashes and latched fixed TSV fields, including offset
changes between kicks; both fail the preserved pre-fix executable. Other existing
signed sampler/filter/sprite/feedback tests remain green.

Some WSL build invocations reported subsecond dependency clock-skew warnings;
all three build gates completed successfully. Those warnings are not a new
rendering or speed result.

## Local checkpoint video

`build-release/video-checkpoint-2026-10-10/AstraRecomp-PS2-startup-checkpoint-preview.mp4`
is a 15-second, silent diagnostic video of ten distinct native replay snapshots
at 350M, 400M, ..., 800M EE checkpoints. Each snapshot is held for 1.5 seconds;
30-fps encoding is playback rate, not emulator FPS. The 640x256 guest pixels
are unscaled, with a 64-pixel label band added above them. No generated motion,
frame interpolation, sharpening or replacement graphics were added.

Each prefix stops at the last complete captured GIF submission at or before its
checkpoint. The selected logical DISPFB region is 1450 through 550M and 1400
from 600M onward, using the separately recorded privileged-register snapshots.
This is not continuous frame capture, timing-accurate GS scanout, a complete
boot animation or real-time Vita output.

All ten PPMs have distinct hashes; the final one matches the 800M native region
above. The MP4 is 137,703 bytes, H.264/yuv420p, 640x320, 450 frames, and passed a
full decode check. SHA256:
`105591c04a7c3b0149a9567585077c2895f001f1f9dfdb293e38b3548940a9e1`.
The ignored `.tools/record_checkpoint_preview.py` consumes the ignored
`frame-plan.json` beside the video, validates prefix counts/cycles, renders from
the original capture and encodes those snapshots. All BIOS-derived video,
capture and image assets remain local and are not committed to GitHub.

Next: isolate the remaining shared dark-pixel/color differences from texture
function/Gouraud precision and compare later individual draw surfaces. Keep
scanout/aspect/interlace and real hardware performance separate from raster
correctness. Do not infer a project completion percentage from these tests.
