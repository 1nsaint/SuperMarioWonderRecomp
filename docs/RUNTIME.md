# Runtime (phase 2)

The runtime turns the recompiled modules into a Windows program. Nothing is emulated at the CPU level: it plays
the part of rtld (loading and linking) and of a small Horizon kernel, and replaces system services.

## Modules

Two modules are recompiled and linked into one executable:

| Module | Base | Why |
|---|---|---|
| `main` | `0x40_0000_0000` | the game (Supergiant engine, The Forge, FMOD, Bink, Lua) |
| `sdk` | `0x40_1000_0000` | Nintendo's SDK: musl libc, libc++/libc++abi, nn::os, nn::fs, nn::sf, ... |

The `sdk` has to be recompiled, because `main` inlines libc++'s object layouts (std::string, locale, ...) and
imports them by name. The host C/C++ runtime could not be substituted. `subsdk0` (Nintendo's Vulkan driver) and
`subsdk1` (OpenGL) are not loaded. Their exports are replaced by host Vulkan (phase 3).

`python -m recomp <nso> private/gen/<name> --name <name> --base <addr>` emits the code plus `table.cpp`:
functions, relocations, symbols and init arrays as `nx::ModuleInfo`. The module image (text/ro/data) is read
at startup from `private/analysis/<name>.bin`, produced by `tools/nso2bin.py`. The generated code carries no game
bytes.

## Boot (`runtime/src/main.cpp`)

What rtld's `_start` does on Horizon, reverse engineered from `rtld`:

1. Map each module at its base and zero its bss (`loader.cpp`).
2. Apply relocations. `RELATIVE` → base + addend. `GLOB_DAT`/`JUMP_SLOT`/`ABS64` → by name, looked up in this order:
   **HLE registry** first, then exports of the loaded modules, then a logging stub.
   The `sdk` calls its own exports through its PLT (7,986 `JUMP_SLOT`s), so any SDK function can be replaced by name.
3. Create the main thread (TLS page, 16 MiB stack).
4. Call `nn::init::Start(main thread handle, main base, rtld init, rtld fini)`. Everything after that is
   recompiled SDK code: libc init, static constructors, `nninitStartup`, `nnMain`.

## Address space (`memory.cpp`)

Guest pointers are host pointers. Fixed ranges are reserved at startup and committed on demand:

| Range | Use |
|---|---|
| `0x40_0000_0000` | modules |
| `0x50_0000_0000` | heap (svcSetHeapSize), up to 6 GiB |
| `0x58_0000_0000` | alias region (svcMapMemory destinations) |
| `0x68_0000_0000` | stack region |
| `0x70_0000_0000` | kernel allocations: TLS pages, main stack, shared memory |
| `0x7F_0000_0000` | HLE thunk addresses (never mapped; `nx::call` dispatches them) |

`svcQueryMemory` is answered from a Horizon-style memory map (`vm_set`/`vm_query`).
`svcMapMemory` copies instead of aliasing: the source is inaccessible while mapped on Horizon, so a copy on
map and a copy back on unmap behave the same.

## Kernel (`kernel.cpp`)

- **Threads:** one Win32 thread per guest thread, each with its own `nx::CPU` and a TLS page (`TPIDRRO_EL0`).
- **Synchronization:** Horizon semantics, under one kernel lock and condition variable:
  - `ArbitrateLock`/`ArbitrateUnlock` (mutex word = owner tag | waiters bit)
  - `WaitProcessWideKeyAtomic`/`SignalProcessWideKey` (condvars hand the mutex over)
  - `WaitForAddress`/`SignalToAddress`, including SignalAndModifyByWaitingCountIfEqual
- **Events and waiting:** `CreateEvent`/`SignalEvent`/`ClearEvent`/`ResetSignal`, and `WaitSynchronization` on events and threads.
- **Memory and info:** `SetHeapSize`, `QueryMemory`, `MapMemory`, `MapPhysicalMemory`, `GetInfo` (regions, program id, entropy), `GetSystemTick` (19.2 MHz from QPC).
- **System registers:** CNTVCT/CNTFRQ/DCZID. `TPIDR_EL0` and `TPIDRRO_EL0` live in the context.

## Services (`ipc.cpp`)

A HIPC/CMIF framework parses the request in TLS: special header, copy/move handles, PID, X/A/B/W/C descriptors,
and domains. It dispatches to a `Service` object and builds the response, returning interfaces as domain objects
or as new sessions. `sm:` is implemented. Unknown services and commands log and succeed, so boot shows
which ones matter.

## Indirect calls

`nx::call(c, target)` finds the module containing `target` and calls through a dense table (text offset / 4 →
function). Otherwise the target is an HLE thunk. Calling into the middle of a function is fatal and logged with
module+offset and the nearest symbol.
