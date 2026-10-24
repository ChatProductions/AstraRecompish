# NFSMW 2005 PS2 → Vita: AstraRecomp research checkpoint 0.3

This branch is an experimental *native recompiler* starting point, not a
playable PS2 game or an NFSMW VPK. It does not modify the existing NFSMW Vita
port, its car files, or any saves.

## Proven on the supplied USA PS2 ELF

Original filename: `SLUS_213.51` (not included in source distributions).

- MIPS R5900 ELF32, one load segment, entry `0x00436128`.
- AstraRT AOT now supports `PADDUW` (unsigned saturated addition of each
  32-bit lane of a 128-bit general-purpose register), `MTHI`, `MTLO`,
  `MTHI1`, `MTLO1`, the R5900-only `MTSAH`, COP1 `MTC1`, `CTC1`,
  `ADDA.S` and `SYNC`.
- Generated host C++ translates the first **76** NFSMW startup instructions,
  up to PC `0x0043626c`, including its first completed branch.
- With a maximum of eight guest instructions per AOT entry, the package
  resumes across Astra's eight-cycle IOP scheduling quantum. Branches and
  their delay slots remain inseparable during code generation.
- Host differential test matches the interpreter's GPR low/high halves,
  HI/LO accumulator pairs, FPR/FCR arrays, FPU accumulator, SA register,
  PC, cycles and COP0 Count
  after these instructions.
- Crucially, a second integration test enables the AOT package through the
  real `Emulator::run_slice()` path. It successfully runs the 76 instructions
  in scheduler-sized pieces with the same guest state as the interpreter.
- The optional generated title package verifies the original guest instruction
  words at each native entry, falling back to the interpreter if the ELF
  differs. A host test deliberately modifies one instruction and confirms
  that no native instructions execute.
- 25 AstraIR unit tests pass; the generated package was linked into a host
  C++17 probe. No VitaSDK, Vita3K, or physical Vita test has been completed.

These results **do not** establish that the game can boot, load assets, or render.

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
  --guard-guest-code --max-block-instructions 8 \
  /absolute/path/to/SLUS_213.51
```

Keep `SLUS_213.51` and any generated source translated from it private.
Neither is included in the release package.

## Opt-in Vita experiment, once VitaSDK is installed

A local Vita build can include the private title package without committing
the proprietary ELF or the generated C++ to this repository:

```sh
cmake -S . -B build-vita-nfsmw -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DASTRART_PRIVATE_VITA_ELF=/absolute/path/to/SLUS_213.51
cmake --build build-vita-nfsmw --target ps2vita.vpk -j4
```

Copy the **same** personally obtained ELF to
`ux0:data/ps2vita/boot.elf` before trying the custom build. In the Vita
frontend, use the existing **TRIANGLE** external-ELF load action. The
opt-in package is armed after that load; firmware/BIOS boot and ordinary
builds remain interpreter-only. If the ELF's loaded code differs, individual
AOT entries reject it and fall back to interpretation.

This is a development instrumentation path, **not a playable Vita port**.
No Vita-side binary has yet been built or measured in this environment.

## Work required to turn this into a game

1. Continue through the next branches and indirect call targets of the real
   startup control flow, with differential tests at every stage.
2. Add a title-specific ELF + required disc-file loader in AstraRT. Compare
   the same input against PCSX2 before writing game-specific HLE shortcuts.
3. Measure the new opt-in execution mode on real Vita or Vita3K, and fix any
   device-specific timing or recompiler semantics before expanding coverage.
4. Investigate required IOP, VIF/VU1, GIF/GS, SPU2, CD/DVD and scheduling
   services, as well as Vita memory pressure. These are not solved by AOT.
5. Only once ELF startup and game services work, attempt frame output and
   performance analysis on Vita hardware.

AstraRecomp is MIT licensed. No game data, PS2 BIOS or proprietary material
from its executable belongs in a public source repository.
