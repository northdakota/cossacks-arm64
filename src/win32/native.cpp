// Functions of the engine (dmcr.exe, GOG build b4eea674…) rewritten for the host, chosen by
// profiling (docs/known-issues.md). Each one replaces the original only if the original's bytes are
// the expected ones, and must give the same result: checked by comparing the frame log of a
// deterministic run with and without them (cossacks-run --no-native).

#include <cstdio>
#include <cstring>
#include <vector>

#include "win32/process.h"

namespace zeus::win32 {
namespace {

// Variables of the engine (exported names in brackets).
constexpr uint32_t kScreenPtr = 0x6955fc;     // ?ScreenPtr@@3PAXA: the drawing buffer
constexpr uint32_t kRealScreenPtr = 0x695600; // ?RealScreenPtr@@3PAXA: the primary surface
constexpr uint32_t kScrSizeX = 0x694530;      // ?SCRSizeX@@3HA: pitch of the drawing buffer
constexpr uint32_t kRScrSizeX = 0x694538;     // ?RSCRSizeX@@3HA: pitch of the primary surface
constexpr uint32_t kRealLx = 0x694e08, kRealLy = 0x694e04;  // ?RealLx / ?RealLy: the screen's size
constexpr uint32_t kScreenReady = 0x694544;   // non-zero once the screen exists

bool matches(Cpu& cpu, uint32_t address, const std::vector<uint8_t>& bytes) {
    return std::memcmp(cpu.host(address), bytes.data(), bytes.size()) == 0;
}

// 0x470794, called by DialogsSystem::RefreshView and others: copies the rectangle (x, y, w, h) of
// the drawing buffer to the same place of the primary surface, clipped to the screen; __cdecl.
// In the emulator each iteration of its `rep movsd` is a block of its own: 6.6 % of a battle.
void install_copy_to_screen(Process& p) {
    Cpu& cpu = p.cpu();
    constexpr uint32_t kAddress = 0x470794;
    if (!matches(cpu, kAddress, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x28, 0x53, 0x56, 0x57, 0x83, 0x3D, 0x44, 0x45, 0x69,
                                 0x00, 0x00, 0x75, 0x05, 0xE9, 0x0B, 0x01, 0x00, 0x00}) ||
        !matches(cpu, 0x47089b, {0x8B, 0x4D, 0xD8, 0x67, 0xE3, 0x02, 0xF3, 0xA5, 0x8B, 0x4D, 0xE4, 0x67, 0xE3, 0x02, 0xF3, 0xA4})) {
        std::fprintf(stderr, "[hôte] copie vers l'écran (%#x) : octets inattendus, laissée à l'émulateur\n", kAddress);
        return;
    }
    cpu.replace_function(kAddress, 0, [](Cpu& c) -> uint32_t {
        if (!c.read_u32(kScreenReady)) return 0;
        int32_t x = static_cast<int32_t>(c.arg(0)), y = static_cast<int32_t>(c.arg(1));
        int32_t w = static_cast<int32_t>(c.arg(2)), h = static_cast<int32_t>(c.arg(3));
        if (x < 0) {
            w += x;
            x = 0;
        }
        if (y < 0) {
            h += y;
            y = 0;
        }
        const int32_t lx = static_cast<int32_t>(c.read_u32(kRealLx)), ly = static_cast<int32_t>(c.read_u32(kRealLy));
        if (x + w > lx) w = lx - x;
        if (y + h > ly) h = ly - y;
        if (w < 0 || h < 0) return 0;
        const uint32_t src_pitch = c.read_u32(kScrSizeX), dst_pitch = c.read_u32(kRScrSizeX);
        uint32_t src = c.read_u32(kScreenPtr) + static_cast<uint32_t>(x) + static_cast<uint32_t>(y) * src_pitch;
        uint32_t dst = c.read_u32(kRealScreenPtr) + static_cast<uint32_t>(x) + static_cast<uint32_t>(y) * dst_pitch;
        for (int32_t row = 0; row < h; ++row, src += src_pitch, dst += dst_pitch)
            std::memmove(c.host(dst), c.host(src), static_cast<size_t>(w));
        return 0;
    });
}

}  // namespace

void install_native_functions(Process& p) {
    install_copy_to_screen(p);
}

}  // namespace zeus::win32
