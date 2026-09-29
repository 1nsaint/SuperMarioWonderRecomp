// Scripted input for unattended runs: NX_INPUT_SCRIPT="<seconds>:<button>:<hold seconds>;..." presses buttons at
// fixed times after the first pad read, e.g. "70:A:0.2;74:A:0.2;80:RIGHT:5" (buttons: A B X Y L R ZL ZR PLUS MINUS
// LEFT RIGHT UP DOWN; the directions also push the left stick). Added on top of the real pad and keyboard.
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "platform.h"

namespace nx::plat {

namespace {

struct Press {
    double at = 0, hold = 0;
    uint64_t bits = 0;
    int lx = 0, ly = 0;
};

std::vector<Press> parse(const char* s) {
    struct Name {
        const char* name;
        uint64_t bits;
        int lx, ly;
    };
    static const Name names[] = {
        {"A", kA, 0, 0},          {"B", kB, 0, 0},          {"X", kX, 0, 0},          {"Y", kY, 0, 0},
        {"L", kL, 0, 0},          {"R", kR, 0, 0},          {"ZL", kZL, 0, 0},        {"ZR", kZR, 0, 0},
        {"PLUS", kPlus, 0, 0},    {"MINUS", kMinus, 0, 0},  {"LEFT", kLeft, -kStickMax, 0},
        {"RIGHT", kRight, kStickMax, 0}, {"UP", kUp, 0, kStickMax}, {"DOWN", kDown, 0, -kStickMax},
    };
    std::vector<Press> v;
    std::string all = s;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find(';', pos);
        if (end == std::string::npos) end = all.size();
        std::string item = all.substr(pos, end - pos);
        pos = end + 1;
        size_t a = item.find(':'), b = item.find(':', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        Press p;
        p.at = std::atof(item.substr(0, a).c_str());
        p.hold = std::atof(item.substr(b + 1).c_str());
        std::string button = item.substr(a + 1, b - a - 1);
        for (const Name& n : names)
            if (button == n.name) p.bits = n.bits, p.lx = n.lx, p.ly = n.ly;
        if (p.bits) v.push_back(p);
    }
    return v;
}

}  // namespace

void input_script(PadState& p) {
    static const char* env = std::getenv("NX_INPUT_SCRIPT");
    if (!env) return;
    static const std::vector<Press> script = parse(env);
    static const uint64_t start = monotonic_ns();
    double t = double(monotonic_ns() - start) / 1e9;
    for (const Press& s : script)
        if (t >= s.at && t < s.at + s.hold) {
            p.buttons |= s.bits;
            if (s.lx) p.lx = s.lx;
            if (s.ly) p.ly = s.ly;
        }
}

}  // namespace nx::plat
