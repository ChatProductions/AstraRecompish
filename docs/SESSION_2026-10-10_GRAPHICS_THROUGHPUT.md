# Session 2026-10-10: graphics throughput, without changing the image

The preceding fractional-geometry milestone is on `main` as `debf278`.
The local 15-second checkpoint video is documented in the
[subpixel session](SESSION_2026-10-10_SUBPIXEL.md#local-checkpoint-video).
Its 30-fps encoding holds ten captured states; it is not a measured guest FPS.
Audio remains deferred and physical Vita performance remains unmeasured.

## Profile and bounded changes

An indicative `-O3 -pg` native-grid replay profile of the saved 800M stream
attributes about 53% of sampled self time to texture sampling, 15% to triangle
work and 14% to fragment writes. Instrumentation affects cost: those percentages
identify candidates, not an uninstrumented speedup or device-performance result.
The profile counted 244,758,398 texture samples and 384,780 triangle calls.

Four changes reduce repeated work, without reducing resolution,
filter taps, draw count or guest timing:

- Each triangle guards Z and RGBA independently. If all three corner values
  match, their exact barycentric result is that value because the integer
  weights sum to the area. Varying attributes retain the previous rational
  interpolation, including overflow-safe wide-area depth. Texture sampling,
  alpha/depth tests, blending and accepted-write diagnostics still run normally.
- Each synchronous textured draw decodes TEX0/CLAMP/TEXA/TEX1 once into a
  stack-owned setup. Triangle callbacks borrow it synchronously with a small
  capture; sprites reuse it for their loops. No persistent cache fields or Gif
  object-layout change were introduced. Every tap still reads live logical
  VRAM before the current fragment writes, preserving aliased feedback.
- Bounded byte-channel filtering/blending uses an exact divisor-aligned positive
  bias before unsigned power-of-two division. Filter products are in
  [-3825,3825], so adding 4096 then dividing by 16 and subtracting 256 reproduces
  signed floor. Blend products are in [-65025,65025]; bias 65536, divisor 128
  and signed subtraction of 512 do the same. The quotient is cast to signed
  before subtracting. No implementation-defined negative shift, approximate
  rounding or floating-point reassociation is used. Horizontal-then-vertical
  filter rounding remains separate; this helper is not arbitrary-coordinate
  division and needs a new bound proof if channel/fraction precision changes.
- On the quarter grid only, an effective zero FRAME mask selects a direct overwrite route, avoiding
  destination reads for every native pixel in a tile. Nonzero FBMSK,
  PSMCT24 alpha preservation and RGB_ONLY still use the original read/modify/
  write path. Byte-wise 4 MiB wrapping, blending reads and watched before/after
  values are unchanged. `clear()` already explicitly used zero masks; this
  does not introduce new PSMCT24-clear alpha semantics.

In the exported 300M inputs, 32,062/34,810 triangles have constant Z and
30,113/34,810 constant RGBA after the existing flat-shading replacement.
These are eligible draw counts, not covered-pixel savings or an FPS prediction.
Incremental floating-point/STQ stepping, approximate reciprocal division,
skipped draws and frozen source textures were deliberately not introduced.

## Reproducible measurement

The new host-only `ps2gif_benchmark` and `ps2gif_benchmark_native` validate and
preload the complete bounded trace and footer before any timing. One warmup and
each requested sample use fresh Gs/Gif instances, reset outside the interval.
Only the submit loop is timed; parsing/rasterization/core allocations remain
inside it. File I/O, container validation, setup, diagnostics/output and final
hashing remain outside. Observers and captures are disabled.

Every trial must reproduce final FRAME, the entire active-preview and 4 MiB
logical-VRAM word hashes, and primitive/transfer counts. Pending or rejected
GIF data fails. Owned process fixtures test CLI bounds, bad containers, split
submissions, empty controls and fresh-depth isolation without timing thresholds.
See [benchmark usage and scope](GIF_TRACE.md#deterministic-host-graphics-throughput).

Matching pre-change libraries and replay/benchmark executables are preserved
under ignored `build-release/*-before-constant-attributes*` paths. The separate
constant-only intermediate is saved under `*-constant-attributes-only*`.
The constant-plus-texture-setup intermediate is under `*-draw-setup-only*`.
The byte-math intermediate is under `*-byte-math-only*`.
All runs use the same local `cloud-sqrt-ft-800m.agf`, optimized Release flags
and raster scale; no firmware-derived benchmark input is committed.

### Final adjacent paired comparison

Both executables submitted the same 800M capture on the PC, with one warmup
and five fresh-instance timed trials apiece. No builds or other project
benchmarks ran alongside this final pair. Times cover graphics submission,
not the preceding 800 million EE instructions.

| Quarter-grid version | Median submit time | Minimum | Maximum |
| --- | --- | --- | --- |
| Frozen pre-optimization | 1.541900142 s | 1.540653118 s | 1.559430059 s |
| Final implementation | 1.217550128 s | 1.210592246 s | 1.243246340 s |

This pair gives **26.64% higher host graphics-submit throughput** and
**21.04% less submit time**. Earlier batches varied materially with host
conditions; their larger gains are not the headline result. CPU frequency
and system load were not controlled, and this is not a physical Vita FPS test.

Every trial preserves FRAME `00000000000A00F0`, preview word hash
`FB810EF9FC0E36C9` and full logical-VRAM word hash `9ABC363A5342F462`.
The input is still 59,964 submissions / 42,686,400 payload bytes, emitting
2,157 sprites, 384,780 triangles and 2,808 lines, with zero rejection and
zero pending bytes. The unchanged native-grid hashes are preview
`EB315B0A3A414B7C` and logical VRAM `C0F2910304BAA56B`.

The native-grid exploratory median fell from 16.456 s (six baseline trials)
to 15.141 s (three byte-math trials), but those batches are not a controlled
paired device-performance measurement. Applying the overwrite shortcut to
the native grid then regressed its median to 17.096 s. That candidate was
rejected: the final shortcut is compile-time restricted to the quarter grid.
The final native GS object is byte-identical to the byte-math-only version
(SHA-256 `526dbbe60015ca82f6c224caff699c0caa1b688876c9b639e8cac44267877983`),
as is its benchmark executable
(`ca6d2d243589758e83a5aae0a9c85aad691b12d76e659b2e017a6809847470a0`).

### Regression gates

Release and UBSan each pass **43/43 CTest targets**. The VitaSDK cross-build
produces `ps2vita.vpk`. These are host/build gates, not hardware validation.

The triangle suite passes 810 checks on each raster grid and also passes when
linked against the frozen pre-optimization libraries. It covers independent
constant/varying attributes, extreme depth, winding, draw-to-draw transitions,
callback inputs, accepted-write diagnostics and watch-on/off VRAM identity.
The bilinear suites pass 611 quarter-grid / 551 native-grid checks, including
all 1,048,576 byte-endpoint/fraction combinations and all 130,051 blend-product
values against independent integer oracles. Wrapped target writes exercise
partial, zero and full masks, PSMCT24 alpha preservation, untouched VRAM,
preview refresh and watched before/after values.

The two benchmark process targets each run eight owned-fixture test methods.
They additionally cover twelve sequential texture-state transitions across
contexts, formats, base addresses, filter/address modes and TEXA, checking
independent colors, full uploaded-source-inclusive VRAM hashes and replay
agreement. Repeated trial equality proves repeatability, not hardware accuracy.

## Resume point

Keep graphics-submit throughput distinct from complete EE/IOP/VU execution,
guest frame completion and hardware FPS. The capture has no authoritative
frame-completion markers. The Vita monitor also currently couples one 25,000
instruction slice to each VBlank-paced UI loop; removing that frontend ceiling
is a separate scheduling change, not a renderer FPS claim. Preserve responsive
controls and deterministic guest event ordering when addressing it.

Next renderer candidates are exact integer edge recurrences and hoisting the
sprite V rational quotient per row. Benchmark each independently with the same hashes and retain the
accurate live-feedback path. The startup image and known VU1 endpoint have not
been promoted into a complete-boot or game-playability claim.
