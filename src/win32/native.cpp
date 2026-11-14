// Functions of the engine (dmcr.exe, GOG build b4eea674…) rewritten for the host, chosen by
// profiling (docs/known-issues.md). Each one replaces the original only if the original's bytes are
// the expected ones, and must give the same result: checked by comparing the frame log of a
// deterministic run with and without them (cossacks-run --no-native).

#include <cstdio>
#include <cstdlib>
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

// COSSACKS_NATIVE_OFF=0x47bdce,0x47b491 leaves those functions to the emulator (to measure one
// replacement, or to find which one a difference comes from).
bool switched_off(uint32_t address) {
    const char* list = std::getenv("COSSACKS_NATIVE_OFF");
    if (!list) return false;
    for (const char* p = list; *p;) {
        char* end = nullptr;
        const unsigned long value = std::strtoul(p, &end, 16);
        if (end == p) {
            ++p;
            continue;
        }
        if (value == address) {
            std::fprintf(stderr, "[native] %#x left to the emulator (COSSACKS_NATIVE_OFF)\n", address);
            return true;
        }
        p = end;
    }
    return false;
}

// 0x470794, called by DialogsSystem::RefreshView and others: copies the rectangle (x, y, w, h) of
// the drawing buffer to the same place of the primary surface, clipped to the screen; __cdecl.
// In the emulator each iteration of its `rep movsd` is a block of its own: 6.6 % of a battle.
void install_copy_to_screen(Process& p) {
    Cpu& cpu = p.cpu();
    constexpr uint32_t kAddress = 0x470794;
    if (switched_off(kAddress)) return;
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

// Clipping window of the drawing buffer and its pitch.
constexpr uint32_t kWindX = 0x6955f4, kWindY = 0x6955f8, kWindX1 = 0x6955ec, kWindY1 = 0x6955f0;  // ?WindX… (inclusive)
constexpr uint32_t kScrWidth = 0x6955dc;      // ?ScrWidth@@3HA: pitch used by the sprite blitters
constexpr uint32_t kSpriteBias = 0x5e06fc;    // added to the pixel pointer (0 once the engine starts)

// One row of a GP sprite frame: a count byte, then segments (skip, length). A count with bit 7 set
// packs each segment in one byte (skip in the low nibble, length in the high one), bits 6 and 5
// adding 16 to every skip and every length of the row; its count is in the low 5 bits.
struct RowReader {
    const uint8_t* c;
    int count = 0, skip_high = 0, length_high = 0;
    bool packed = false;
    void start() {
        const uint8_t head = *c++;
        packed = (head & 0x80) != 0;
        if (packed) {
            count = head & 0x1f;
            skip_high = (head & 0x40) ? 0x10 : 0;
            length_high = (head & 0x20) ? 0x10 : 0;
        } else {
            count = head;
        }
    }
    void segment(int& skip, int& length) {
        if (packed) {
            const uint8_t b = *c++;
            skip = (b & 0xf) | skip_high;
            length = (b >> 4) | length_high;
        } else {
            skip = c[0];
            length = c[1];
            c += 2;
        }
    }
};

// Draws one GP sprite frame into the drawing buffer at (x, y), clipped to the window, the way the
// engine's blitters do it, their limits included: a frame clipped on the left is not clipped on the
// right (the buffer's margins take what passes), and rows are clipped by whole segments the way they
// are. The blitters differ only in what they do with each visible run of `length` pixels at `to`
// (the run's pixels in the frame data are at `from`): `run(to, from, length)`.
struct BlitCounts {
    uint64_t whole = 0, left = 0, right = 0, top = 0, rejected = 0;
};

template <typename Run>
void blit_frame(Cpu& c, int32_t x, int32_t y, uint32_t header, uint32_t src, BlitCounts& n, Run run) {
    x += static_cast<int16_t>(c.read_u16(header + 4));
    y += static_cast<int16_t>(c.read_u16(header + 6));
    const int32_t width = static_cast<int16_t>(c.read_u16(header + 8));
    int32_t height = static_cast<int16_t>(c.read_u16(header + 0x15));
    const int32_t wx = static_cast<int32_t>(c.read_u32(kWindX)), wy = static_cast<int32_t>(c.read_u32(kWindY));
    const int32_t wx1 = static_cast<int32_t>(c.read_u32(kWindX1)), wy1 = static_cast<int32_t>(c.read_u32(kWindY1));
    if (y + height <= wy || x + width <= wx || x > wx1 || y > wy1) {
        ++n.rejected;
        return;
    }
    RowReader row{c.host(header + 0x17)};
    if (y < wy) {  // rows above the window: their pixels are skipped
        ++n.top;
        const int32_t hidden = wy - y;
        height -= hidden;
        y += hidden;
        for (int32_t r = 0; r < hidden; ++r) {
            row.start();
            for (int s = 0; s < row.count; ++s) {
                int skip, length;
                row.segment(skip, length);
                src += static_cast<uint32_t>(length);
            }
        }
    }
    if (y + height > wy1) height = wy1 - y + 1;
    const int32_t right = x + width - 1;
    const uint32_t pitch = c.read_u32(kScrWidth);
    uint32_t line = c.read_u32(kScreenPtr) + static_cast<uint32_t>(x) + static_cast<uint32_t>(y) * pitch;
    const uint8_t* from = c.host(src);
    if (x >= wx && right <= wx1) {  // whole rows
        ++n.whole;
        for (int32_t r = 0; r < height; ++r, line += pitch) {
            uint8_t* to = c.host(line);
            row.start();
            for (int s = 0; s < row.count; ++s) {
                int skip, length;
                row.segment(skip, length);
                to += skip;
                run(to, from, length);
                to += length;
                from += length;
            }
        }
    } else if (x < wx) {  // the left part of each row is hidden
        ++n.left;
        for (int32_t r = 0; r < height; ++r, line += pitch) {
            uint8_t* to = c.host(line);
            int32_t hidden = wx - x;
            row.start();
            for (int s = 0; s < row.count; ++s) {
                int skip, length;
                row.segment(skip, length);
                to += skip;
                hidden -= skip;
                if (hidden > 0) {
                    if (hidden >= length) {
                        hidden -= length;
                        from += length;
                        to += length;
                        continue;
                    }
                    from += hidden;
                    to += hidden;
                    length -= hidden;
                    hidden = -1;
                }
                run(to, from, length);
                to += length;
                from += length;
            }
        }
    } else {  // the right part of each row is hidden
        ++n.right;
        for (int32_t r = 0; r < height; ++r, line += pitch) {
            uint8_t* to = c.host(line);
            int32_t room = wx1 - x + 1;
            row.start();
            for (int s = 0; s < row.count; ++s) {
                int skip, length;
                row.segment(skip, length);
                to += skip;
                room -= skip;
                if (room <= 0) {
                    from += length;
                    continue;
                }
                int rest = 0;
                if (room >= length) {
                    room -= length;
                } else {
                    rest = length - room;
                    length = room;
                    room = -1;
                }
                run(to, from, length);
                to += length;
                from += length + rest;
            }
        }
    }
}

// How often each path of the blitters ran (COSSACKS_NATIVE_STATS=1 prints them at exit): a check
// against the original only covers the paths that ran.
struct PathCounts {
    BlitCounts copy, remap, blend;
    static void print(const char* name, const BlitCounts& n) {
        std::fprintf(stderr, "[native] %s: whole %llu, left %llu, right %llu, top clipped %llu, outside %llu\n", name,
                     static_cast<unsigned long long>(n.whole), static_cast<unsigned long long>(n.left),
                     static_cast<unsigned long long>(n.right), static_cast<unsigned long long>(n.top),
                     static_cast<unsigned long long>(n.rejected));
    }
    ~PathCounts() {
        if (!std::getenv("COSSACKS_NATIVE_STATS")) return;
        print("sprite blitter", copy);
        print("remap blitter", remap);
        print("blend blitter", blend);
    }
};
PathCounts counts;

// 0x47b491: draws a GP sprite frame (ShowGP, ShowGPLayers…); __cdecl (x, y, frame header, pixels,
// unused). Copies the frame's pixels; the pixel pointer gets the bias at 0x5e06fc added.
void install_sprite_blitter(Process& p) {
    Cpu& cpu = p.cpu();
    constexpr uint32_t kAddress = 0x47b491;
    if (switched_off(kAddress)) return;
    if (!matches(cpu, kAddress, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x34, 0x53, 0x56, 0x57, 0x8B, 0x45, 0x10, 0x0F, 0xBF, 0x48, 0x04}) ||
        !matches(cpu, 0x47b50c, {0x8B, 0x45, 0x14, 0x03, 0x05, 0xFC, 0x06, 0x5E, 0x00}) ||
        !matches(cpu, 0x47b61d, {0x8A, 0x02, 0x03, 0xF8, 0x8A, 0x4A, 0x01, 0x83, 0xC2, 0x02}) ||
        !matches(cpu, 0x47b81d, {0xC7, 0x45, 0xCC, 0x00, 0x00, 0x00, 0x00})) {
        std::fprintf(stderr, "[hôte] dessin de sprite (%#x) : octets inattendus, laissé à l'émulateur\n", kAddress);
        return;
    }
    cpu.replace_function(kAddress, 0, [](Cpu& c) -> uint32_t {
        blit_frame(c, static_cast<int32_t>(c.arg(0)), static_cast<int32_t>(c.arg(1)), c.arg(2),
                   c.arg(3) + c.read_u32(kSpriteBias), counts.copy,
                   [](uint8_t* to, const uint8_t* from, int length) { std::memcpy(to, from, static_cast<size_t>(length)); });
        return 0;
    });
}

