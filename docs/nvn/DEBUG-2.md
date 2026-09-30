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
