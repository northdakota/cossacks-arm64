// DDRAW : affichage du programme émulé (also MDRAW.DLL, GSC's DirectDraw wrapper that Cossacks
// imports instead of DDRAW.DLL). Surfaces of 8 bits with a palette (Cossacks' mode), 16 bits
// 5-5-5 (Zeus) or 32 bits.
//
// DirectDraw expose des objets COM : le programme reçoit un pointeur vers un objet dont le
// premier mot désigne une table de pointeurs de méthodes. Chaque méthode est ici une fonction
// hôte (convention stdcall, l'objet en premier argument). Une méthode non écrite arrête le
// programme en la nommant. Les surfaces vivent dans la mémoire émulée ; la surface primaire
// est recopiée dans la fenêtre à chaque fin d'écriture.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

#include "win32/process.h"

namespace zeus::win32 {
namespace {

constexpr uint32_t DD_OK = 0;
constexpr uint32_t DDERR_UNSUPPORTED = 0x80004001, DDERR_NOTFOUND = 0x887600FF;

// Indicateurs de DDSURFACEDESC et capacités de surface.
constexpr uint32_t DDSD_CAPS = 0x1, DDSD_HEIGHT = 0x2, DDSD_WIDTH = 0x4, DDSD_PITCH = 0x8,
                   DDSD_BACKBUFFERCOUNT = 0x20, DDSD_LPSURFACE = 0x800, DDSD_PIXELFORMAT = 0x1000;
constexpr uint32_t DDSCAPS_BACKBUFFER = 0x4, DDSCAPS_PRIMARYSURFACE = 0x200;
constexpr uint32_t DDBLT_COLORFILL = 0x400, DDBLT_KEYSRC = 0x8000;
constexpr uint32_t DDBLTFAST_SRCCOLORKEY = 0x1;
constexpr uint32_t WM_MOVE = 0x0003, WM_SIZE = 0x0005, WM_DISPLAYCHANGE = 0x007E;

struct Surface {
    int width = 0, height = 0, pitch = 0, bpp = 16;
    uint32_t pixels = 0;      // adresse émulée des pixels
    uint32_t palette = 0;     // object of the attached IDirectDrawPalette, or 0
    bool primary = false;
    uint32_t back_buffer = 0; // objet de la surface arrière attachée, ou 0
    uint32_t color_key = 0;
    bool has_color_key = false;
    int refs = 1;
};

struct State {
    std::map<uint32_t, Surface> surfaces;  // adresse de l'objet -> surface
    std::map<uint32_t, std::array<uint32_t, 256>> palettes;  // object -> ARGB8888 colours
    std::map<uint32_t, bool> all_256;  // object -> created with DDPCAPS_ALLOW256
    uint32_t surface_vtable = 0, clipper_vtable = 0, palette_vtable = 0;
    int mode_width = 800, mode_height = 600, mode_bpp = 16;
    uint32_t primary = 0;  // object of the primary surface, or 0
};

// Display modes offered by EnumDisplayModes, in each depth.
constexpr std::array<std::pair<int, int>, 6> kModes = {{{640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 960}, {1280, 1024}}};
constexpr std::array<int, 3> kDepths = {8, 16, 32};

// Without DDPCAPS_ALLOW256, entries 0 and 255 of a palette of the primary surface are reserved
// in exclusive mode: always black and white, whatever the program writes there. Cossacks relies on
// it: the highlights of its sprites (the windmill's dome) use index 255, which its own palette
// file makes dark.
constexpr uint32_t DDPCAPS_ALLOW256 = 0x40;
void reserve_entries(std::array<uint32_t, 256>& colours) {
    colours[0] = 0xFF000000u;
    colours[255] = 0xFFFFFFFFu;
}

struct Method {
    const char* name;
    uint32_t args;  // en comptant l'objet
    std::function<uint32_t()> fn;  // vide : méthode non écrite
};

uint32_t make_vtable(Process& p, const std::string& interface, const std::vector<Method>& methods) {
    const uint32_t table = p.alloc(static_cast<uint32_t>(methods.size() * 4));
    for (size_t i = 0; i < methods.size(); ++i) {
        const std::string name = interface + "::" + methods[i].name;
        std::function<uint32_t()> fn = methods[i].fn;
        if (!fn) fn = []() -> uint32_t { throw std::runtime_error("méthode non fournie"); };
        p.cpu().write_u32(table + 4 * static_cast<uint32_t>(i), p.host_function(name, methods[i].args * 4, std::move(fn)));
    }
    return table;
}

uint32_t make_object(Process& p, uint32_t vtable) {
    const uint32_t object = p.alloc(8);
    p.cpu().write_u32(object, vtable);
    return object;
}

// Pixel format announced to the program: 8 bits through a palette, 16 bits 5-5-5 (that of
// Zeus' sprite files) or 32 bits x8r8g8b8.
void write_pixel_format(Cpu& cpu, uint32_t address, int bpp) {
    cpu.fill(address, 0, 32);
    cpu.write_u32(address + 0, 32);
    cpu.write_u32(address + 12, static_cast<uint32_t>(bpp));
    if (bpp == 8) {
        cpu.write_u32(address + 4, 0x40 | 0x20);  // DDPF_RGB | DDPF_PALETTEINDEXED8
        return;
    }
    cpu.write_u32(address + 4, 0x40);  // DDPF_RGB
    cpu.write_u32(address + 16, bpp == 32 ? 0xFF0000 : 0x7C00);
    cpu.write_u32(address + 20, bpp == 32 ? 0x00FF00 : 0x03E0);
    cpu.write_u32(address + 24, bpp == 32 ? 0x0000FF : 0x001F);
}

void write_surface_desc(Cpu& cpu, uint32_t desc, const Surface& s, uint32_t pixels) {
    cpu.write_u32(desc + 4, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_LPSURFACE | DDSD_PIXELFORMAT);
    cpu.write_u32(desc + 8, static_cast<uint32_t>(s.height));
    cpu.write_u32(desc + 12, static_cast<uint32_t>(s.width));
    cpu.write_u32(desc + 16, static_cast<uint32_t>(s.pitch));
    cpu.write_u32(desc + 36, pixels);
    write_pixel_format(cpu, desc + 72, s.bpp);
    cpu.write_u32(desc + 104, s.primary ? DDSCAPS_PRIMARYSURFACE : 0x40);  // sinon OFFSCREENPLAIN
}

struct Rect {
    int left, top, right, bottom;
};

Rect read_rect(Cpu& cpu, uint32_t address, const Surface& whole) {
    if (!address) return {0, 0, whole.width, whole.height};
    return {static_cast<int32_t>(cpu.read_u32(address)), static_cast<int32_t>(cpu.read_u32(address + 4)),
            static_cast<int32_t>(cpu.read_u32(address + 8)), static_cast<int32_t>(cpu.read_u32(address + 12))};
}

}  // namespace

void install_ddraw(Process& p) {
    auto st = std::make_shared<State>();
    Cpu& cpu = p.cpu();

    auto surface = [st](uint32_t object) -> Surface& {
        const auto found = st->surfaces.find(object);
        if (found == st->surfaces.end()) throw std::runtime_error("surface inconnue");
        return found->second;
    };
    // Recopie la surface primaire dans la fenêtre, lue en place dans la mémoire émulée.
    auto present = [&p, &cpu, st](const Surface& s) {
        Desktop& d = p.desktop();
        p.last_refresh = p.peek_ticks();
        if (s.bpp == 8) {
            static const std::array<uint32_t, 256> grey = [] {
                std::array<uint32_t, 256> g{};
                for (uint32_t i = 0; i < 256; ++i) g[i] = 0xFF000000u | i << 16 | i << 8 | i;
                return g;
            }();
            const auto palette = st->palettes.find(s.palette);
            d.present8(cpu.host(s.pixels), s.width, s.height, s.pitch,
                       palette == st->palettes.end() ? grey.data() : palette->second.data());
        } else if (s.bpp == 32) {
            d.present32(cpu.host(s.pixels), s.width, s.height, s.pitch);
        } else {
            d.present(cpu.host(s.pixels), s.width, s.height, s.pitch, false);
        }
        if (d.should_stop()) p.exit(0);
    };
    auto new_surface = [&p, st](int width, int height, bool primary, int bpp) {
        Surface s;
        s.width = width;
        s.height = height;
        s.bpp = bpp;
        s.pitch = (width * (bpp / 8) + 3) & ~3;
        s.pixels = p.alloc(static_cast<uint32_t>(s.pitch * height));
        s.primary = primary;
        const uint32_t object = make_object(p, st->surface_vtable);
        st->surfaces[object] = s;
        if (primary) st->primary = object;
        return object;
    };
    // Changement de mode d'affichage : la fenêtre plein écran suit la taille de l'écran et en
    // est avertie. Le jeu en dépend : son traitement de WM_MOVE (0x5C4C14) est le seul code qui
    // fixe le rectangle d'écran (0xF2AB08) copié à chaque image par FUN_0050c1b0.
    // Hypothèse : la liste et l'ordre exacts des messages de Windows ne sont pas établis.
    auto set_mode = [&p, st](int width, int height, uint32_t result) {
        Desktop& d = p.desktop();
        if (width == d.width() && height == d.height()) return;
        d.set_mode(width, height);
        const uint32_t size = static_cast<uint32_t>(height) << 16 | static_cast<uint32_t>(width);
        p.send_messages({{d.hwnd, WM_DISPLAYCHANGE, 16, size}, {d.hwnd, WM_SIZE, 0, size}, {d.hwnd, WM_MOVE, 0, 0}},
                        result);
    };
    // Copie d'un rectangle entre surfaces, avec couleur transparente facultative.
    auto copy = [&cpu](const Surface& dst, int dx, int dy, const Surface& src, Rect r, bool keyed) {
        if (r.left < 0) { dx -= r.left; r.left = 0; }
        if (r.top < 0) { dy -= r.top; r.top = 0; }
        r.right = std::min(r.right, src.width);
        r.bottom = std::min(r.bottom, src.height);
        if (dx < 0) { r.left -= dx; dx = 0; }
        if (dy < 0) { r.top -= dy; dy = 0; }
        const int w = std::min(r.right - r.left, dst.width - dx), h = std::min(r.bottom - r.top, dst.height - dy);
        if (w <= 0 || h <= 0) return;
        // En place dans la mémoire émulée, ligne par ligne dans le même ordre qu'avant ; chaque
        // ligne source est lue en entier avant d'écrire la destination (surfaces qui se recouvrent).
        if (src.bpp != dst.bpp) throw std::runtime_error("copie entre surfaces de profondeurs différentes");
        const int bytes_per_pixel = src.bpp / 8;
        const size_t bytes = static_cast<size_t>(w) * bytes_per_pixel;
        std::vector<uint8_t> line(keyed ? bytes : 0);
        for (int y = 0; y < h; ++y) {
            const uint8_t* from = cpu.host(src.pixels + static_cast<uint32_t>((r.top + y) * src.pitch + r.left * bytes_per_pixel));
            uint8_t* to = cpu.host(dst.pixels + static_cast<uint32_t>((dy + y) * dst.pitch + dx * bytes_per_pixel));
            if (!keyed) {
                std::memmove(to, from, bytes);
                continue;
            }
            std::memcpy(line.data(), from, bytes);
            for (int x = 0; x < w; ++x) {
                uint32_t c = 0;
                std::memcpy(&c, &line[static_cast<size_t>(x) * bytes_per_pixel], static_cast<size_t>(bytes_per_pixel));
                if (c != src.color_key) std::memcpy(to + x * bytes_per_pixel, &c, static_cast<size_t>(bytes_per_pixel));
            }
        }
    };

    p.refresh_screen = [st, present] {
        const auto primary = st->surfaces.find(st->primary);
        if (primary != st->surfaces.end()) present(primary->second);
    };

    // --- IDirectDrawSurface ------------------------------------------------------------
    st->surface_vtable = make_vtable(p, "IDirectDrawSurface", {
        {"QueryInterface", 3, [&p, &cpu, surface] {
            ++surface(p.arg(0)).refs;
            cpu.write_u32(p.arg(2), p.arg(0));
            return DD_OK;
        }},
        {"AddRef", 1, [&p, surface] { return static_cast<uint32_t>(++surface(p.arg(0)).refs); }},
        {"Release", 1, [&p, st] {
            const auto found = st->surfaces.find(p.arg(0));
            if (found == st->surfaces.end()) return 0u;
            if (--found->second.refs > 0) return static_cast<uint32_t>(found->second.refs);
            p.release(found->second.pixels);
            st->surfaces.erase(found);
            return 0u;
        }},
        {"AddAttachedSurface", 2, {}},
        {"AddOverlayDirtyRect", 2, {}},
        {"Blt", 6, [&p, &cpu, surface, copy, present] {
            Surface& dst = surface(p.arg(0));
            const uint32_t flags = p.arg(4);
            const Rect d = read_rect(cpu, p.arg(1), dst);
            if (p.trace)
                std::fprintf(stderr, "    Blt vers %#x (%d,%d)-(%d,%d) depuis %#x, indicateurs %#x\n", p.arg(0), d.left,
                             d.top, d.right, d.bottom, p.arg(2), flags);
            if (flags & DDBLT_COLORFILL) {
                const uint32_t color = cpu.read_u32(p.arg(5) + 80);  // DDBLTFX.dwFillColor
                const int x0 = std::max(d.left, 0), x1 = std::min(d.right, dst.width);
                const int bytes_per_pixel = dst.bpp / 8;
                if (x1 > x0) {
                    std::vector<uint8_t> line(static_cast<size_t>(x1 - x0) * bytes_per_pixel);
                    for (size_t i = 0; i < line.size(); i += static_cast<size_t>(bytes_per_pixel))
                        std::memcpy(&line[i], &color, static_cast<size_t>(bytes_per_pixel));
                    for (int y = std::max(d.top, 0); y < std::min(d.bottom, dst.height); ++y)
                        cpu.write(dst.pixels + static_cast<uint32_t>(y * dst.pitch + x0 * bytes_per_pixel), line.data(), line.size());
                }
            } else if (p.arg(2)) {
                const Surface& src = surface(p.arg(2));
                const Rect s = read_rect(cpu, p.arg(3), src);
                if (s.right - s.left != d.right - d.left || s.bottom - s.top != d.bottom - d.top)
                    throw std::runtime_error("copie avec changement d'échelle non fournie");
                copy(dst, d.left, d.top, src, s, (flags & DDBLT_KEYSRC) && src.has_color_key);
            }
            if (dst.primary) present(dst);
            return DD_OK;
        }},
        {"BltBatch", 4, {}},
        {"BltFast", 6, [&p, &cpu, surface, copy, present] {
            Surface& dst = surface(p.arg(0));
            const Surface& src = surface(p.arg(3));
            if (p.trace) {
                const Rect r = read_rect(cpu, p.arg(4), src);
                std::fprintf(stderr, "    BltFast vers %#x en (%d,%d) depuis %#x (%d,%d)-(%d,%d), indicateurs %#x\n",
                             p.arg(0), static_cast<int32_t>(p.arg(1)), static_cast<int32_t>(p.arg(2)), p.arg(3), r.left,
                             r.top, r.right, r.bottom, p.arg(5));
            }
            copy(dst, static_cast<int32_t>(p.arg(1)), static_cast<int32_t>(p.arg(2)), src,
                 read_rect(cpu, p.arg(4), src), (p.arg(5) & DDBLTFAST_SRCCOLORKEY) && src.has_color_key);
            if (dst.primary) present(dst);
            return DD_OK;
        }},
        {"DeleteAttachedSurface", 3, {}},
        {"EnumAttachedSurfaces", 3, {}},
        {"EnumOverlayZOrders", 4, {}},
        {"Flip", 3, [&p, surface, present] {
            Surface& front = surface(p.arg(0));
            if (!front.back_buffer) return DDERR_UNSUPPORTED;
            std::swap(front.pixels, surface(front.back_buffer).pixels);
            present(front);
            return DD_OK;
        }},
        {"GetAttachedSurface", 3, [&p, &cpu, surface] {
            Surface& s = surface(p.arg(0));
            if (!s.back_buffer || !(cpu.read_u32(p.arg(1)) & DDSCAPS_BACKBUFFER)) return DDERR_NOTFOUND;
            ++surface(s.back_buffer).refs;
            cpu.write_u32(p.arg(2), s.back_buffer);
            return DD_OK;
        }},
        {"GetBltStatus", 2, [] { return DD_OK; }},
        {"GetCaps", 2, [&p, &cpu, surface] {
            cpu.write_u32(p.arg(1), surface(p.arg(0)).primary ? DDSCAPS_PRIMARYSURFACE : 0x40);
            return DD_OK;
        }},
        {"GetClipper", 2, {}},
        {"GetColorKey", 3, {}},
        {"GetDC", 2, {}},
        {"GetFlipStatus", 2, [] { return DD_OK; }},
        {"GetOverlayPosition", 3, {}},
        {"GetPalette", 2, [&p, &cpu, surface] {
            const Surface& s = surface(p.arg(0));
            if (!s.palette) return 0x887600FFu;  // DDERR_NOPALETTEATTACHED
            cpu.write_u32(p.arg(1), s.palette);
            return DD_OK;
        }},
        {"GetPixelFormat", 2, [&p, &cpu, surface] {
            write_pixel_format(cpu, p.arg(1), surface(p.arg(0)).bpp);
            return DD_OK;
        }},
        {"GetSurfaceDesc", 2, [&p, &cpu, surface] {
            const Surface& s = surface(p.arg(0));
            write_surface_desc(cpu, p.arg(1), s, s.pixels);
            return DD_OK;
        }},
        {"Initialize", 3, {}},
        {"IsLost", 1, [] { return DD_OK; }},
        {"Lock", 5, [&p, &cpu, surface] {
            const Surface& s = surface(p.arg(0));
            uint32_t pixels = s.pixels;
            if (p.arg(1)) {
                const Rect r = read_rect(cpu, p.arg(1), s);
                pixels += static_cast<uint32_t>(r.top * s.pitch + r.left * (s.bpp / 8));
            }
            write_surface_desc(cpu, p.arg(2), s, pixels);
            return DD_OK;
        }},
        {"ReleaseDC", 2, {}},
        {"Restore", 1, [] { return DD_OK; }},
        {"SetClipper", 2, [] { return DD_OK; }},
        {"SetColorKey", 3, [&p, &cpu, surface] {
            Surface& s = surface(p.arg(0));
            s.has_color_key = p.arg(2) != 0;
            if (p.arg(2)) s.color_key = cpu.read_u32(p.arg(2));  // dwColorSpaceLowValue
            return DD_OK;
        }},
        {"SetOverlayPosition", 3, {}},
        {"SetPalette", 2, [&p, surface, present] {
            Surface& s = surface(p.arg(0));
            s.palette = p.arg(1);
            if (s.primary) present(s);
            return DD_OK;
        }},
        {"Unlock", 2, [&p, surface, present] {
            const Surface& s = surface(p.arg(0));
            if (s.primary) present(s);
            return DD_OK;
        }},
        {"UpdateOverlay", 6, {}},
        {"UpdateOverlayDisplay", 2, {}},
        {"UpdateOverlayZOrder", 3, {}},
    });

    // --- IDirectDrawClipper et IDirectDrawPalette : acceptés, sans effet -----------------
    st->clipper_vtable = make_vtable(p, "IDirectDrawClipper", {
        {"QueryInterface", 3, {}},
        {"AddRef", 1, [] { return 2u; }},
        {"Release", 1, [] { return 0u; }},
        {"GetClipList", 4, {}},
        {"GetHWnd", 2, {}},
        {"Initialize", 3, {}},
        {"IsClipListChanged", 2, {}},
        {"SetClipList", 3, [] { return DD_OK; }},
        {"SetHWnd", 3, [] { return DD_OK; }},
    });
    // PALETTEENTRY: red, green, blue, flags, one byte each.
    st->palette_vtable = make_vtable(p, "IDirectDrawPalette", {
        {"QueryInterface", 3, {}},
        {"AddRef", 1, [] { return 2u; }},
        {"Release", 1, [] { return 0u; }},
        {"GetCaps", 2, [&p, &cpu] {
            cpu.write_u32(p.arg(1), 0x4 | 0x40);  // DDPCAPS_8BIT | DDPCAPS_ALLOW256
            return DD_OK;
        }},
        {"GetEntries", 5, [&p, &cpu, st] {
            const auto& colours = st->palettes[p.arg(0)];
            const uint32_t start = p.arg(2), count = p.arg(3), out = p.arg(4);
            for (uint32_t i = 0; i < count && start + i < 256; ++i) {
                const uint32_t c = colours[start + i];
                const uint8_t entry[4] = {static_cast<uint8_t>(c >> 16), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c), 0};
                cpu.write(out + 4 * i, entry, 4);
            }
            return DD_OK;
        }},
        {"Initialize", 4, {}},
        {"SetEntries", 5, [&p, &cpu, st, present] {
            auto& colours = st->palettes[p.arg(0)];
            const uint32_t start = p.arg(2), count = p.arg(3), in = p.arg(4);
            for (uint32_t i = 0; i < count && start + i < 256; ++i) {
                uint8_t entry[4];
                cpu.read(in + 4 * i, entry, 4);
                colours[start + i] = 0xFF000000u | static_cast<uint32_t>(entry[0]) << 16 | static_cast<uint32_t>(entry[1]) << 8 | entry[2];
            }
            if (!st->all_256[p.arg(0)]) reserve_entries(colours);
            // A new palette shows at once on the screen (fades from and to black).
            const auto primary = st->surfaces.find(st->primary);
            if (primary != st->surfaces.end() && primary->second.palette == p.arg(0)) present(primary->second);
            return DD_OK;
        }},
    });