// 0x47bdce: the same blitter for frames that recolour what is already drawn instead of covering it
// (shadows and the like: ShowGP picks one of six tables by the kind of frame); __cdecl (x, y, frame
// header, pixels (0, unused), table of 256 colours). Every pixel under the frame's runs becomes
// table[pixel]; the frame's pixel data is not read.
void install_remap_blitter(Process& p) {
    Cpu& cpu = p.cpu();
    constexpr uint32_t kAddress = 0x47bdce;
    if (switched_off(kAddress)) return;
    if (!matches(cpu, kAddress, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x34, 0x53, 0x56, 0x57, 0x8B, 0x45, 0x10, 0x0F, 0xBF, 0x48, 0x04}) ||
        !matches(cpu, 0x47be49, {0x8B, 0x45, 0x14, 0x89, 0x45, 0xF0}) ||
        !matches(cpu, 0x47bf1b, {0x8B, 0x5D, 0x18}) ||
        !matches(cpu, 0x47c080, {0x33, 0xC0, 0x8A, 0x07, 0x8A, 0x04, 0x03, 0x88, 0x07, 0x47, 0xFE, 0xC9})) {
        std::fprintf(stderr, "[hôte] dessin par table (%#x) : octets inattendus, laissé à l'émulateur\n", kAddress);
        return;
    }
    cpu.replace_function(kAddress, 0, [](Cpu& c) -> uint32_t {
        const uint8_t* table = c.host(c.arg(4));
        blit_frame(c, static_cast<int32_t>(c.arg(0)), static_cast<int32_t>(c.arg(1)), c.arg(2), c.arg(3), counts.remap,
                   [table](uint8_t* to, const uint8_t*, int length) {
                       for (int i = 0; i < length; ++i) to[i] = table[to[i]];
                   });
        return 0;
    });
}

