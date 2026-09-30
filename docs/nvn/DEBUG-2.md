# Debugging the NVN → Vulkan path (continued)

Continues [DEBUG.md](DEBUG.md) (entries 1-58, and the environment variable table) from entry 59. Milestone W7 on
GitHub (issues #43-#48): the glitches in the user's recording of course 1-1 (`docs/media/wonder/glitches/`) and
60 fps.

## Bugs found this way (2026-09-30, W7), in order

59. **The game's CPU waits did not cover a submit still being replayed (issue #43, `nvn_queue.cpp`).**
    - The game records frame N+1 on one thread while another thread is inside `nvnQueueSubmitCommands` for frame N.
      Our replay runs inside that call.
    - `nvnSyncWait`, `nvnQueueFinish` and pool invalidate/finalize returned at once there: `gpu_serial()` is 0, and
      the replay-thread job is inline. So the game could rewrite vertex and uniform data that the replay had not read
      yet. This is the leading cause of the horizon band and the exploded meshes (`glitches/play-012`, `play-044`).
    - Now there are two counters, submits called and submits done (a mutex and condition variable, never the GPU
      lock):
      - a fence records the submits made before it;
      - a sync wait blocks until those are done;
      - `QueueFinish`, `MemoryPoolInvalidateMappedRange` and `MemoryPoolFinalize` wait for every submit made so far.
    - Each wait gives up after 2 s, and the timeout is logged. The log also prints how many waits blocked (`CPU waits
      blocked on a submit still being replayed`). The course run had one.
    - `NX_SYNC_LOOSE=1`: the old behaviour.

60. **Depth EQUAL colour pass after a pre-pass: a small depth bias toward the camera (issue #44, `nvn_raster.cpp`).**
    - The colour pass tests EQUAL (taken as LESS_OR_EQUAL since #13) against depth written by a pre-pass that uses a
      different vertex shader. Metal compiles the two separately. Where the colour pass lands a few ulps behind, the
      pixel keeps the pre-pass's (0,0,0,1): black silhouettes and black/white wedges on slopes (`play-019`).
    - Pipelines with EQUAL, depth tested and not written enable depth bias. At each draw they get the game's bias
      minus `NX_EQUAL_SLACK` units (default 4), minus a quarter of that per unit of slope, with no clamp.
    - It only lets through what lies within that distance of the surface the pre-pass kept. `NX_EQUAL_SLACK=0`: off.

61. **Pipelined fences on by default (issue #46, `nvn_frames.cpp`; PERF-PLAN step 3): course 50 → 60 fps.**
    - A submit with dispatches is split after its last `DispatchCompute`. The first part is queued at the split,
      while the rest is still being replayed. Only its fence is waited for, and the write-backs are copied before
      `gpu_submit` returns, so guest memory holds the compute results exactly when it does synchronously.
    - The fence of the rest, and of the frame, is never waited for. At most 2 submits are in flight.
    - Review fixes applied before turning it on:
      1. The upload ring wrapping over its own submit waits for that submit's first part (the GPU may still read it).
      2. A first part that failed to queue drops the submit's write-backs (they would copy unwritten buffers over
         guest memory). A rest that failed to begin is not queued.
      3. A draw that binds storage over a written region (it may write the copy) counts like a compute binding.
         The write-backs then wait for the whole submit.
      4. The throttle (oldest submit waited for) runs after the rest is queued. Before, the rest waited behind it.
    - Log: `pipelined: 60 submits, 60 split (first part: 75% of the replay), write-backs after the first part 60`.
    - Course 1-1 headless:

      | Mode | fps |
      |---|---|
      | synchronous | 48-58 |
      | pipelined | 57-60 |
      | with #62-#64 | 60.0 steady |

    - Images match the synchronous run (animation phase aside).
    - `NX_PIPELINED=0`: the synchronous path; `NX_PIPELINED=3|4`: more submits in flight.

62. **Windowed present mode: MAILBOX, else IMMEDIATE, FIFO last (issue #47, `nvn_vk.cpp`).**
    - FIFO waited for the display's refresh while the game paced itself on its own 60 Hz vsync event: two clocks
      drifting against each other.
    - IMMEDIATE is now preferred over FIFO when MAILBOX is missing. On macOS, IMMEDIATE only removes the Metal
      layer's display sync; the compositor still shows whole frames.
    - The chosen mode is logged (`present mode ...`).
    - With pipelined fences, the blit's fence is not waited for either (`vkQueuePresentKHR` waits on the semaphore).
    - `NX_FIFO=1`: FIFO.

63. **Replay marks: dropped snapshots and sampler slots (issue #48).**
    - `drop_snapshot` erased the snapshot copy without `note_replaced`. A replay whose mark predates the new
      registration resolved the slot to that snapshot key and found nothing. Now the copy is noted as retired: it
      stays alive in the deferred list until the replay floor passes it.
    - Sampler pool slots had no history. A sampler re-registered with other state mid-replay was seen early. Now
      changes are recorded (only when the state differs: re-registering the same sampler every frame records
      nothing), and `sampler_entry_at` resolves the slot at the mark.
    - Regression caught in the course run: a slot **empty** at the mark fell back to `SamplerInfo{}` (nearest,
      clamp, `maxLod` 0.25, the #50 bug). The ground lost its grass and turned black past the brick stair. NVN reads
      descriptors when the GPU runs, so the game may register a slot after recording the lists and before
      submitting. An empty slot at the mark now uses the current entry.

64. **A texture slot or object empty at the mark resolves to the current one (issue #45, flat green silhouettes).**
    - The same rule as #63, for texture pool slots (`pool_entry_at` returned the "no texture" entry) and for objects
      (`find_object` returned null for an object created after the mark but before the replay). Both fell to the
      zero dummy for a few frames: the leading hypothesis for the flat green layers (`play-017`, `play-069`).
    - `NX_MARK_EMPTY_OLD=1`: the empty entry, as before, for bisecting. Confirmation needs a windowed run (the
      silhouettes were never seen headless).

    ![Course at 60 fps](../media/wonder/38-course-60fps-pipelined.png)

## W8 (2026-09-30, night)

65. **Button icons showed as "?" (issue #49): our own NintendoExtended font.**
    - The game draws button icons as Private Use Area characters of shared font 5 (NintendoExtended). `hle_pl.cpp`
      gave it Arial Unicode, which has none of them. The console's fonts (system archives 0100000000000810-814 of
      firmware 23) are key generation 23, and our keys stop at `master_key_15`.
    - The codepoints come from the game's own text (`Mals/*/Key.msbt` labels):
      - E0E0-E0E9: A, B, X, Y, L, R, ZL, ZR, SL, SR;
      - E0EF / E0F0: Plus / Minus;
      - E0AB-E0AE, E0EA: the D-pad;
      - E100 / E101: the sticks;
      - E090-E097: the directions;
      - also the older E0A0-E0A9 set and E145/E146 (seen in layouts only).
    - The title is "Press <10.14:08cd> + <10.14:09cd> to Start": L + R.
    - `tools/fonts/make_button_font.py` draws those glyphs (our own vector shapes; only the letters inside come from
      the host font). It writes `private/wonder/fonts/shared_5.ttf` (git-ignored: it embeds the host font), which
      `hle_pl.cpp` already loads first.
    - Check: the title reads "Press [L] + [R] to Start", and menus show "(B) Back (A) Select".
    - To build it: `private/venv/bin/python tools/fonts/make_button_font.py`.

66. **Windowed stretches at 35-40 fps (issue #51): the GPU goes past the frame, headless does not.**
    - In the play session log, the slow stretches in the course show:
      - replay 8-10 ms;
      - `fence wait` 13-15 ms: all of it the wait for the first part of the split submit, with no throttle waits;
      - 60 of 60 submits split.
      So the GPU takes ~25 ms per frame there.
    - The same course headless: replay ~6 ms, fence ~4 ms, 60 fps. Audio is not the difference: the renderer runs
      headless too, only the output is dropped.
    - A windowed test run cannot measure it unattended. When the window is covered, SDL reports it hidden and
      `gpu_present` stops (`window_minimized`).
    - Next, with the window visible: `NX_GPU_PROFILE=1` in the slow stretch; `NX_PIPELINED=3`; and a check of whether
      the compositor holds the 3 swapchain images (IMMEDIATE, 2560x1440).

67. **World map: the player was a flat silhouette (issue #50). A pool swizzle leaked from another slot.**
    - The map draws the player into a target of its own. It then composites the player over the scene
      (fs `0bebd775`) from three textures:
      - A (RG16F): the player's distance and silhouette strength, written by fs `0030c911`;
      - C (R32F): the scene depth made linear by fs `0aa56d1b`;
      - P: the player colour.
    - Where `A.x >= C.x + 1e-5`, the player is behind the scenery: dark overlay. Otherwise P is shown.
    - Measured in one frame (new `NX_SNAP_RAW`): A.x and C match within 4e-6 at every player pixel. The shader
      still took the "behind" branch.
    - Cause: the game also registers A in another pool slot with a G G G G view. `TexturePoolRegister` stored that
      swizzle on the Texture (`pool_swizzle`). `sampled_view` applied it to every slot whose own view has no swizzle,
      the composite's included. A.x then read the strength (1.0), not the distance.
    - Fix (`nvn_pipeline.cpp` `sampled_view`): a slot's own view decides. `pool_swizzle` stands in only when no slot
      view is known. `NX_POOL_SWIZZLE_ANY=1`: as before.
    - Ruled out on the way, each kept as an opt-in switch:
      - depth invariance of GREATER (`NX_GREATER_SLACK=1`);
      - clip-space z convention: `NX_CLIP_Z_PATCH=1` maps z in the vertex shaders (`nvn_spirv_clip.cpp`) instead of
        using VK_EXT_depth_clip_control. It gives the same image, so MoltenVK's extension works.
    - Kept on: a pass that samples its own depth image while neither testing nor writing depth now runs without it
      attached (undefined on Metal). `NX_KEEP_SAMPLED_DEPTH=1`: as before.
    - Debug tools added:
      - `NX_SNAP_RAW=1`: 4-byte formats snapshotted as stored, to `.raw`;
      - `NX_SNAP_FS_MAX`;
      - `NX_TRACE_FS_HASH` takes a list;
      - `NX_TRACE_VERTS=1`: the first vertices of stream 0.
    - In the draw trace, the indented state lines come *before* the `draw N:` line they belong to.
    - Check: the map shows the chosen character. The course is still 60 fps, and menus and "Here we go!" are
      unchanged.

## Android bring-up (2026-09-30, evening; branch `feat/android-wonder`)

68. **First run on a phone: the alias region collided with Android's zygote.**
    - Device: Galaxy S23 (SM-S911B, Snapdragon 8 Gen 2, Adreno 740, 8 GB, Android 16).
    - The app died at start with `FATAL: cannot reserve alias at 5800000000 (+1000000000): the range is in use`.
    - `/proc/<pid>/maps` showed `app_process64` (the zygote's executable, position independent) at
      0x5f_0972_b000. Android places it at random, from about 0x55_5555_0000 to 0x65_5555_0000, at every boot. So
      the old layout worked for Hades only by luck of the boot.
    - Fix: on Android the alias region (64 GiB) sits at 0x20_0000_0000, below the modules. Android keeps to the low
      4 GiB there (ART heap). The guest learns the region from svcGetInfo; `memory.cpp` marks the gap between the
      alias and the stack region inaccessible.
    - The second failure was the data folder. Files pushed by `adb` into `Android/data/<pkg>` belong to the shell
      user, and the app cannot read them. The data now goes to `Internal storage/Super Mario Bros. Wonder`, with
      all-files access granted by `appops set --uid <pkg> MANAGE_EXTERNAL_STORAGE allow`
      (`tools/wonder_push_android.sh`).
    - Result: the game runs on the S23. Adreno 740, Vulkan 1.3, MAILBOX, 2340x1080 swapchain, 40-60 fps at the
      title, tens of thousands of draws with none failed, and still running after a minute. The screen stays black
      (#69).

69. **Black screen on the phone: the NVN present path was Mac-only.** The picture renders (no failed draw), and
    `gpu_present` blitted it to the swapchain. Three things are wrong there on Android:
    - The swapchain format was hard-coded to `B8G8R8A8_UNORM`. Android surfaces offer RGBA, not BGRA; the format
      now comes from `vkGetPhysicalDeviceSurfaceFormatsKHR`.
    - `compositeAlpha` was hard-coded to OPAQUE, which an Android surface may not offer (INHERIT then takes the
      image's alpha as transparency). It is now chosen from the capabilities, and the picture's alpha is forced to 1.
    - The blit does not rotate. With the surface's `currentTransform` at 90° the compositor has to turn the image.
    - Fix: `nvn_present.cpp`, a render pass that samples the window texture. It rotates by `preTransform` (the
      swapchain made in the panel's own orientation), fits the picture with bars (16:9 on 19.5:9) and writes alpha
      1. It is on by default on Android; `NX_PRESENT_PASS=1` enables it elsewhere, `=0` restores the blit.
    - A new Android window (home, lock screen) now re-makes the surface and the swapchain (`window_generation`).
    - Check without a phone: `NX_PRESENT_TEST=1080x2340:90` renders the pass into an offscreen "display" in headless
      runs and dumps it. The title comes out turned 90° with bars, the same mapping as the Hades pass that is proven
      on the S23.
    - The on-device check is pending (the phone was disconnected).

70. **Adreno reads BC natively.** On the S23 no texture image failed to be made, and the game uploads BC1/BC4/BC5/
    BC7 from its first frames. Decision D1 (BC → ASTC on the Mac, ANDROID-PORT.md) is therefore for Mali phones
    only, not a blocker for Adreno.

71. **Touch controls for Wonder, drawn (A4).**
    - `touch_layout.cpp` holds a fixed layout: round buttons with A big at the bottom right, B, Y, X around it, and
      ZR, R, L, +, - along the top. It is hit-tested by `platform_android_input.cpp`.
    - The left half is a stick that appears where the thumb lands. Pushed past 85% it also holds Y (dash), so
      running and jumping need one thumb each.
    - The present pass draws the circles over the picture: translucent, brighter while held, with the stick under
      the thumb. They hide while a controller is in use.
    - Hades keeps its own zones.
    - `NX_OVERLAY_DEMO=1` draws the layout on the Mac for the self-test
      (`docs/media/wonder/41-android-touch-overlay-selftest.png`).
    - Also checked on the Mac: menus take short taps. Seven 0.15 s A presses go from the title to course 1-1. The
      "A held ~1 s" of 2026-09-29 is gone.

72. **The Android emulator is no use for rendering on the M1.** The emulator 34.2.16 selects SwiftShader (a CPU
    Vulkan) whatever the settings: `-no-window` forces it, and with a window, `ANDROID_EMU_VK_ICD` and the bundled
    MoltenVK ICD were ignored. Wonder boots on it, but no frame came in 11 minutes: every pipeline compiles on the
    CPU. It is kept for install and boot checks only (AVD `WonderTest`: 5 GB RAM, 16 GB disk). Rendering checks use
    `NX_PRESENT_TEST` on the Mac and the phone.
