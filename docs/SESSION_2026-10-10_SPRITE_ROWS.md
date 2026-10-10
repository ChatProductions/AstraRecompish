# Session 2026-10-10: evaluate exact per-row sprite coordinate work

The preceding frontend-pacing milestone is on `main` as `2a5227f`.
Audio remains deferred. This experiment uses the existing saved 800M graphics
stream; it is not new EE execution, a VU fix or a completed startup animation.

## Candidate and correctness bounds

For FST sprites, the affine V numerator depends on the raster row but not X.
The candidate computes that numerator and its exact signed quotient once per
nonempty textured row. It retains the original unclipped origin/span,
reversed geometry, truncating division, nearest nonnegative clamp and linear
16.16 conversion. U math, bilinear tap order, RGBA math and per-fragment live
VRAM reads remain unchanged. No floating-point expression is reassociated.

The row guard excludes untextured and empty-width rows; a nonempty Y range
implies a nonzero raw Y span. Empty sprites still run the capture/trace footer
and increment their ordinal. With common XYOFFSET, raw span is within
[-65535,65535], UV endpoints are in [0,16383], and visible anchors are below
7168 raw units. The numerator is bounded below 2^32 in magnitude and its
linear conversion below 2^44, well within signed 64-bit range.

The expanded sprite suite passes **195 checks on each grid** when linked
against the frozen pre-candidate libraries. Independent color cases include
reversed Y, clipping, high raw UV endpoints, signed raw geometry extremes and
descending-V nearest boundaries. Nearest/linear empty-footprint cases include
zero width, zero height, zero area, invalid scissors and an offscreen X span.
All cases check the capture and trace footer, including the target hash and
ordinal. Nonempty linear color and live-feedback behavior retain their separate
bilinear and framebuffer-feedback regression gates.

## Measurement

Frozen host archives/executables are under ignored
`build-release/*-before-incremental-edges*` paths (saved for both proposed edge
and sprite experiments). The frozen Vita GIF object/archive are under
`build-vita/*-before-sprite-row*`. No firmware-derived input is committed.
The host submit benchmark uses fresh instances, one warmup, then timed trials.
All captured workload hashes and counters must remain unchanged.

| Grid / trials per version | Original median (range) | Candidate median (range) |
| --- | --- | --- |
| Quarter / 5 | 1.345674385 s (1.244251094-1.383204675) | 1.322245951 s (1.314425529-1.361033691) |
| Native / 3 | 13.479621232 s (12.776830519-13.844581161) | 13.173198047 s (13.106909300-13.252413313) |

These are small, overlapping host deltas, with uncontrolled host CPU/load.
They are **inconclusive**, not an additional speedup or faster Vita frame
delivery claim. The earlier separately measured 27% quarter-grid improvement
remains the reported graphics result.

### Discard decision

VitaSDK ARM disassembly settles the key question: the original compiler output
already puts the V `__aeabi_ldivmod` outside the inner pixel loop for both
nearest and linear sampling. In the frozen original object, the linear X-loop
backedge at `0x23C2` jumps to `0x2368`, bypassing the V division at `0x2358`;
the nearest backedge at `0x2502` jumps to `0x249C`, bypassing `0x247C`.
The candidate has the same division-frequency pattern, with different register
allocation/offsets. Counting four static divide call sites without inspecting
loop backedges would incorrectly imply both coordinates are divided per pixel.

The explicit source hoist was therefore **discarded**. It has no demonstrated
additional target benefit and is redundant with the existing compiler result.
Only the stronger owned regression fixtures and this experiment record are
retained. The tested candidate preserved all input counts and the known
quarter/native preview and full logical-VRAM hashes; reverting it does not
roll back the earlier guarded attributes, sampler setup, byte math or tile
overwrite improvements.

Post-revert Release and UBSan each pass **44/44 CTest targets**, and the Vita
VPK builds. The final quarter/native benchmark executables are byte-identical
to the frozen originals (SHA-256 respectively
`B7EC33E66154C107C8289878445DD91C2D3F59072B95C331F431A212CE3826C1`
and `CA6D2D243589758E83A5AAE0A9C85AAD691B12D76E659B2E017A6809847470A0`).
The Vita GIF object also exactly matches its frozen copy
(`4FAD4DF7E5E13BF1B182CCAB3FD8147536D1484D0B0470B5E90DC63337409030`).

## Resume point

Do not combine host graphics throughput, guest frame completion and UI refresh
rate. The checkpoint MP4 is still ten held captured states. The known VU1
endpoint, complete intro, native GS storage and display scanout remain open.
Before implementing the proposed integer edge recurrence, the same `-O3`
Vita ARM output was inspected. Its hot triangle loop already advances the
three signed 64-bit coverage weights with add/carry operations at `0x262A`
and tests their signs at `0x26AE`; the fragment-write path returns to that
increment block. An explicit recurrence was therefore not added on assumption
alone. Inspect remaining texture-read work next, requiring bounds-safe byte
semantics, unchanged live feedback and matching hashes before any speed claim.
