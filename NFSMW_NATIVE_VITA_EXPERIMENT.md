# NFSMW 2005 PS2 → Vita: AstraRecomp research checkpoint 0.3

This branch is an experimental *native recompiler* starting point, not a
playable PS2 game. It does not modify the existing NFSMW Vita port, its car
files, or any saves. Private title-specific test VPKs are distributed through
VitaTunnel-Relay; `ux0:data/ps2vita/boot.elf` is supplied independently and
is **never** included inside these VPKs.

## October 29, 2026: host replay replaces the VPK-per-syscall cycle

Vita r4 stopped at `0x00495904`, syscall `0x78` (`sceSifSetDChain`), after
6,855,355 guest cycles. Its preceding debug call printed `main() argc = 0`.
The new host-only `nfsmw_startup_replay` tool reproduces **exactly** that
cycle count, instruction address, syscall and debug string using the user's
private ELF without a Vita transfer or any private binary in this repository.
This is a debugging interpreter on the development computer, **not** a
proposed PS2 emulator to install on Vita.

The EE-side SIF0 CHCR initialization (`0x184`) and the real/software SIF
register accesses (`sceSifGetReg`/`sceSifSetReg`) now have narrowly scoped
implementations with host regression tests. With these calls handled, the
same guest reaches `0x00496D28`–`0x00496D38`: a loop reading SIF register 4
(`SMFLAG`) until bit `0x00020000` (`SIF_STAT_CMDINIT`) becomes set.
The replay detects this loop at approximately 7.2 million cycles with
`sif_smflag=0` and **`iop_cycles=0`**.

This is a real architectural dependency: the bare-ELF path currently does
not boot or execute the IOP firmware/modules, so no IOP-side SIFCMD handshake
can occur. Simply fabricating the ready bit would conceal the problem and
lead to more arbitrary startup failures. There is therefore **no new Vita
test VPK** for these host-only changes. A genuine IOP service replacement
(SIFCMD/RPC and required module/asset services), or an explicitly supported
IOP execution environment, must be designed and validated first.

To reproduce the stall on a host C++17 toolchain, with your own private ELF:

```sh
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release
cmake --build build-host --target nfsmw_startup_replay -j4
build-host/nfsmw_startup_replay /absolute/path/to/boot.elf 15000000
```

The expected final diagnostic is `reason=blocked-sifcmd-no-iop`, not
successful game initialization. The game ELF and any generated AOT are never
placed in source control by this workflow.

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
- 25 AstraIR unit tests passed at the original checkpoint. Later VitaTunnel
  hardware testing and the host startup replay are described above.

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

## Public GitHub Actions build

The [Vita VPK workflow](https://github.com/ChatProductions/AstraRecompish/actions/workflows/vita-vpk.yml)
builds the general AstraRecomp frontend and uploads the resulting VPK as a
downloadable Actions artifact. It runs on pushes to `main` and this research
branch, or manually via **Run workflow**. It does **not** include the private
NFSMW executable or any game-specific AOT instructions, and therefore does
not constitute an NFSMW game build.

## Opt-in Vita experiment (maintainer build environment)

A local Vita build can include the private title package without committing
the proprietary ELF or the generated C++ to this repository:

```sh
cmake -S . -B build-vita-nfsmw -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DASTRART_PRIVATE_VITA_ELF=/absolute/path/to/SLUS_213.51
cmake --build build-vita-nfsmw -j4
```

Copy the **same** personally obtained ELF to
`ux0:data/ps2vita/boot.elf` before trying the custom build. In the Vita
frontend, use the existing **TRIANGLE** external-ELF load action. The
opt-in package is armed after that load; firmware/BIOS boot and ordinary
builds remain interpreter-only. If the ELF's loaded code differs, individual
AOT entries reject it and fall back to interpretation.

This is a development instrumentation path, **not a playable Vita port**.
The user does not need VitaSDK to install a test VPK through VitaTunnel.

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
