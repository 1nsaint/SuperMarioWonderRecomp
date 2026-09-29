# Debugging the NVN → Vulkan path

The tools used to take Wonder from "frames presented but black" to the title screen, and what each bug looked like.
Everything below is switched on with environment variables on the normal build: no rebuild needed.

## Environment variables

| Variable | What it does | Code |
|---|---|---|
| `NX_LOG=<file>` | runtime log (use `private/wonder/nx.log`) | runtime |
| `NX_DUMP_FRAMES=N` | every Nth presented window texture → `private/wonder/frames/NNNNN.ppm` | nvn_vk.cpp |
| `NX_DUMP_TEXTURES=<hex>,<hex>` | at the same frames, those `NVNtexture*` are read back too → `NNNNN_<texture>.ppm` (blitted to RGBA8) | nvn_debug.cpp |
| `NX_DUMP_RAW=1` | with the above: texels as stored → `.raw` (u32 width, height, VkFormat, then 4-byte texels) | nvn_debug.cpp |
| `NX_DUMP_ON_TARGET=<hex>` | dump `NX_DUMP_TEXTURES` right after the submit that draws into that texture (every `NX_DUMP_FRAMES`-th such draw), before later frames overwrite them | nvn_draw.cpp, nvn_vk.cpp |
| `NX_TRACE_DRAWS=<first>` | logs 80 replayed draws from draw number `<first>`: shaders, targets, textures (handle:texture, size, format, `rt` = GPU-written), fragment `c3` contents, blend/mask/logic op, viewport/scissor/cull, depth | nvn_draw.cpp |
| `NX_TRACE_TARGET=<hex>` | with `NX_TRACE_DRAWS`: only draws into that color target | nvn_draw.cpp |
| `NX_TRACE_TEXTURES=1` | every `nvnTextureInitialize`: target, format, size, levels, flags, storage size, block height, swizzle | nvn_objects.cpp |
| `NX_NVN_LISTS=1` | per ended command list (a window of 200): commands, draws, calls, the caller; per submit (5 frames): handles | nvn_cmd.cpp, nvn_queue.cpp |
| `NX_NVN_STATES=1` | decoded state objects and samplers as they are bound | nvn_state.cpp, nvn_sampler.cpp |
| `NX_DUMP_SHADER_MISSES=1` | writes shader blocks that have no SPIR-V yet (to translate offline) | nvn_shaders.cpp |
| `NX_NVN_REAL=1` | records every proc's real driver address in `nvn_trace.txt` | nvn_trace.cpp |
| `NX_TRACE_SKIN=1` | the first draws of every vertex shader with an integer vertex input (skinned models): input formats and streams, vertex uniform blocks, the first vertices' bone indices and the bone rows of c8 they select | nvn_draw_trace.cpp |
| `NX_TRACE_SKINLISTS=1` | follows command lists that bind skinned programs: recorded (handle, command buffer), called, replayed; dumps the first ones' commands (`P<vs>` binds, `*` skinned, `D` draws, `C<handle>(cmds/draws/programs)` calls, `GONE` for a handle already dropped) | nvn_cmd.cpp |
| `NX_INT_ATTRIBS=0` | turns off the integer vertex input patch (on by default since #26) | nvn_pipeline.cpp |
| `NX_ZERO_DRAWS=1` | every draw inside an occlusion query; every 300 frames lists the `vs fs` shader pairs whose draws never passed a single sample (what the scene draws but we lose) | nvn_debug.cpp |
| `NX_GPU_PROFILE=1` | GPU time per render pass (timestamps at pass starts), top 10 labels every 60 frames: `ms/frame WxH L<level> fs <hash>` | nvn_debug.cpp |
| `NX_SKIP_FS=<hash>,<hash>` | drops draws with those fragment shaders (measure what they cost, or hide a broken one) | nvn_draw.cpp |
| `NX_INPUT_SCRIPT="<s>:<button>:<hold s>;..."` | presses buttons at times after the first pad read (A B X Y L R ZL ZR PLUS MINUS LEFT RIGHT UP DOWN; directions also push the stick) | input_script.cpp |
| `NX_INPUT="<frame>:<button>,..."` | older, per presented frame, 8-frame presses | hle_hid.cpp |
| `NX_TRACE_FS=1` | every file the game opens or looks for; also counts reads of streamed audio (`.bwav`) | hle_fs.cpp |
| `NX_HEADLESS=1` | no visible window, nothing presented; frames still render and dump (unattended runs) | platform_mac.cpp, nvn_vk.cpp |
| `NX_MEM_LIMIT_GB=<n>` | the memory guard ends the process past this footprint (default 8); `memory:` lines every 30 s | memguard_mac.cpp |
| `NX_THREAD_DUMP_AT=<s>` | once, every guest thread's call chain (frame-pointer walk) | kernel.cpp |
| `NX_TRACE_FS_HASH=<hash>` | with `NX_TRACE_DRAWS`: only draws with that fragment shader | nvn_draw.cpp |
| `NX_TRACE_COUNT=<n>` | how many draws `NX_TRACE_DRAWS` logs (default 80) | nvn_draw.cpp |
| `NX_TRACE_GLYPHS=1` | with the draw trace: the first glyph records of text draws, the atlas memory at their UVs, the sampler | nvn_draw.cpp |
| `NX_SWAP_FS=<from>:<to>` | draws with fragment shader `<from>` use `<to>` (already loaded): shows what a draw covers | nvn_draw.cpp |
| `NX_SNAP_DRAWS=<n>,<n>` | mid-frame snapshots: colour target 0 right after those draws → `frames/00000_<draw hex>.ppm` (use `seq -f %.0f`: macOS `seq` prints big numbers in scientific notation) | nvn_debug.cpp |
| `NX_SNAP_FS=<hash>,...[@<draw>]` | snapshots after draws with those fragment shaders (8 at most), from draw `<draw>` on | nvn_draw.cpp |
| `NX_SNAP_FS_BEFORE=<hash>[@<draw>]` | snapshot of colour target 0 *before* such a draw (what a feedback pass like a tonemap reads) | nvn_draw.cpp |
| `NX_TRACE_VS_BLOCKS=1` | with the draw trace: the vertex stage's uniform blocks (camera block rows, c4..c6) | nvn_draw.cpp |
| `NX_TRACE_UPLOADS=1` | uploads of big textures, with how much of their memory is non-zero | nvn_pipeline.cpp |

Always on: every 60 presents `nvn/vk: per frame over 60: submits, replay ms (host), GPU wait ms, present wait ms,
upload MB`.

Clear colors are logged under `NX_TRACE_DRAWS` too (`clear <texture> (WxH) to r g b a`, each pair once).

Reading dumps: `.ppm` opens in Preview. `.raw` of `VK_FORMAT_B10G11R11_UFLOAT_PACK32` (122): bits 0-10 R and 11-21 G
(5-bit exponent, 6-bit mantissa), 22-31 B (5/5); exponent 31 is Inf/NaN. A 20-line numpy script decodes it.

Shaders: `spirv-cross private/wonder/spirv/<hash>.spv --version 450` shows the translated GLSL; the hash is what
`NX_TRACE_DRAWS` prints. `private/wonder/spirv/results.tsv` has each shader's resources.

## Bugs found this way (2026-09-28), in order

1. **Only the composite was ever submitted** (24k draws recorded, ~1 per frame replayed). `NX_NVN_LISTS` showed the
   scene lists (625 commands, 29 draws) ended but never submitted. The engine reads
   `nvnCommandBufferGetCommandMemoryUsed` right after `EndRecording` (main+6fef0) and drops a list that "used
   nothing"; ours restarted at 0 with each recording. Fix: usage accumulates across recordings, reset only when memory
   is added (nvn_cmd.cpp).
2. **Solid magenta window** (screenshot `docs/media/wonder/01-magenta-culled-composite.png`). The only draw into the
   window is a plain copy shader; forcing a black texture still gave magenta, so the draw wrote nothing: it was
   **culled**. NVN's front face is in GL terms; with the upper-left window origin Wonder sets, the winding is reversed
   in Vulkan's framebuffer. Fix: invert `NVNfrontFace` (nvn_pipeline.cpp). The magenta was the window texture's
   guest memory.
3. **Title art scrambled in 32-row bands** (`02-title-wrong-block-height.png`). `nvnTextureBuilderGetPackagedTextureLayout`
   returns 1 for every packaged texture, so it is not the BNTX block height. Fix: packaged textures use the fit from
   the BNTX survey (docs/nvn/TILING.md): smallest power of two >= rows * 3 / 32, at most 16 GOBs
   (`03-title-tiling-fixed.png`).
4. **Channel masks read with overlapping outputs**: `nvnChannelMaskStateGetChannelMask` writes four `NVNboolean`
   words; the output slots were 1 byte apart. Fix: 4-byte slots.
5. **Text quads covering the screen / black boxes**: textures are read through swizzles (a font atlas in R8 is
   registered with alpha = R; BC4 masks as 1,1,1,R). Vulkan returns alpha 1 without them, so the SDF text shader never
   discarded. Fix: the builder's swizzle (`nvnTextureBuilderGetSwizzle`) and the registering view's
   (`nvnTextureViewGetSwizzle` in `nvnTexturePoolRegisterTexture`) go into the sampled `VkImageView`.
6. **Everything green** (`04-title-swizzle-nan-green.png`): not a mask, not the target format (RGBA16F instead of
   R11G11B10F changed nothing), not Metal fast math. `NX_DUMP_RAW` showed R and B are **NaN/Inf in the scene**, G
   valid; RGBA8 turns NaN into 0. The NaN comes from the bloom chain, whose passes render into one mip level of a
   texture while sampling another: NVN selects the level with the `NVNtextureView` given to `SetRenderTargets` and
   to the texture pool, which we ignored (everything went to level 0, the sampled levels were uninitialised). Fix:
   `nvnTextureViewGetLevels/GetLayers/GetSwizzle` read for render targets (recorded as a `TargetViews` blob) and pool
   entries; attachments are single-level/layer views with that mip's extent; images the GPU renders stay in
   `VK_IMAGE_LAYOUT_GENERAL` (sampled and rendered at once). 3D images are created 2D-array compatible, since a
   colour-grading LUT is rendered slice by slice and Metal refuses a 2D view of a plain 3D texture (it aborted with
   `_mtlValidateArgumentsForTextureViewOnDevice`). Result: `05-title-screen.png`, correct colours.
7. **2-3 fps, the GPU busy ~550 ms per frame** on a title screen. Sampling the process: every thread waited on the
   submit thread, which waited on MoltenVK's fence. `NX_GPU_PROFILE` showed ~10 ms per render pass whatever its size
   (a 32x18 pass: 44 ms); dropping the draw calls only (everything else kept) brought the GPU to 9 ms, while
   `rasterizerDiscardEnable` left it at 525 ms, so the cost was before the fragment stage. Cause: **8-bit index buffers**. Metal has none and MoltenVK converts them on the GPU at every draw.
   Widening them to 16 bits on the host: GPU ~10 ms per frame, the title at the game's 30 fps.
8. **After the title, a crash: "call into the middle of a function" main+22e810.** Not a function pointer: a
   `switch` jump table (`adr x10, anchor; ldrh w11, [x9, x8, lsl #1]; add x10, x10, x11, lsl #2; br x10`) whose
   index is a byte with no bounds check. The recompiler's `_bound` walked back to an older `cmp w8, #2` / `b.ne`
   guarding another value and capped the table at 3 entries; case 3 was missing. Fix (recomp/analysis.py): the
   search stops when the index register is redefined, and only a `cmp` followed by `b.hi`/`b.hs` is a bound. The
   recompiler now writes only changed files, so the rebuild after regenerating took 25 s instead of ~15 min.
9. **Metal abort on a cube view with 1 layer**: a shader declaring `samplerCube` given a 2D texture. Views Metal
   cannot make (cube of a non-cube image, 3D of 2D, 1D of 2D) now read the dummy texture.
10. **Input**: the pad reached the game (`hid: first buttons 1`) and A works: after it the game loads the file
    select (`NX_TRACE_FS`). `GetNpadStates` now keeps a 16-sample history like hid (Wonder ignores the count and reads
    all 16 entries), and `BindNpadStyleSetUpdateEvent` signals its event once (the controller is connected).
    Screenshot: `06-file-select.png` (text still missing).
11. **The opening was garbage after compute** (chevron patterns). Compute shaders search the buffer descriptor table
    in `c[0]` to find which buffer an address falls in; it was bound as zeros. Also, storage buffers are copies in the
    upload buffer and Metal does not bound-check, so a shader writing past a small or missing one overwrote the next
    data. Fix: `c[0]` filled with the bound descriptors, storage padded to 64 KB, compute writes copied back to guest
    memory after the submit.
12. **"upload buffer full"**: a busy submit needed 67 MB of the 64 MB ring and wrapped over its own data. 256 MB now.
13. **The opening never advanced: streamed music was never consumed** (`NX_TRACE_FS` showed each `.bwav` read 2-3
    times, then never). `ExecuteAudioRendererRendering` returned 0x40299 in 0.5 us: it only renders when the render
    system's execution mode and rendering device are manual/CPU (`GetExecutionMode`, `GetRenderingDevice`, render
    system +0x2d0/+0x2d4), and the game opens the renderer in the automatic mode. Switched after opening: ~250 us a
    frame, the streams play.
