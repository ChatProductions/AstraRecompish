# NFSMW 2005 PS2 → Vita: AstraRecomp research checkpoint 0.2

This branch is an experimental *native recompiler* starting point, not a
playable PS2 game or an NFSMW VPK. It does not modify the existing NFSMW Vita
port, its car files, or any saves.

## Proven on the supplied USA PS2 ELF

Original filename: `SLUS_213.51` (not included in source distributions).

- MIPS R5900 ELF32, one load segment, entry `0x00436128`.
- AstraRT AOT now supports `PADDUW` (unsigned saturated addition of each
  32-bit lane of a 128-bit general-purpose register), `MTHI`, `MTLO`,
  `MTHI1`, `MTLO1`, the R5900-only `MTSAH`, and COP1 `MTC1`.
- Generated host C++ translates the first **66** NFSMW startup instructions.
- Host differential test matches the interpreter's GPR low/high halves,
  HI/LO accumulator pairs, FPR array, SA register, PC, cycles and COP0 Count
  after these instructions.
- The instruction at `0x004361ac` is **MTSAH**, not a REGIMM branch. Both
  engines now execute it and 32 `MTC1` register transfers, then stop at
  `0x00436230` before unsupported COP1 `ADDA.S`.
  This interpreter fallback **does not**
  establish that the game can boot, load assets, or render.
- 22 AstraIR unit tests pass; the generated package was linked into a host
  C++17 binary and passed its end-to-end differential check. No VitaSDK or
  Vita3K run has been completed on this checkpoint.

## Reproduce with your own game

From the repository root (CMake and a C++17 compiler required):

```sh
cmake -S . -B build-nfsmw -DCMAKE_BUILD_TYPE=Release \
  -DASTRART_GAME_ELF=/absolute/path/to/SLUS_213.51
cmake --build build-nfsmw --target nfsmw_entry_probe -j4
ctest --test-dir build-nfsmw -R '^nfsmw_entry_probe$' --output-on-failure
```

Or generate inspectable C++ without a host build:

```sh
python3 tools/generate_astrart.py \
  --output /tmp/nfsmw_startup_aot.cpp \
  --package-name private-nfsmw-entry \
  /absolute/path/to/SLUS_213.51
```

Keep `SLUS_213.51` and any generated source translated from it private.
Neither is included in the release package.

## Work required before a Vita test

1. Continue through `ADDA.S`, `SYNC`, `CTC1` and subsequent real startup
   control flow, with a differential test at every stage.
2. Add a title-specific ELF + required disc-file loader in AstraRT. Compare
   the same input against PCSX2 before writing game-specific HLE shortcuts.
3. Integrate an opt-in game package and execution mode with Astra's Vita
   frontend; do not replace the research smoke-test package globally.
4. Investigate required IOP, VIF/VU1, GIF/GS, SPU2, CD/DVD and scheduling
   services, as well as Vita memory pressure. These are not solved by AOT.
5. Cross-build with VitaSDK, verify through Vita3K, then measure correctness,
   frame output and performance on actual Vita hardware.

AstraRecomp is MIT licensed. No game data, PS2 BIOS or proprietary material
from its executable belongs in a public source repository.
