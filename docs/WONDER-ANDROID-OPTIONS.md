# Wonder Android launcher: options

The launcher (`android/wonder/`, package `io.github.belmantegu.wonderrecomp`) is ported from the Castlevania one and
drawn as in `android/launcher-mockup/index.html`. Options are stored in two files, and the runtime applies the **first
line** of a key (`platform_android.cpp` sets environment variables without overriding), so the launcher rewrites that
line and drops later duplicates (`GameConfig`):

- `config.txt` in the data folder (`Android/data/io.github.belmantegu.wonderrecomp/files/`): `NX_*` lines become
  environment variables before the game boots; plain keys (`render_scale`, `trace`) are read with `plat::setting()`.
- `app.txt` (app-internal): language, FPS counter, advanced rows. Only the launcher reads it.

A row left at the runtime's own default has no line, so the runtime's defaults keep applying. Rows marked *explicit* are
always written. A row the runtime does not read is not shown: no dead switches. Every option below that changes the game
restarts it (the game runs in its own process, `:game`, and reads `config.txt` once at start).

## Rows

| Row | Stored as | Reader in the runtime | Default (no line) | Status |
|---|---|---|---|---|
| Console mode (handheld / TV) | `NX_HANDHELD=1` / `0` (always written) | `svc_am.cpp`, `svc_misc.cpp` (tests that the variable exists); `platform_android.cpp` now removes it when it is `0` | handheld 720p | live; the `0` case is a 6-line runtime change |
| Internal resolution | `NX_RENDER_RES=WxH` (960x540 .. 1920x1080, 16:9) | `svc_am.cpp` GetDefaultDisplayResolution | the console mode's own size | live |
| Window scale | `render_scale=0.5..1` | `platform_android.cpp` `setting("render_scale")` | by GPU (1 on Adreno 7xx+) | live |
| FSR 1 / sharpness | `NX_FSR=1`, `NX_FSR_SHARP=0..2` | `nvn_present.cpp` | off / 0.2 | live |
| FPS limit (60 / 30) | `NX_FPS_CAP=30` (no line = the game's own rule) | `nvn_queue.cpp` `frame_time_probe` (forces interval 2 and the game's interval fields, governors off), `perf_hint.cpp` (ADPF target 33.3 ms) | the game's rule: 60, 30 when heavy | **code written, built into the APK, NOT yet run on the phone** (game speed at 30 not verified) |
| GPU submits in flight | `NX_PIPELINED=2..4` | `nvn_frames.cpp` | **synchronous** (see below) | live, tagged experimental |
| GPU turbo | `NX_GPU_TURBO=1` | `perf_hint.cpp` | off | live |
| Memory limit | `NX_MEM_LIMIT_GB` | `memguard_android.cpp` | 60% of RAM | live |
| CPU cores (advanced) | `NX_AFFINITY=1`/`2` | `frame_stats.cpp` (modes 0, 1, 2) | 0, the scheduler decides | live |
| Draws per submitted part (advanced) | `NX_SUBMIT_CHUNK_DRAWS=0`/`400` | `nvn_frames.cpp` | 150 on Android | live |
| Pipeline cache (advanced) | `NX_PIPELINE_CACHE=0` when off | `nvn_pipeline_cache.cpp` | on | live |
| On-screen buttons | `NX_TOUCH_SHOW=0` when off | `touch_layout.cpp` `touch_enabled()`, `platform_android_input.cpp` | on | **ported** (new, small) |
| Button opacity / size | `NX_TOUCH_OPACITY` 10..100 (85 = the old look), `NX_TOUCH_SIZE` 50..160 | `platform_android_input.cpp` overlay alpha, `touch_layout.cpp` radius | 85 / 100 | **ported** |
| Edit buttons, mirror, reset | `NX_TOUCH_LAYOUT=A:x,y,r;B:...`, `NX_TOUCH_MIRROR=1` | `touch_layout.cpp` `wonder_buttons()`, stick side in `platform_android_input.cpp` | the original layout | **ported**; the editor is `LayoutEditorActivity` |
| Mute | `NX_MUTE=1` | `platform_android_audio.cpp` `audio_open` (same rule as Windows/macOS) | off | **ported** (2 lines) |
| Trace system calls (advanced) | `trace=1` | `main.cpp` `setting("trace")` | off | live |
| FPS counter, position | `app.txt` `fps_mode`, `fps_pos` | `FpsOverlay` over `GameActivity.nativeFrames()` (frames presented, `nx::g_frame`) | off | live (one JNI function in `platform_android.cpp`) |
| Language, advanced rows | `app.txt` | the launcher | auto / off | live |
| Data: state, space, import, wipe, clear cache | none | files under the data folder (`pipelines_*.bin`, `analysis`, `extract`, `spirv`) | | live |
| Saves export / import | none | zip of `user/save/` (`hle_fs.cpp` mount) | | live |
| Logs, save diagnostics, This device | none | `nx.log`, `nx.log.prev` (`loader.cpp`), `Diagnostics` | | live |

