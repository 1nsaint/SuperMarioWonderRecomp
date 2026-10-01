# Wonder on Android: measurements (Galaxy S23)

Device: Galaxy S23 SM-S911B. Snapdragon 8 Gen 2, Adreno 740, Vulkan 1.3, 8 GB of RAM (6.9 GB visible), Android 16.
Handheld mode (`NX_HANDHELD=1`: the game renders for 1280x720). Build: branch `feat/android-wonder` after the
Windows merge (DEBUG-2.md #89). The studies that build on these numbers are in this folder.

## How they are taken

| Tool | What |
|---|---|
| `tools/wonder_phone_course.sh <serial> <name> [NX_X=Y ...]` | scripted course 1-1 run on the phone. It writes `config.txt` with the timed inputs, dumps a frame every 600 and samples memory every 20 s, takes a 10 s CPU profile at 220 s, and pulls everything into `private/wonder/phone_<name>/` |
| `REPLAY=<file>` (with the script above) | replays a recorded play instead of the blind script |
| `NX_INPUT_RECORD=1` (`config.txt`) | records what the player does into `input_record.txt`, in `NX_INPUT_SCRIPT` format (`runtime/src/input_script.cpp`) |
| `tools/wonder_replay_script.py <input_record.txt>` | the scripted menus, then the recorded course inputs shifted to when the run enters the course |
| `tools/wonder_phone_memwatch.sh <serial> <out>` | memory from `dumpsys meminfo` every 20 s while the app runs: total PSS, the Graphics part (GPU), native heap, the phone's MemAvailable |
| `tools/android_profile_report.py <profile.txt>` | samples per thread, libraries, hottest functions of our code, symbolized against `private/wonder/ba/libwonder.so` (the build that ran) |
| `NX_GPU_PROFILE=1` | GPU time per render pass (timestamps), top labels every 60 frames |

## Results (2026-10-01)

| Run | Mode | fps (whole run) | Course only | Replay (CPU) | Fence wait | Notes |
|---|---|---|---|---|---|---|
| user play, 2026-09-30 (old APK) | pipelined | 37.1 mean | - | 18-26 ms in course | 1-8 ms | first run: every pipeline compiled (43-47 ms spikes) |
| `sync1` | synchronous (default off macOS since #76) | 30.4 mean, 53% of intervals < 30 | - | - | - | blind script; cold pipeline cache |
| `phone_rec` (user playing, recorded) | pipelined | 53.0 mean, 11% < 30 | - | - | - | the user's own run |
| `pipe1` (the user's run replayed) | pipelined | 43.8 mean | **32.0** | **19.6 ms** | 5.3 ms | pipeline cache warm (0.5 ms per pipeline) |
| `gpu1` (same, GPU profile on) | pipelined | 32.4 mean | - | - | - | **GPU 17.0 ms/frame**; one 960x540 pass 5.1 ms |

Conclusions so far:
- Pipelined submission is worth ~20 fps on the phone over synchronous. Whether it can be the Android default
  depends on the exploded-mesh cause (`PIPELINED-AND-FLICKER.md`).
- In the course both sides are over the 16.7 ms frame: the CPU replay at ~19.6 ms (the M1 does it in ~6) and the
  GPU at ~17 ms.
- On the replay thread (`phone_pipe1/profile_report.txt`): our code takes 38% of the samples, libc 34%
  (copies, allocations), the Adreno driver 26%. The hottest places are `draw` (`vkCmd*` into the driver),
  `replay_list`, `clears_flush` and map lookups.
- Adreno compiles a new pipeline in ~15-20 ms (149 pipelines took 3.1 s in one window of the first run), which
  makes the on-disk pipeline cache (DEBUG-2 #73) essential: warm, 0.5 ms each.

Memory (`dumpsys meminfo`, MB):

| Run | App total (peak) | Graphics (GPU) | Phone available |
|---|---|---|---|
| `sync1` | 3,280 | 1,066 | ~950-1,230 |
| `phone_rec` | 3,640 | 1,168 | ~1,000 |
| `pipe1` | 3,695 | 1,207 | ~930-1,400 |

The runtime's own guard (Pss from `smaps_rollup`) reported only 0.5-1.2 GB: it does not see most of it
(`MEMORY.md`).

Graphics bugs reported by the user on the phone:
- the background's glow flickers from frame to frame;
- other graphics bugs, not yet localised.

The sampled frames of `pipe1` look clean: the flicker is temporal and needs frame bursts
(`PIPELINED-AND-FLICKER.md`).
