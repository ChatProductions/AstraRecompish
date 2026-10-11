# Whole-system dispatch and PAL intro timing — October 11, 2026

User focus: improve FPS and advance the real PS2 startup intro. Audio remains
deferred. This checkpoint changes production execution, not just replay speed.
No physical-Vita performance result or complete intro is claimed.

## Dispatch work removed, with an architectural oracle

`Cpu::run` formerly warmed a private `EeBlockCache` even though execution never
used it. BIOS scheduling calls this method at no more than eight modeled EE
cycles per IOP boundary. Each miss decoded up to 32 words, and repeated ordinary
instructions also fetched all seven zero-fill pattern words unnecessarily.

Remove that lookup, member and reset clear. Keep the standalone cache and its
tests for future actual consumers. Reject a non-SQ/nonzero-offset first word
before reading the remaining six pattern words. The new bounded Memory span
classifier resolves TLB backing without reading guest bytes; only proven RAM,
loaded ROM or directly addressed scratch storage may be speculatively inspected.
Spans are at most 4 KiB, check overflow, backing limits and page continuity.

This is also a correctness fix, not a requirement to preserve dead metadata's
side effects. An owned RDRAM-response instruction-fetch experiment reproduces:

| Version | `run(8)` | `step()` | Run fault word / next RDRAM response |
| --- | --- | --- | --- |
| Frozen old core | StepLimit, PC B000F460 | InvalidInstruction, PC B000F440 | 00000000 / 00000000 |
| New core | InvalidInstruction, PC B000F440 | InvalidInstruction, PC B000F440 | 0000001F / 0000001F |

The old prefetch consumed the responses before the architectural fetch. The new
run path agrees with `step`, including zero budgets. Native zero-fill now also
checks a requested halt before storing; previously it could clear RAM after halt.
`cpu_dispatch_tests` passes **803 checks**, comparing complete public CPU
architectural state and affected RAM across multiple budgets, zero-fill page
crossings, delay slots, halt, TLB aliases/boundaries and MMIO fetches.
The same 803-check fixture also passes a standalone strict ASan+UBSan build
of Cpu, Memory and the SPU2 dependencies, without sanitizer diagnostics.

Host `sizeof(Cpu)` falls from 104,064 to 1,664 bytes: exactly **102,400 bytes
(100 KiB)** of unused decoded slots removed. The ARM `Cpu::run` symbol is 0x228
bytes versus 0x236 in the saved pre-change VELF, and no longer calls block lookup.
These are generated-code/storage observations, not a throughput measurement.

## Actual whole-system comparison, not trace-logger overhead

New host tool:

```
build-release/ps2bios_benchmark BIOS MODELED_EE_CYCLES SAMPLES [SLICE_QUANTUM]
build-release/ps2bios_benchmark --self-test
```

It executes fresh `Emulator::run_slice` boots, with a default 2,048-cycle outer
slice. Normal eight-cycle IOP and device event horizons remain in force. Only
run-slice execution is timed; BIOS input, reset/allocation, full-state digesting
and output are outside the interval. Samples must reproduce the first result.
There is no warmup, and the output explicitly labels that fact. The owned ROM
branch-loop self-test checks whole-system equality across 1/7/8/2048 quanta.

Modeled EE cycles include interrupt delivery and existing semantic fast-path
credits; they are **not a measured retired-instruction rate**, actual PS2 elapsed
time, frontend FPS, or physical Vita performance. Hashes cover public EE/IOP
architectural registers, full EE/IOP RAM, public VU state and VU1 banks, complete
logical VRAM, preview and graphics counts. They do not serialize private pending
pipeline/device state and are not a universal equivalence proof.

The frozen original library SHA-256 is
`45c6478d7b6d6cd7f9bcb1880c7d279e2a71ceb0dbe10de5f217ec10b7bda89f`.
Matching frozen headers avoid class-layout/ABI mistakes. Before any PAL change,
original versus dispatch-only at **300,000,000 modeled EE cycles** agrees:

| Digest | Both versions |
| --- | --- |
| CPU | 3AEC2B1B9469189B |
| EE RAM | E9D04C16E9EE405F |
| IOP RAM | 86102F3D3C0EEF75 |
| VU | 6AE85C0A5A4737A7 |
| Graphics | 0AE395B8C5C55A81 |

Both end at EE PC 00272728, IOP PC 0000AE94, fast-path credit 3,611,510,
998 VU pairs, 5,418 GIF submissions and 5,362 VIF submissions. The uncontrolled
single runs took 170.870343543 and 137.460069171 seconds. **Vita3K was open**;
these are not a quiet rotated comparison and establish no reliable FPS gain.
Do not conflate later PAL's deliberately changed workload with a dispatch speedup.

## Concrete PAL cadence mismatch

