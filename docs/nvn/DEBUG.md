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