14. **The Mac froze: the process reached 85 GB.** GPU images of destroyed textures were never freed (no
    `vkDestroyImage` anywhere) and the opening creates textures continuously; with unified memory the machine ran out
    of RAM and swapped until it hung. Fix: a texture's image goes to a graveyard freed at the next submit/present,
    with its cached views. The same run stays at ~4.5 GB. The memory guard (`NX_MEM_LIMIT_GB`) now stops any leak
    before it matters.
15. **Dialog text invisible** (open). With `NX_SWAP_FS` the glyph quads land correctly; without the view swizzle
    they are solid boxes, so the pipeline works and the sampled coverage is 0. The glyph atlas (GPU copy and guest
    memory) holds the glyphs at the recorded UVs (`NX_TRACE_GLYPHS`). The translated vertex shader picks each corner's
    texture coordinate through a table in `c[1]` (values 0x378, 0x390, 0x3a0, 0x450, 0x430, 0x440) and compares the
    looked-up value with a single constant (888, 1072) the offline translator resolved; the other corners fall back to
    a position value, so they sample empty atlas. Needs the translator to express that indexed load.
16. **Another "call into the middle of a function"** (main+28b3c0, after the opening dialogs): a jump table whose base
    register (x24) is set by an `adrp/add` on another control-flow path, 0x5000 bytes before; the linear backward
    search found an unrelated `movk`. The recompiler now tries every constant the function puts in the base register
    when the bound is known, and keeps the one whose entries all land on instructions of the function.
