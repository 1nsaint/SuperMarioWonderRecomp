// Super Mario Bros. Wonder (010015100B514000, 1.0.0): what the shared runtime needs to know about the game.
// The game draws through NVN (docs/NVN.md); what we know about it is in docs/nvn/WONDER.md.
#include <nx/game.h>
#include <nx/runtime.h>

namespace nx {

const game::Info game::kInfo = {"wonder", "Super Mario Bros. Wonder", 0x010015100B514000ull,
                                "io.github.belmantegu.wonderrecomp", "private/wonder",
                                0x1000000 /* main.npdm: 16 MiB system resource */};

void game::after_init() {}
void game::renderer_start() {}
void game::first_frame() {}

}  // namespace nx