### Hidden rows of the mockup (marked PORTAR there)

| Row | Why it is not offered |
|---|---|
| FXAA | Wonder has no FXAA pass (`nvn_present.cpp` only has FSR). Porting Castlevania's pass is a new shader and a new pipeline in the present path: not small, and not testable without a device. |
| Frame limit as a plain sleep | A sleep-only cap would slow the game's logic; the shipped `FPS limit` row forces the game's own interval 2 instead. |
| Full screen Hor+ (`widescreen`), display size (`display_res`) | Wonder renders the game's own 16:9 (the present pass adds bars). There is no Hor+ in the runtime. |
| Vibration, floating / fixed stick | The native stick already appears where the thumb lands. Haptics would need a JNI vibrator call from the input thread. |
| Game volume, audio latency | `platform_android_audio.cpp` opens AAudio with fixed settings and has no gain. |
| Button art / circles | The runtime always draws plain circles; there is no art to choose. |
| Driver GPU tab (custom Turnip, download, test) | Castlevania ships hook libraries from another APK (`libhook_impl.so` ...) for this. Wonder's runtime loads `libvulkan.so` directly. "This device" (Diagnostics tab) shows the GPU and Vulkan facts instead. |
| Send report | There is no report server for Wonder. "Save diagnostics" writes a zip you can share. |

### Differences from the mockup's defaults (checked against the runtime)

- **GPU submits in flight**: the mockup said `2 (default)`. The runtime default on Android is synchronous (`pipelined()` is
  only on by default on macOS; commit dc3d5fc found exploded meshes with pipelining, DEBUG-2 #76). The row keeps
  synchronous as the default and tags 2/3/4 as experimental.
- **CPU cores**: the mockup said `3 (default)`. `frame_stats.cpp` knows modes 0, 1 and 2 (3 behaves as 1). Default is 0.
- **Internal resolution** is 16:9 (the game's own aspect), not "the phone's aspect".

## Import

`LauncherActivity` asks for a folder (SAF, no storage permission) holding `prod.keys` (+ `title.keys`) and the dump. It passes
`.xci` (and split parts `.xc0`, `.xci.00` ...), `.nsp`, `prod.keys`, `title.keys`, `keys.txt` to `NativeImporter.run` with
title `010015100B514000` (Super Mario Bros. Wonder 1.0.0, `games/wonder/game.cpp`). The recompiled code is for **1.0.0**: the
launcher tells the player to put only the 1.0.0 dump there (the importer applies an update it finds in the folder, which would
change `main` and break the recompiled code).

The importer writes `analysis/*.bin`, `analysis/main.npdm` and `extract/romfs/`. It does not write `spirv/`: the translated
shaders come from the APK (`assets/spirv.zip`, made by `android/build_apk.sh` from `private/wonder/spirv`, which needs the
player's own dump and `tools/maxwell/spirv_harness`), and `config.txt` is created on the first import or Play. The button
font (`fonts/shared_5.ttf`) is not produced: on Android the runtime falls back to a system font (`hle_pl.cpp`).
A "data .zip" made by `tools/pack_phone_wonder.py` is also accepted.

## Vulkan 1.3 requirement

`android/wonder/AndroidManifest.xml` requires Vulkan 1.3 (`0x403000`). Reading `runtime/src/nvn/`: the layer asks for API 1.3 and
chains `VkPhysicalDeviceVulkan13Features`, uses dynamic rendering (`vkCmdBeginRendering`, `nvn_draw.cpp`), **legacy**
`vkCmdPipelineBarrier` (no synchronization2) and **binary** semaphores plus fences (no timeline semaphores). Dynamic rendering is
the only 1.3 feature actually needed. A device on Vulkan 1.1/1.2 would need: `VK_KHR_dynamic_rendering` enabled and the KHR
entry points used (or `NX_RENDER_PASS_OBJECTS=1` made the default: that path exists in `nvn_renderpass.cpp`, not measured),
the 1.3 features struct chained only on 1.3, and a check of the translated shaders' SPIR-V version and capabilities. Effort:
about a day of code plus testing on a real 1.1/1.2 phone, which nobody here has run. The target (Adreno 740) is 1.3, the gain is
only devices that are unlikely to run the game fast enough, so the manifest and runtime are left at 1.3.

## Not verified

None of this has run on a device. The launcher compiles (javac/d8), the APK passes `apksigner verify` and `aapt2 dump badging`,
`OptionsModel`/`GameConfig`/`ControlLayout` have a plain-JVM test of the file rules, and the runtime edits (`touch_layout.cpp`,
`platform_android_input.cpp`, `platform_android_audio.cpp`, `platform_android.cpp`) pass `clang -fsyntax-only` with the NDK.
