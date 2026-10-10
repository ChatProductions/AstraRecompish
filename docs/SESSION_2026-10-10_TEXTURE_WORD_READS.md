# Session 2026-10-10: bounded live texture word reads

The sprite-row investigation is saved on `main` as `d889e52`. Its redundant
source change was discarded; the stronger regression fixtures were retained.
This follow-up tests work still present in the compiled texture-memory reader.
Audio remains deferred. There is no new EE execution, VU endpoint or verified
complete startup animation in this experiment.

## Exact read contract

The old Vita `read_local32` performs four byte loads, four address masks and
little-endian byte assembly. For a four-byte-aligned address, masking to 4 MiB
can produce at most `0x3FFFFC`; the whole word is contiguous and in bounds.
On compiler-identified little-endian targets, an alias-safe `memcpy` therefore
has the same byte value and can become a native word load. Unaligned words
still use the old individually wrapped byte route, including `0x3FFFFD` through
`0x3FFFFF` and uint32/high-address aliases. Unknown byte-order configurations
retain explicit byte assembly.

The PSMCT32/24 sampler's base is a multiple of 256 and pixel offset a multiple
of four. Those paths call the internal aligned helper directly, rather than
checking alignment at every tap. The public diagnostic reader remains guarded.
The helper has no persistent state: each call still reads live graphics memory
before the current fragment writes. No pointer cast, strict-aliasing violation,
decoded-texel cache, swizzling claim or filter/texture-function change is used.
PSMCT16 sampling remains unchanged.

## Owned byte oracle

Nine legal IMAGE tags fill all 4 MiB with independently generated, nontrivial
bytes. The tests compare every starting byte address, grouped by all four
alignments, against uint64/modulo little-endian assembly. That includes
1,048,576 aligned words and 3,145,728 unaligned words. Additional high-address
aliases cross the uint32 boundary without relying on the core's arithmetic.
A later IMAGE overwrite of the final qword must immediately change aligned
and cross-wrapped reads, and reset must clear them. Existing filtered draw,
PSMCT24 alpha and framebuffer-feedback fixtures continue exercising the sampler.

The expanded bilinear suites pass 620 quarter-grid / 560 native-grid checks in
Release. Their earlier exhaustive filter/blend oracles remain in place. The
560-check native fixture also passes against the frozen pre-change renderer,
against a build with `__BYTE_ORDER__` undefined (forcing this reader's byte
fallback), and under AddressSanitizer plus UBSan. The forced configuration
tests this reader's fallback, not a complete big-endian port of the project.

## Timing and candidate selection

An initial prototype used the guarded public reader for every texture tap.
Its quarter-grid median was 1.321275177 s versus an adjacent 1.216018265 s
baseline, so that routing was not kept. The refined sampler uses its proven
alignment to avoid that per-tap branch.

The refined quarter-grid pair uses one warmup and five fresh-instance trials:
baseline median 1.328992903 s, range 1.277707338-1.367558047 s;
candidate median 1.295611278 s, range 1.240231000-1.330359324 s.
Those overlapping, uncontrolled host ranges do not justify an additional
headline quarter-grid speedup.

The adjacent refined native-grid pair uses one warmup and three fresh-instance
trials per binary:

| Native-grid binary | Submit times (seconds) | Median |
| --- | --- | --- |
| Frozen baseline | 13.579903396, 13.365998795, 13.622317540 | 13.579903396 |
| Refined word reader | 12.900050404, 13.000091833, 12.703029805 | 12.900050404 |

That is 5.27% greater host graphics-submit throughput, or 5.01% less submit
time, in this pair. The ranges do not overlap, but the three-trial host result
has uncontrolled frequency/load and is not a physical-Vita performance claim.
No builds or tests ran alongside the timed loops. The rejected public-guard-only
routing is not the final candidate. These are graphics-submit times only,
not full-core rates, completed guest frames or Vita FPS.

Every warmup and trial preserves FRAME `00000000000A00F0`, 59,964 submissions,
2,157 sprites, 384,780 triangles, 2,808 lines, 368,640 IMAGE/local bytes, and zero
rejected tags or pending bytes. Quarter-grid preview / full logical-VRAM word
hashes remain `FB810EF9FC0E36C9` / `9ABC363A5342F462`; native-grid hashes remain
`EB315B0A3A414B7C` / `C0F2910304BAA56B`. The grids intentionally differ from
each other; each is compared only with its own baseline.

## Final gates and generated instructions

Release and UBSan each pass all 44 CTest targets. The Vita SDK rebuild produces
the VPK. The final ARM aligned helper masks the address with `ubfx` at `0x618`
and reads the word with `ldr r0, [r3, r1]` at `0x620`, with no alignment branch
or library `memcpy` call. The public reader still routes unaligned addresses
through four individually masked byte loads. This generated-code check is
why the bounded, alias-safe live reader is retained; the earlier redundant
sprite-row and edge-loop source rewrites were not.

Frozen pre-candidate host binaries remain at
`build-release/*-before-incremental-edges*`; the original Vita GIF object is
`build-vita/gif-before-sprite-row.obj`. All firmware-derived captures stay local.
The separately reported earlier 27% paired quarter-grid improvement remains
the established host graphics result; none of these measurements establishes
physical-device FPS or guest frame completion.

## Resume point

Keep the accurate path and owned byte/reference regressions as the correctness
gate. Measure actual generated instructions and whole-runtime performance
instead of assuming an attractive source rewrite pays off. Native GS storage,
display scanout, complete startup animation and VU timing/arithmetic remain open.