// 0x47da93: the same blitter for frames blended with what is already drawn (ShowGP and others,
// most often with the table at 0x69ba04); __cdecl (x, y, frame header, pixels, table of 256 × 256
// colours). Every pixel under the frame's runs becomes table[frame pixel × 256 + pixel]; no bias on
// the pixel pointer.
void install_blend_blitter(Process& p) {
    Cpu& cpu = p.cpu();
    constexpr uint32_t kAddress = 0x47da93;
    if (switched_off(kAddress)) return;
    if (!matches(cpu, kAddress, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x34, 0x53, 0x56, 0x57, 0x8B, 0x45, 0x10, 0x0F, 0xBF, 0x48, 0x04}) ||
        !matches(cpu, 0x47db0e, {0x8B, 0x45, 0x14, 0x89, 0x45, 0xF0}) ||
        !matches(cpu, 0x47dbe0, {0x8B, 0x5D, 0x18}) ||
        !matches(cpu, 0x47dc2a, {0x33, 0xC0, 0x8A, 0x07, 0x8A, 0x26, 0x8A, 0x04, 0x03, 0x46, 0xAA, 0xFE, 0xC9})) {
        std::fprintf(stderr, "[hôte] dessin mélangé (%#x) : octets inattendus, laissé à l'émulateur\n", kAddress);
        return;
    }
    cpu.replace_function(kAddress, 0, [](Cpu& c) -> uint32_t {
        const uint8_t* table = c.host(c.arg(4));
        blit_frame(c, static_cast<int32_t>(c.arg(0)), static_cast<int32_t>(c.arg(1)), c.arg(2), c.arg(3), counts.blend,
                   [table](uint8_t* to, const uint8_t* from, int length) {
                       for (int i = 0; i < length; ++i) to[i] = table[(from[i] << 8) | to[i]];
                   });
        return 0;
    });
}

}  // namespace

void install_native_functions(Process& p) {
    install_copy_to_screen(p);
    install_sprite_blitter(p);
    install_remap_blitter(p);
    install_blend_blitter(p);
}

}  // namespace zeus::win32
