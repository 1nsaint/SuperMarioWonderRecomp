# Android

The same recompiled code runs on Android (arm64-v8a): the generated C++ is portable, and the runtime talks to
the OS through `runtime/src/platform.h` (`platform_win.cpp` / `platform_android.cpp`).

| | Windows | Android |
|---|---|---|
| Memory | VirtualAlloc at fixed addresses | mmap `MAP_FIXED_NOREPLACE` (fails loudly if a range is taken) |
| Threads | Win32 threads | pthreads (64 MiB host stacks, 256 MiB for the guest main thread) |
| Window | Win32 window, F11 fullscreen | NativeActivity window, full screen under the camera cutout |
| Input | XInput pad + keyboard | game controllers (Bluetooth/USB) + touch |
| Audio | XAudio2 | AAudio (low latency, callback) |
| Vulkan | vulkan-1.dll, VK_KHR_win32_surface | libvulkan.so, VK_KHR_android_surface |

The game renders at the phone's own resolution and aspect by default (Hades handles wide screens itself: at
19.5:9 it draws its own side art). `config.txt` in the data folder can set another one, e.g. `resolution=1560x720`
for more speed.

## Build and install

```powershell
. .\dev.ps1
android\build_apk.ps1                      # libhades.so (NDK r28, ~45 MB stripped) -> private\apk\hades.apk (~15 MB)
tools\push_android.ps1                     # install + copy your data to the phone + start
tools\push_android.ps1 -Lean               # without the other languages' fonts (~45 MB less)
tools\push_android.ps1 -SkipData -Resolution 1560x720
adb logcat -s hades                        # the runtime's log (also in Android/data/<pkg>/nx.log)
```

Data on the phone (`/sdcard/Android/data/io.github.belmantegu.hadesrecomp/files/`):

| Path | What |
|---|---|
| `analysis/main.bin`, `analysis/sdk.bin` | module images (`tools/nso2bin.py`) |
| `extract/romfs/` | the RomFS with the update applied (3.9 GB) |
| `user/save/` | saves (created by the game) |
| `config.txt` | optional settings |

## Size

The APK carries only code (15 MB). The game data cannot shrink much: `Movies` (2.1 GB) are not cutscenes but the
characters' animations as Bink videos, `Audio` (961 MB) are FMOD banks and `Win/Packages` (583 MB) the 720p
textures, all already compressed and all used. Only the fonts of other languages are optional.

## Touch controls

No overlay is drawn yet: the left half of the screen is a virtual stick (drag from where the thumb lands), the
right half has four zones: up-left Y, up-right X, down-left B, down-right A; along the top: L, +, ZR, R.
A Bluetooth controller is the better way to play for now.

## Known limits

- Leaving the app (home button) loses the surface; come back only by restarting for now.
- The address space: the guest ranges sit at 0x40_0000_0000-0x6F_0000_0000, below the 39-bit top where
  Android maps libraries; a failed reservation is reported in the log.