The actual BIOS configures `SMODE1=0000000740816504` (CMOD=3) and
`SYNCV=00A9000502101401` (front-porch low bit set). Memory previously used NTSC
cadence for every mode. The pinned [PCSX2 counter reference](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/Counters.cpp#L187-L205)
distinguishes PAL timing; its [mode/deadline update](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/Counters.cpp#L307-L388)
preserves the already pending deadline. Interlace detection uses CMOD/SYNCV,
not SMODE2.INT alone. These facts guided independently written integer code.

Support only confirmed analogue PAL interlace (CMOD=3, SYNCV bit0=1). Reset and
all other configurations retain the **explicit legacy NTSC fallback**, not a
claim that progressive/digital modes are correct. Existing CSR/GIF behavior is
unchanged; this is video-interrupt and Timer3 external-clock cadence, not scanout.

PAL durations at 294,912,000 modeled master cycles/s:

| Interval | Exact cycles |
| --- | --- |
| Field | 5,898,240 |
| Scanline | 18,874 + 46/125 |
| VBlank | 462,422 + 2/125 |
| Render | 5,435,817 + 123/125 |

Share fractional carry across render/blank phases so whole fields do not drift.
Update selection only when CMOD/SYNCV selection bytes change. A real selection
change resets old fractional accumulators but preserves current deadlines and
phase; the new cadence schedules the subsequent edges. Repeated same-mode,
reserved-bit and SMODE2 writes do not retime or reset carries.

`video_timing_tests` independently derives expected durations and passes **3,369
checks**, including 125 PAL fields, 625 scanlines, 100 retained NTSC fields,
all byte/half/word/doubleword writes through direct/cached/uncached aliases,
mid-blank/mid-scanline switches, reset, fallback and irregular-slice equality.
The same fixture against the frozen pre-PAL core fails **1,848 checks**, proving
the regression. An isolated strict ASan+UBSan build passes without diagnostics.

## Gates and local handoff

Release and UBSan each pass **56/56 CTest targets**, including the new owned
dispatch, video timing and benchmark self-test. VitaSDK package build passes.
Subsecond Windows/WSL timestamp-skew warnings occurred in some host make
dependencies; no global clock or security configuration was changed.

The VPK SHA-256 is
`d7f7a371e72f9e4a01950662cc80284013d0225297502164e3632b76283ac5a1`;
its `eboot.bin` matches the generated SELF. A separate Desktop copy is ready:
`AstraRecomp-PS2-Vita-2026-10-11-pal-dispatch.vpk`. The installed Vita3K app/user
BIOS are not changed by this turn. Firmware-derived captures and streams remain
ignored/local, never committed to GitHub.

## Fresh one-billion-cycle boot and visual record

The ignored experiment `.tools/intro_checkpoint_run.cpp` uses normal whole-system
run slices, with a GIF submission observer and source-bounded selected-display
captures every 50M cycles. It does not inject hardware events or skip ahead.
It finishes at **1,000,000,000 modeled EE cycles**, EE PC **00081FD8**, IOP PC
**0000AE98**, reason StepLimit. Mode observations first see the final PAL
registers in the 2,048-cycle outer sample ending at **245,676,544**; that is a
sample boundary, not an instruction-exact mode-write timestamp.

Terminal GIF stream: **68,083 records**, **48,379,520 payload bytes**, no GIF/VIF
packet rejections, 436,374 triangles, 2,461 sprites, 3,660 lines and zero points.
The last submission is at cycle 998,244,996, next EE PC 8000028C. The stream is
50,558,240 bytes and fully validates through the bounded reader/footer/checksum.
SHA-256:
`d3cc5393cde2e965ec741d7580dc0aececcabec3c15f97d0bad7f09e6b9bd992`.

This extends actual execution beyond the earlier 800M capture. Correct PAL
cadence deliberately changes the number/order of draws at a given cycle, so
**old 800M image hashes are not the expected output for this different workload**.
At new 800M there are 49,833 GIF records and 319,550 triangles; at 1B the final
selected DISPFB2 is **1450**, not 1400. FRAME is A0000, PMODE 66 and DISPLAY2
001FF9FF01848290. Always use each checkpoint's actual selected buffer.

New stream's own full-preview/full-logical-VRAM anchors:

| Grid | Preview hash | Logical VRAM word hash | Selected 640x256 region hash |
| --- | --- | --- | --- |
| Quarter | 516D80164CD3BFB1 | 23AC840ECC6669C2 | EBDD4780B54674AB |
| PC-only half | D4BF6DFD76F11B5D | FB74E24DEE27D63A | A9BB9ACD0D3BAE9B |

Visual inspection of the selected half-grid replay shows readable guest **Sony
Computer Entertainment** lettering over clouds and colored lights. It does
**not confirm the complete intro/towers/PS2 logo**. Default/Vita quarter sampling
still loses fine lettering; half-grid output is a PC diagnostic, not a Vita
resolution/FPS improvement or native GS scanout.

The 998 VU pairs remain normal packet-one E-bit termination,
not a target count to inflate. The unchanged first malformed PATH1 warning was
also reproduced by earlier reference modes; do not present it as the proven cause
of missing later geometry. No GIF/VIF packet rejections above must not be confused
with that separate VU PATH1 rejection. Later cloud draws are EE-generated DIRECT
work. The run used the user BIOS locally, not a fake intro image.

The local video preparation checks eight genuine 350/450/550/650/750/850/950/1000M
checkpoints. Each GIF prefix ends at or before its live checkpoint, the next
record is later, and the quarter replay's entire selected-region RGB matches
the fresh live capture. Half replay consumes that same prefix with more source
samples. The video holds each checkpoint for 1.5 seconds and clearly labels
**not real-time Vita, measured FPS, scanout or a verified complete intro**.
Local video:
`build-release/video-pal-progress-2026-10-11/AstraRecomp-PS2-PAL-intro-progress.mp4`.
It is H.264/yuv420p, 1280x352, 360 encoded frames at 30 FPS, **12 seconds**, silent;
the 30 FPS is an encoding rate, not emulator performance. All eight live-RGB
comparisons, metadata probe and full video decode pass. File size is 196,533
bytes; SHA-256
`6fdfbf53e97e3e95b33a7ce872f3b18d32c70feba54f4b30fd8f7ed444e560ad`.
No generated replacement graphics, interpolated motion or sharpening is used.

Next: profile empty graphics-drain overhead, preserving all four completed
queues (Memory GIF/VIF, Vif DIRECT and Vu PATH1), and extend timed video-edge
observations/boot execution before blaming a normal VU program termination.
Real Vita FPS and missing geometry still need direct evidence.
