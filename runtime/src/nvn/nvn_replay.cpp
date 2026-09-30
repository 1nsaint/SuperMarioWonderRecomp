// The replay of recorded NVN command lists into a Vulkan command buffer (split from nvn_vk.cpp): render targets,
// clears, calls into other lists, copies; draws and state go to nvn_draw.cpp.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nvn_vk.h"

namespace nx::nvn {

using namespace nx::vk;

void replay_list(VkCommandBuffer cb, Targets& st, const CmdList& list, int depth) {
    if (skin_lists_on()) skin_list_replayed(list, depth);
    for (const Cmd& c : list.cmds) {
        switch (c.op) {
            case Cmd::SetRenderTargets: {  // the pass ends only when a draw or clear needs other targets
                static const bool always = std::getenv("NX_TARGETS_BREAK") != nullptr;  // bisecting: the old way
                if (always) draw_end_rendering(cb);
                st.color_count = c.n;
                for (uint32_t k = 0; k < 8; k++) st.colors[k] = k < c.n ? c.a[k] : 0;
                st.depth = c.a[8];
                st.views = {};
                if (c.blob < list.blobs.size() && list.blobs[c.blob].size() == sizeof(TargetViews))
                    std::memcpy(&st.views, list.blobs[c.blob].data(), sizeof st.views);
                break;
            }
            case Cmd::ClearColor:
            case Cmd::ClearDepthStencil: draw_clear(cb, st, c); break;
            case Cmd::CallList:
                if (depth < 8) {
                    if (auto l = command_list(c.a[0]))
                        replay_list(cb, st, *l, depth + 1);
                    else {
                        static int missing = 0;
                        if (++missing <= 5) log("nvn: called handle %llx is unknown", (unsigned long long)c.a[0]);
                    }
                }
                break;
            case Cmd::CopyTextureToTexture:
                draw_end_rendering(cb);
                if (gpu_profile_on()) gpu_profile_mark(cb, "texture copies");
                replay_copy(cb, c, list);
                if (gpu_profile_on()) gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
                break;
            default: draw_command(cb, c, list, st); break;
        }
    }
}

}  // namespace nx::nvn