17. **The first course showed a mirrored sky; the whole scene was upside down** (`NX_SNAP_DRAWS` mid-frame: the course
    was drawn flipped, sky gradient included; the tonemap then produced half a mirrored sky). Root cause, older than
    all of this: NVN with the upper-left window origin maps NDC +Y to the top of the render target, like D3D; Vulkan
    maps it to the bottom. Bug #2 had been "fixed" by inverting the front face, which made culling right but left
    every render target upside down; some scenes (the title, the opening) came out right because their post chain
    flipped twice, the course did not. A global experiment (`NX_TMP_FLIPY`: negative-height viewports, front face as
    is) made the title, the opening and the course all correct. Fix: every draw uses a negative-height viewport and
    NVN's front face unchanged.
18. **Dialog text invisible (#15) — and 792 other shaders silently wrong.** Found by a parallel investigation of the
    offline translator: the glyph vertex shader's per-corner texture coordinates come from a Maxwell `BRX` jump table
    (0x378/0x390/0x3a0 are jump targets, not data); Ryujinx decodes it into `if / else if / else`, and its SPIR-V back
    end drops `else if` blocks (they become unreachable), so only the first case survived. Fixed in the harness with a
    local patch to `SpirvGenerator.Generate` (docs/nvn/TRANSLATOR.md, "Silent miscompile: `else if` chains"); 793
    blocks regenerated, all validated and compiled to Metal.
19. **EQUAL depth after a pre-pass** — relaxed to LESS_OR_EQUAL when the pass does not write depth (Metal compiles the
    pre-pass and colour-pass vertex shaders separately; bit-identical depth is not guaranteed). Kept as a safety net;
    it was not the cause of #17.
21. **The picture froze at the first talking-flower balloon of the prologue course; the sound played on.** (#20 is
    the integer vertex inputs.) The game was not stuck: guest threads kept running, frames kept being presented and
    scripted input kept driving the logic (walking right, it even finished the prologue course and reached the world
    map). The GPU was gone: `vkQueueSubmit failed (-4)` on every submit from then on, and with
    `MVK_CONFIG_LOG_LEVEL=1` MoltenVK said why: `Caused GPU Hang Error (kIOGPUCommandBufferCallbackErrorHang)`, a
    command buffer that ran too long. Found in three steps. (1) Not the 793 regenerated shaders: the same hang with
    the pre-fix `.spv.orig` files (a spirv folder of symlinks under `NX_DATA_ROOT`). (2) A bisection over the 2,330
    shaders that contain a loop, leaving half of them out of that folder per run (a missing shader skips its draws),
    12 runs: fragment shader `b2f3dc2ff54d1ba4` (uber). (3) Its loop walks a tiled light list: the tile is
    `floor(gl_FragCoord * c[6][62].zw * (16, 9))`, the count is `c[8]` at byte 8196 + 256 * tile, and the loop runs
    that many times. A local trace of the bound banks: `c[8]` bound with 0xb000 bytes (144 tiles) and `c[6][62].zw`
    = 1/384, 1/216, but the same materials also draw into 1920x1080 targets, where the tile reaches ~800 and the count
    is read up to 213 KB into the bank. On Maxwell a constant read past the bound size returns 0 (no lights). Here the
    bank was the bound bytes zero-padded to 4 KB in the upload buffer, the translator declares every bank as
    vec4[4096] and Metal checks nothing: the count came from whatever followed, up to 4 billion iterations. 2,071 of
    the loop shaders read the same light list; which draw hung first depended on what lay in the upload buffer.
    Fix (`clamp_uniform_indices`, nvn_spirv_patch.cpp): when a shader module is created, every uniform access chain
    whose array index is computed gets `UMin(index, 4095)` (a negative index clamps too), and the set 0 bindings read
    that way are bound zero-padded to the full 64 KB (`dynamic_uniform_bindings`, `build_sets`), so every read past
    the data returns 0 as on Maxwell. 5,159 of the 12,751 modules change; all pass `spirv-val`. With it, no GPU hang:
    390 s in the prologue course, then a run from that save through the world map into the first course (Course001)
    until the 5 GB memory guard stopped it. Another process that hangs the GPU makes ours fail as well
    (`kIOGPUCommandBufferCallbackErrorInnocentVictim`): one game run at a time.
22. **The Mac restarted (WindowServer watchdog) during a test run.** The run used a build without #21: a shader hung
    the GPU, WindowServer (which draws the whole desktop) waited in Metal/IOGPU, its watchdog fired twice and macOS
    restarted. Safeguards: a lost device (`VK_ERROR_DEVICE_LOST`) or a submit not finishing within 2 s ends the
    process at once (nvn_vk.cpp), so a hang cannot keep the GPU busy. Test runs must use a build with the current
    fixes, one game process at a time.
23. **Compute pipelines refused: "Threadgroup memory size (49152) exceeds the maximum threadgroup memory allowed
    (32768)"** (compute `0cd5c52ea70a3bb0`, world map and first course; its effect was missing). The translator sizes
    shared memory with `IGpuAccessor.QueryComputeSharedMemorySize()`. The harness did not answer it, so Ryujinx's
    default of 0xc000 applied, and all 21 of Wonder's 71 compute shaders that use shared memory declared `uint[12288]`
    (48 KB). The M1 allows 32 KB, so Metal refused every one of them. The real size is in the control section at
    **+0x754**, right after the work-group size. It is 384 to 2,312 bytes in Wonder, and it is 0 for exactly the 50
    compute shaders without shared memory. The SPIR-V agrees with it. In the 18 shaders with a 32x1x1 work group,
    every access is `shared[(tid * S + k) >> 2]` with a constant `k < S` and `32 * S` equal to the control size (for
    `0cd5c52ea70a3bb0`, S = 60 and the size is 1,920 bytes). The three 16x16 ones are tiles with a border: 800 bytes
    = 20 rows of 40 bytes, and 2,312 = 17 x 17 x 8. This is also the amount Maxwell allocates per work group. Fix
    (`resize_shared_memory`, nvn_spirv_patch.cpp): `ShaderControl::shared_size` reads +0x754, and when a module is
    created, the Workgroup array is redeclared with `ceil(size / 4)` elements. It only ever shrinks the array. The
    harness answers the query from +0x754 too (`StaticGpuAccessor.QueryComputeSharedMemorySize`), so a future
    re-translation declares the right size and the runtime pass does nothing. Offline check
    (`private/wonder/patchwork/test/run.sh`): all 21 modules pass `spirv-val`, and `spirv-cross --msl` gives
    `threadgroup spvUnsafeArray<uint, 480>` for `0cd5c52ea70a3bb0`, not 12288. Metal
    `makeComputePipelineState` refuses all 21 before the fix and accepts all 21 after it (1,920 B for
    `0cd5c52ea70a3bb0`).
24. **Graphics pipeline refused: "Fragment input(s) `user(locn6)` mismatching vertex shader output type(s) or not
    written by vertex shader"** (vs `c5f64e8c84fa824d` + fs `4a9740c442f9ba61`, effect_static). The vertex shader
    writes locations 0-5 and 7, and the fragment shader reads 0-7. Vulkan allows an input that nothing writes (its
    value is undefined), but Metal links the stages by name and refuses the pipeline. On Maxwell such an attribute
    reads as (0, 0, 0, 1): Ryujinx initializes every output the next stage reads to that (Translator.cs,
    `InitializeOutputComponent`). This shader multiplies its alpha by `in6.w`, so 0 would make the effect invisible.
    Fix (`unwritten_inputs` + `default_unwritten_inputs`, nvn_spirv_patch.cpp): at pipeline creation, the fragment
    input locations missing from the vertex shader's reflected outputs become Private variables that start as
    (0, 0, 0, 1). The patch takes out their Location decoration, retypes the access chains to Private pointers, and
    removes them from the entry point interface (SPIR-V 1.3). The fragment module cache is keyed by that mask as
    well. When a pipeline needs it, the log says `inputs 0x40 not written by the vertex stage read (0, 0, 0, 1)`.
    Offline check: `spirv-val` and `spirv-cross --msl` pass, and the MSL fragment keeps inputs `locn0-5,7` with
    `float4 _45 = float4(0.0, 0.0, 0.0, 1.0)`. Metal `makeRenderPipelineState` on the pair fails as above before the
    fix and links after it. As a worst case, all 7,662 fragment shaders with inputs were patched with every input
    marked unwritten: 0 refused, 0 `spirv-val` failures, and a sample of 510 passed through `spirv-cross --msl`.
25. **Huge flat pale-green wedges across the course** (and still no characters). They came with the work-in-progress
    SPIR-V patch that retypes vertex inputs to integers for integer attribute formats (`patch_int_inputs`): an A/B run
    with it off (`NX_NO_INT_ATTRIBS`, now inverted to `NX_INT_ATTRIBS=1` to turn it on) showed clean scenes (the Wonder
    Flower, the airship, the golden Wonder sky). Off by default until it is right; characters are still invisible
    either way, so integer inputs are not (or not the only) cause. **Update (#26):** the patch is right and is now on by default. The
    characters were missing for another reason (#26); with both fixes no wedge showed up in the course runs.
26. **No character in the course: Mario, Luigi, the Talking Flowers, Bowser... never drawn.** Two layers.
    - *Metal refused every skinned pipeline.* The bone indices (vertex location 6) have an integer format, and the
      translated shader declares every input as `vec4`: MoltenVK logged `SPIR-V to MSL conversion error: Vertex
      attribute type mismatch between host and shader` (on stderr only, in the `.out` files, not in `NX_LOG`) and the
      draws were dropped as `no pipeline` (83,680 in one play session). `patch_int_inputs` (#25) fixes it; it was off.
    - *The draw lists were gone.* With the patch on, most runs still drew no skinned model at all: no pipeline was
      even requested. `NX_TRACE_SKINLISTS` showed that the lists binding the skinned programs are recorded, called
      and replayed every frame, but right after each skinned bind they call per-mesh lists recorded when the course
      loaded (handles like 0x5833, ~60,000 lists earlier), and those were `GONE`: nvn_cmd.cpp kept the last 65,536
      handles and dropped older ones that had not been used yet. The characters' lists are first called only after
      the intro, so whether they survived depended on timing (runs differed: some logged the pipeline errors above,
      most never drew a character). `NX_ZERO_DRAWS` could not see any of this, since those draws never reached the GPU.
    - Fix: a handle now lives as long as its command memory, as on the GPU. Each list remembers the bytes it used
      (memory added by `AddCommandMemory` + our usage estimate), and a newer list recorded over those bytes drops it.
      The count limit is a safety net only (2^18), and it never drops a list that was not called yet. Memory stayed
      at 3.2 GB (peak 3.5) through the course. The integer input patch is on by default (`NX_INT_ATTRIBS=0` turns it
      off). Result: Mario, Luigi, Peach, Daisy, the Toads, Yoshi, Nabbit, Prince Florian, Bowser and Kamek all render
      (docs/media/wonder/19-21).