    // --- IDirectDraw -------------------------------------------------------------------
    const uint32_t ddraw_vtable = make_vtable(p, "IDirectDraw", {
        {"QueryInterface", 3, [&p, &cpu] {
            uint8_t iid[16];
            cpu.read(p.arg(1), iid, 16);
            std::fprintf(stderr, "[hôte] IDirectDraw::QueryInterface {%02x%02x%02x%02x-...} : même objet rendu\n",
                         iid[3], iid[2], iid[1], iid[0]);
            cpu.write_u32(p.arg(2), p.arg(0));
            return DD_OK;
        }},
        {"AddRef", 1, [] { return 2u; }},
        {"Release", 1, [] { return 0u; }},
        {"Compact", 1, [] { return DD_OK; }},
        {"CreateClipper", 4, [&p, &cpu, st] {
            cpu.write_u32(p.arg(2), make_object(p, st->clipper_vtable));
            return DD_OK;
        }},
        {"CreatePalette", 5, [&p, &cpu, st] {
            const uint32_t object = make_object(p, st->palette_vtable);
            auto& colours = st->palettes[object];
            colours.fill(0xFF000000u);
            if (const uint32_t in = p.arg(2))
                for (uint32_t i = 0; i < 256; ++i) {
                    uint8_t entry[4];
                    cpu.read(in + 4 * i, entry, 4);
                    colours[i] = 0xFF000000u | static_cast<uint32_t>(entry[0]) << 16 | static_cast<uint32_t>(entry[1]) << 8 | entry[2];
                }
            st->all_256[object] = (p.arg(1) & DDPCAPS_ALLOW256) != 0;
            if (!st->all_256[object]) reserve_entries(colours);
            if (p.trace) std::fprintf(stderr, "    palette %#x (indicateurs %#x)\n", object, p.arg(1));
            cpu.write_u32(p.arg(3), object);
            return DD_OK;
        }},
        {"CreateSurface", 4, [&p, &cpu, st, new_surface, surface] {
            const uint32_t desc = p.arg(1);
            const uint32_t flags = cpu.read_u32(desc + 4), caps = cpu.read_u32(desc + 104);
            const bool primary = (flags & DDSD_CAPS) && (caps & DDSCAPS_PRIMARYSURFACE);
            int width = st->mode_width, height = st->mode_height, bpp = st->mode_bpp;
            if (!primary) {
                if (flags & DDSD_WIDTH) width = static_cast<int32_t>(cpu.read_u32(desc + 12));
                if (flags & DDSD_HEIGHT) height = static_cast<int32_t>(cpu.read_u32(desc + 8));
                if ((flags & DDSD_PIXELFORMAT) && cpu.read_u32(desc + 72 + 12)) bpp = static_cast<int>(cpu.read_u32(desc + 72 + 12));
            }
            if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return 0x80070057u;  // E_INVALIDARG
            if (bpp != 8 && bpp != 16 && bpp != 32) return DDERR_UNSUPPORTED;
            const uint32_t object = new_surface(width, height, primary, bpp);
            if (p.trace)
                std::fprintf(stderr, "    surface %#x : %dx%d, %d bits%s (indicateurs %#x, capacités %#x)\n", object, width,
                             height, bpp, primary ? ", primaire" : "", flags, caps);
            if (primary && (flags & DDSD_BACKBUFFERCOUNT) && cpu.read_u32(desc + 20) > 0)
                surface(object).back_buffer = new_surface(width, height, false, bpp);
            cpu.write_u32(p.arg(2), object);
            return DD_OK;
        }},
        {"DuplicateSurface", 3, {}},
        {"EnumDisplayModes", 5, [&p, &cpu] {
            // The callback (DDSURFACEDESC*, context) is called for each mode until it returns 0.
            const uint32_t context = p.arg(3), callback = p.arg(4);
            // The usual modes, then wide ones in the proportions of the Mac's screen (width a
            // multiple of 32), so that the game can fill it: the game lists them in its options.
            std::vector<std::pair<int, int>> sizes(kModes.begin(), kModes.end());
            const double aspect = p.desktop().screen_aspect();
            if (aspect > 1.4)
                for (int h : {720, 768, 800, 900, 1050, 1080}) {
                    const int w = static_cast<int>(h * aspect / 32 + 0.5) * 32;
                    if (std::find(sizes.begin(), sizes.end(), std::make_pair(w, h)) == sizes.end()) sizes.push_back({w, h});
                }
            auto modes = std::make_shared<std::vector<std::pair<std::pair<int, int>, int>>>();
            for (int depth : kDepths)
                for (const auto& m : sizes) modes->push_back({m, depth});
            const uint32_t desc = p.alloc(108);
            auto next = std::make_shared<std::function<uint32_t(size_t)>>();
            *next = [&p, &cpu, modes, desc, context, callback, next](size_t i) -> uint32_t {
                if (i >= modes->size()) {
                    p.release(desc);
                    return DD_OK;
                }
                Surface mode;
                mode.width = (*modes)[i].first.first;
                mode.height = (*modes)[i].first.second;
                mode.bpp = (*modes)[i].second;
                mode.pitch = mode.width * mode.bpp / 8;
                cpu.fill(desc, 0, 108);
                cpu.write_u32(desc, 108);
                write_surface_desc(cpu, desc, mode, 0);
                cpu.write_u32(desc + 4, DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT);
                cpu.call_guest(callback, {desc, context}, [next, i, desc, &p](uint32_t more) -> uint32_t {
                    if (!more) {
                        p.release(desc);
                        return DD_OK;
                    }
                    return (*next)(i + 1);
                });
                return DD_OK;
            };
            return (*next)(0);
        }},
        {"EnumSurfaces", 5, {}},
        {"FlipToGDISurface", 1, [] { return DD_OK; }},
        {"GetCaps", 3, [&p, &cpu] {
            // Capacités toutes nulles : pas d'accélération matérielle annoncée.
            for (int i = 1; i <= 2; ++i) {
                const uint32_t caps = p.arg(i);
                if (!caps) continue;
                const uint32_t size = cpu.read_u32(caps);
                if (size > 4 && size <= 1024) cpu.fill(caps + 4, 0, size - 4);
            }
            return DD_OK;
        }},
        {"GetDisplayMode", 2, [&p, &cpu, st] {
            Surface mode;
            mode.width = st->mode_width;
            mode.height = st->mode_height;
            mode.bpp = st->mode_bpp;
            mode.pitch = st->mode_width * st->mode_bpp / 8;
            write_surface_desc(cpu, p.arg(1), mode, 0);
            cpu.write_u32(p.arg(1) + 4, DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT);
            return DD_OK;
        }},
        {"GetFourCCCodes", 3, {}},
        {"GetGDISurface", 2, {}},
        {"GetMonitorFrequency", 2, [&p, &cpu] {
            cpu.write_u32(p.arg(1), 60);
            return DD_OK;
        }},
        {"GetScanLine", 2, {}},
        {"GetVerticalBlankStatus", 2, [&p, &cpu] {
            cpu.write_u32(p.arg(1), 1);
            return DD_OK;
        }},
        {"Initialize", 2, {}},
        {"RestoreDisplayMode", 1, [set_mode] {
            set_mode(Desktop::kScreenWidth, Desktop::kScreenHeight, DD_OK);
            return DD_OK;
        }},
        {"SetCooperativeLevel", 3, [] { return DD_OK; }},
        {"SetDisplayMode", 4, [&p, st, set_mode] {
            const int bpp = static_cast<int>(p.arg(3));
            if (bpp != 8 && bpp != 16 && bpp != 32) return DDERR_UNSUPPORTED;
            st->mode_width = static_cast<int32_t>(p.arg(1));
            st->mode_height = static_cast<int32_t>(p.arg(2));
            st->mode_bpp = bpp;
            if (p.trace) std::fprintf(stderr, "    mode %dx%d, %d bits\n", st->mode_width, st->mode_height, bpp);
            set_mode(st->mode_width, st->mode_height, DD_OK);
            return DD_OK;
        }},
        {"WaitForVerticalBlank", 3, [] { return DD_OK; }},
    });

    // MDRAW.DLL: GSC's wrapper in the GOG edition; CSEMU.DLL: the Steam edition's.
    for (const char* dll : {"ddraw.dll", "mdraw.dll", "csemu.dll"})
        p.def(dll, "DirectDrawCreate", 12, [&p, &cpu, ddraw_vtable] {
            cpu.write_u32(p.arg(1), make_object(p, ddraw_vtable));
            return DD_OK;
        });
}

}  // namespace zeus::win32
