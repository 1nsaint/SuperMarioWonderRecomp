# Progress: Super Mario Bros. Wonder, NVN → Vulkan on macOS

Living log, newest first. The plan is [docs/NVN.md](docs/NVN.md); what we learn about the game is in
[docs/nvn/WONDER.md](docs/nvn/WONDER.md). Hades is out of scope for this track (it is done on PC and focused on
Android elsewhere).

**Target:** Wonder 1.0.0 (010015100B514000) running natively on the Mac (Apple M1, 16 GB, macOS 14), CPU recompiled,
NVN implemented over Vulkan (MoltenVK).

## Status by phase

| Phase | Issue | State |
|---|---|---|
| N0 shared core, per-game layout, probe | #7 | done for this track (Windows/Hades items left, not needed here) |
| N1 CPU side: recompile, boot to `nvnBootstrapLoader` | #8 | **in progress**: recompiler complete for Wonder; runtime on macOS next |
| N2 NVN reconnaissance (tracer, API.md) | #9 | not started |
| N3 device, pools, sync, window (first pixel) | #10 | not started |
| N4 textures and samplers | #11 | not started |
| N5 Maxwell shaders → SPIR-V | #12 | not started (shader inventory done) |
| N6 pipeline state and draws | #13 | not started |
| N7 whole game, performance | #14 | not started |
| NA AudioRenderer (`audren:u`) | — (new) | not started; the game can boot silent |

## Log

### 2026-09-28

**Extraction (Mac).** nstool v1.9.2 (macOS arm64 release) with our own `prod.keys`:
`nstool -x /secure` on the XCI, then `-x /0` (ExeFS) and `-x /1` (RomFS) on the program NCA. The cartridge has no
update partition: this is 1.0.0. Output in `private/wonder/{exefs,romfs}` (3.5 GB). Steps in WONDER.md.

**Probe.** NVN (`NintendoSDK_NVN-16_2_0`), SDK 16.2.0 (Hades: 9.3). Modules: `rtld`, `main` (53.6 MB), `sdk`
(13.3 MB), no `subsdk`. Engine: sead + NintendoWare G3d2/Ui2d/Vfx2/Font, Pia (online). 1,107 imports in `main`,
791 of them not in Hades. The loader table asks for all **536** NVN procs by name (the whole C API; what is used
comes out of the N2 trace).

**New finding: AudioRenderer.** Hades only used AudioOut (FMOD mixes inside the game). Wonder imports 197
`nn::audio` renderer functions: voices, ADPCM/Opus, reverb, delay, aux, sub/final mixes. The service side
(`audren:u`, the audio DSP) must be implemented. Added as phase NA.

**MoltenVK on the M1** (1.4.2 via Homebrew, Vulkan 1.4):
has `external_memory_host` (pools imported, no shadow copies), extended dynamic state 1-3, dynamic rendering,
timeline semaphores, descriptor indexing, sync2, robustness2, ASTC and BC natively. Missing: geometry shaders,
transform feedback, logicOp, `vertex_input_dynamic_state`, float64 in shaders.

**Shaders in the RomFS** (all plain zstd, no dictionary): `Shader/SecredUber...bfsha` (42 MB, G3d2 uber shader),
`Shader/Secred...sarc` (agl shaders), Vfx2 effects (`Effect/static...esetb.byml`, 102 MB, plus 33 per-effect),
layouts (`Layout/*.blarc`).

**Recompiler (issue #8).**
- `main`: 209,629 functions, 16.3M instructions (10x Hades), 1,742 jump tables.
- `sdk`: 40,395 functions, 1.5M instructions.
- Crashes fixed: a register Capstone cannot name (literal `ldr` read the address as a register), a scalar by-element
  `fmul` without an arrangement.
- Silent miscompile fixed: vector `clz`/`cls` were emitted as general-purpose register ops (caught by difftest).
- New instructions: AdvSIMD scalar forms (one lane), `fcvtzs/fcvtzu` to a SIMD register, `fmaxp/fminp/fmaxnmp/
  fminnmp`, `smaxp/sminp/umaxp/uminp`, `sminv/uminv`, `facge/facgt`, `sqrdmulh`, `shsub/uhsub`, `srshl`, `rshrn`,
  `uabdl/sabdl`, `umlsl/smlsl`, `sadalp/uadalp`, `ld2r/ld3r/ld4r`, `scvtf/ucvtf` vector fixed point, `xpaclri`,
  exclusive pairs `ldxp/ldaxp/stxp/stlxp`, **all LSE atomics** (`cas*`, `casp*`, `ld{add,clr,eor,set,smax,smin,umax,
  umin}*`, `st*`, `swp*`), crypto extension (AES, SHA-1, SHA-256, PMULL.1Q; host instructions on arm64, a software
  version elsewhere, both tested).
- `frsqrts/frecps` are now fused like hardware (one rounding, no intermediate overflow).
- `tests/difftest.py` runs on macOS/Linux now (clang++, mmap + signals, Unicorn CPU model MAX for ARMv8.1+).
  Results: `main` 2,414 tests / 28,968 runs, `sdk` 3,472 tests / 55,552 runs, **0 mismatches**.

## Next

1. Commit the recompiler work (branch `feat/nvn-n1-wonder-cpu`, stacked on `feat/nvn-n0-core-split`).
2. `runtime/src/platform_mac.cpp` (SDL3 window/input/audio, MoltenVK surface), CMake for macOS.
3. `games/wonder/game.cpp`, module images (`tools/nso2bin.py`), build `main` + `sdk` + runtime on the Mac.
4. Boot: TIPC `sm:`, new SVCs and IPC commands of SDK 16, offline stubs for friends/NEX/bcat/prepo, until the
   first `nvnBootstrapLoader` call.
5. N2: HLE `nvnBootstrapLoader` / `nvnDeviceGetProcAddress` with a logging thunk per proc.
