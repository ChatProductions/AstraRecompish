# Session 2026-10-10: isolated half-grid rendering experiment

The source-bounded preview removes unrelated framebuffer storage but cannot
restore detail lost earlier in rendering. The default/Vita renderer evaluates
integer native anchors every four pixels and expands each accepted sample to
a 4x4 native tile. Raw triangle coordinates already retain their signed 12.4
precision; quarter-grid lettering loss is not the old vertex-snapping bug.
Thin lines also still use the legacy coarse-grid projection.

## Narrow experiment, unchanged Vita default

`ps2graphics_half` is a separate host graphics library compiled with the public
`PS2VITA_GS_RASTER_SCALE=2` definition. Its consumers are
`ps2gif_replay_half`, `ps2gif_benchmark_half`, and the owned half-grid fixture.
They do not link the differently sized `ps2core` or native graphics library.
The default core/Vita remains scale4; the separate native library remains
scale1. No game-specific text detection, selective glyph patch or image
sharpening is added.

This path evaluates 320x224 genuine raster samples and writes approximate 2x2
native tiles. It preserves more thin detail but is not hardware GS coverage,
swizzling or scanout. The existing Vita display preview and monitor blitter
remain 160x112/step4, so simply recompiling the Vita core at scale2 would not
correctly expose the extra samples. That integration is intentionally not done.

## Owned gates and deterministic 800M replay

The independent half-grid fixture passes **43 checks** in Release and UBSan,
plus isolated ASan+UBSan with strict warnings. It compares full owned targets
for projected points/lines, physical watch footprints, fractional/tiny
triangles with both windings and shared edges, fractional sprites, sixteen
distinct texture samples, preserved source texels, and 4 MiB wrapped masked
writes including PSMCT24 alpha preservation. Its PPM header/payload must be
320x224. These are logical diagnostic oracles, not golden hardware images.

The existing benchmark subprocess fixture is parameterized by scale. It now
also runs at scale2, with independently calculated full preview/VRAM hashes,
fragmented submissions, fresh-depth and live-state checks, strict malformed
containers, and explicit raster-grid scope labels. Different grids must be
deterministic within their own configuration; their VRAM hashes should not
be expected to agree with one another.

Release and UBSan each pass **50/50 CTest targets** including the source-extent
follow-up. The Vita SDK builds the unchanged-grid VPK. Some WSL dependency
checks show subsecond clock-skew warnings; all builds finish and test binaries
pass. This is not physical-Vita or Vita3K rendering/performance validation.

Replaying the saved complete 800M GIF stream with the half grid gives:

- 59,964 submissions / 42,686,400 payload bytes, zero rejected/pending;
- 2,157 sprites, 384,780 triangles, 2,808 lines, zero points;
- active-preview word hash `A7844706F03EBBD5`;
- full 4 MiB logical-VRAM word hash `4E2F3D91A7198922`;
- selected DISPFB2 `1400`, 640x256 native region exported as **320x128**;
- selected-region word hash `C928064A6EF82C50`, 17,602 nonblack RGB samples;
- PPM SHA-256 `1205f07be3d4b24b1363ec80203e0a15e3d935c47c91c1d84d830006b51f1cc3`.

The local nearest-displayed 800M image was visually inspected: “Sony Computer
Entertainment” is readable, unlike the corresponding quarter-grid streak.
The clearer native-grid view remains a separate diagnostic. This is a density
tradeoff demonstrated on one startup workload, not complete animation or
retail-game compatibility.

## Cost and progress recording

The fresh 800M metadata run completes with a byte-identical GIF stream and
matched per-checkpoint DISPLAY/SMODE source geometry. After the run, builds,
tests and sanitizers finish, the cost probe rotates order across three rounds:
quarter/half/native, half/native/quarter, native/quarter/half. Each invocation
uses a fresh validated/preloaded stream, one warmup and one timed submit pass;
full preview/VRAM/draw-state signatures must match that grid's known values.
No builds, tests, BIOS capture, video generation or other timings run alongside.

| Grid | Three submit samples (ns) | Median |
| --- | --- | ---: |
| Quarter, 160x112 | 1,247,064,684 / 1,247,284,350 / 1,229,901,951 | 1.2471 s |
| Half, 320x224 | 3,498,987,738 / 3,534,326,019 / 3,479,909,272 | 3.4990 s |
| Native, 640x448 | 11,903,717,595 / 11,937,323,510 / 11,943,222,471 | 11.9373 s |

For this PC/workload pair, half-grid submit time is **2.81x quarter** and
**29.31% of native**. The lettering improvement costs more than the current
grid, not less. That is why the experiment is retained as a host diagnostic
and is **not enabled on Vita**. Host frequency/cache/load remain uncontrolled;
the probe excludes EE/IOP/VU execution, presentation and frame completion. This
is not a new runtime FPS, real-time boot or device throughput result.

The raw local timing logs and summary are in
`build-release/grid-cost-2026-10-10`; the ignored orchestrator is
`.tools/measure_grid_cost.ps1`.

The new local recording is
`build-release/video-half-grid-2026-10-10/AstraRecomp-PS2-half-grid-progress.mp4`.
It compares quarter and half rendering at ten actual 350M-800M GIF prefixes,
matched to the fresh privileged-display snapshots. The selected source is
640x256 at every checkpoint. Quarter raw 160x64 and half raw 320x128 samples
are replicated 4x and 2x respectively into equal 640x256 panels. No sharpening,
interpolated motion or replacement graphics are used. Each checkpoint is held
1.5 seconds; 30 encoded frames/s is playback rate, not emulator FPS.

Verified clip: H.264/yuv420p, 1280x352, 450 frames, 15.000 seconds, 214,450 bytes,
one video stream and no audio. Full ffmpeg decoding succeeds; 550M/800M labelled
comparisons were visually inspected. All ten raw half PPMs have distinct hashes,
and the 800M PPM matches the standalone half replay SHA-256 above. MP4 SHA-256:
`7ecc5a1ce28baf427b363c4bf0eba968fd4bf1a0dfee9d6aabb016199bbd427b`.
The ignored `.tools/record_half_grid_progress.ps1` verifies complete trace
identity, prefix counters and matched source geometry before recording. This
is PC diagnostic checkpoint playback, not live Vita, measured FPS, accurate
scanout, continuous full boot animation or audio. Firmware-derived frames,
videos and recording helpers remain local.

## Resume point

Use the measured quality/cost tradeoff to select the next accuracy/performance
experiment. Do not enable this density on Vita without a matching preview path
and device-relevant evidence. Keep genuine native coverage, color/texture
precision, fields/scanout and the original VIF/VU timing issues separate.
Audio stays deferred; VU1's normal 998-pair saved endpoint is unchanged.
