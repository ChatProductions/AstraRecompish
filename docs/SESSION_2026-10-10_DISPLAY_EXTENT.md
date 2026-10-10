# Session 2026-10-10: requested display-source bounds

Previous monitor-copy milestone: `483bda6`. This follow-up separates an adjacent
buffer artifact from the remaining quarter-grid rendering loss. Audio remains
deferred. It does not increase guest rendering resolution, establish complete
startup scanout, or measure physical-Vita FPS.

## Source-region contract

The old selected-DISPFB diagnostic reads a fixed 640x448 region. At the saved
800M endpoint, DISPLAY2 is `001FF9FF01848290`: DW=2559, DH=511, MAGH=3, MAGV=0.
The recent-store ring also records SMODE2=3 (INT+FFMD). Half-height storage at
width 640 occupies 256 rows. Reading 448 rows can include the next framebuffer
at byte `A0000`; this explains the extra lower cloud region without claiming
that the complete visible PS2 frame has been reconstructed.

`GsDisplayWindow` decodes all DISPLAY fields. Requested source width is
`(DW+1)/(MAGH+1)` and height is `(DH+1)/(MAGV+1)`, using integer division.
For supported NTSC/PAL interlaced frame mode, height is then rounded upward
to half that result. SMODE1.CMOD=2/3 identifies the supported analogue modes;
raw SMODE2.INT alone is not enough to assume analogue interlacing.
Packing is checked against [ps2dev gsKit](https://github.com/ps2dev/gsKit/blob/master/ee/gs/include/gsInit.h),
and source-rectangle arithmetic against the pinned
[PCSX2 v2.8.2 GSPCRTC SetRects implementation](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/GSState.cpp).

`GsLinearDisplayPreview::update_source` selects the enabled circuit's DISPLAY
and DISPFB together, reads live SMODE1/2, and keeps the same native-four-pixel
anchors and 160x112 output. Anchors outside the requested source extent become
opaque black **without reading VRAM**. Nothing is stretched, sharpened,
interpolated, or synthesized. Larger sources are explicitly labelled cropped
to the fixed 640x448 viewport. DISPFB DBX/DBY and stride remain the memory
origin; DISPLAY DX/DY do not become VRAM offsets.

Progressive requested geometry is supported. INT with CMOD 0/1 is deliberately
unsupported: HDTV/digital mode detection is not guessed. Zero derived width or
height is an empty extent; raw DISPLAY=0 with zero magnification legitimately
derives 1x1 rather than being treated as an unset register. Disabled/dual
circuits, bad FBW/format, unsupported modes and empty extents clear every stale
pixel without reads, with the existing framebuffer-error precedence preserved.

The original `update` API remains a separate fixed-region diagnostic and its
172-check fixture still passes unchanged. The Vita default now uses the
source-bounded API; L still toggles to the old draw-target view. Its reusable
preview is static storage, with compiled size `0x1183C` (71,740 bytes), not a
large per-frame stack object or allocation.

This is still **not GS scanout**. Field parity/selection, weaving/deinterlacing,
video-mode/overscan detection, output placement/timing, DPMS effects, PMODE
mixing and native VRAM swizzling remain outside this helper. It only limits a
logical-linear memory diagnostic to supported requested geometry. The default
renderer still writes one quarter-grid sample across a four-by-four native
tile; thin lettering can disappear before this preview ever reads it.

## Owned tests and builds

The new extent target passes **196,989 checks** in Release, UBSan, and an isolated
ASan+UBSan build with `-Wall -Wextra -Werror`. Oracles independently calculate
field extraction, quotient/odd-height arithmetic and physical addresses:

- All 64 DISPLAY bit positions set and cleared, reserved fields, all 65,536
  DW/MAGH combinations, and all 131,072 DH/MAGV/CMOD/INT/FFMD combinations.
- Every pixel and ordered VRAM read of 80 region/mode cases, including 1x1,
  odd boundaries, 640x256, maximum fields, narrow strides and 4 MiB wrapping.
- Selected/inactive circuit independence, live replacement/shrinking, full
  stale-image clearing, zero off-extent reads, and fixed-region compatibility.

The BIOS trace fixture passes **13 unittest methods**, including full-width
privileged-register writes, both circuits, reserved bits, mode/extent error
precedence and snapshots taken immediately after low/high DISPLAY stores.
The old diagnostic PPM filenames and bytes remain unchanged.

Release and UBSan each pass **50/50 CTest targets**, including the related
[host-only half-grid experiment](SESSION_2026-10-10_HALF_GRID.md). The Vita SDK builds the VPK;
no physical-Vita or Vita3K rendering run is claimed. For a supported 640x256
source, 10,240 anchors read VRAM rather than the old 17,920. This count is not
a timing benchmark and must not be converted into a device FPS improvement.

## Checkpoint evidence and recording

`ps2bios_trace` now records raw SMODE1, SMODE2, DISPLAY1 and DISPLAY2 at each
requested completed instruction checkpoint, plus a separately labelled
requested-source-extent summary. This metadata is observational; no new PPM
output is opened and the GIF container format is unchanged. Display state is
not contained in an AGF submission stream, so missing mode registers must not
be silently inferred from a replay.

A fresh bounded run completes **800,000,000 EE instructions**, next PC
`002728B0`, opcode `8C820000`, reason=running, host_deadline=0. Exit 1 is the
normal requested-limit result. Its ignored artifact prefix is
`build-release/source-extent-check-800m`. This is a repeated validation run, not
additional progress beyond the previous 800M endpoint.

At all ten 350M-800M checkpoints, live SMODE1=`0000000740816504` (CMOD3/PAL),
SMODE2=3 and selected DISPLAY2=`001FF9FF01848290` request a supported 640x256
source. PMODE is `66`; DISPFB2 switches from `1450` to `1400` after 550M, exactly
as in the earlier capture. VU1 again ends normally after 998 pairs at PC `0308`;
the known separate PATH1 rejection remains unresolved.

The entire new AGF file is **byte-identical** to the saved stream:
44,605,312 bytes, SHA-256
`39e00de6b227debe926af59bcfd3fd354cd122c0e313357b3e3ff9dfd5def9ad`.
It validates 59,964 submissions / 42,686,400 payload bytes. The default replay
has zero rejected/pending bytes, unchanged draw counts and preview/full-VRAM
word hashes `FB810EF9FC0E36C9` / `9ABC363A5342F462`. Thus the metadata and frontend
changes do not change the saved guest GIF stream or default raster output.

The 350M live fixed-region export also matches an independently replayed
fixed-region helper PPM byte-for-byte (SHA-256
`82de33e60d91967762b5223bed14cfa5d8a2974b2fdc8e47346adfdf5cf2d288`).
The new local clip is
`build-release/video-source-extent-2026-10-10/AstraRecomp-PS2-source-bounds-progress.mp4`.
It compares the old fixed region with the source-bounded view at ten actual
350M-800M checkpoints. Each is held 1.5 seconds, with identical in-extent
quarter-grid samples; the extra lower region is black, not reconstructed.
The 800M fixed panel matches the fresh BIOS fixed-region export byte-for-byte
(SHA-256 `5e20358ce87204b9c4089b5bb15442375ba67f828a64cf8055a14b375624140e`).
The bounded panel's PPM SHA-256 is
`a9cbd816db805706fd580e552a2d2f99fc2b86a95eec522bf26866acddd14e9e`.

Verified clip: H.264/yuv420p, 1280x544, 450 frames at 30 encoded frames/s,
15.000 seconds, 180,239 bytes, one video stream, no audio. Full ffmpeg decoding
succeeds; the final comparison was visually inspected and all ten raw bounded
PPMs have distinct hashes. MP4 SHA-256:
`1eabb90997c733dfc415c2b900f7dff42b606c0bbe328971bbb1adc9cdcd3c38`.
The matched fresh register snapshots and byte-identical full AGF permit reuse
of the saved submission-boundary plan without guessing display state. The
ignored `.tools/record_display_extent.ps1` verifies these matches before export.
This is held-checkpoint diagnostic playback, not a live Vita recording, timed
scanout, complete animation, audio or runtime FPS measurement. All firmware
images, clips and recording helpers stay local rather than being published.

## Resume point

Keep the source-selection/extent fix separate from native coverage and texture
precision. Continue the startup milestone with owned and reference per-draw
oracles; do not switch the Vita to the expensive native grid without measuring
the cost. VU1's saved 998-pair normal endpoint is not an unsupported-instruction
stall or proof that the full boot animation is complete.
