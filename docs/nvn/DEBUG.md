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
   to the texture pool, which we ignore (everything goes to level 0, the sampled levels are uninitialised). Fix in
   progress: render-target and pool views (levels, layers).
