#include "win32/desktop.h"

#include "win32/text.h"

#include <SDL.h>
#ifdef __APPLE__
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <CoreFoundation/CoreFoundation.h>
extern char** environ;  // the process environment, for posix_spawnp
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace zeus::win32 {
namespace {

// Messages Windows produits à partir des événements SDL.
constexpr uint32_t WM_ACTIVATE = 0x0006, WM_SETFOCUS = 0x0007, WM_KILLFOCUS = 0x0008,
                   WM_CLOSE = 0x0010, WM_ACTIVATEAPP = 0x001C,
                   WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102,
                   WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202,
                   WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205, WM_MBUTTONDOWN = 0x0207,
                   WM_MBUTTONUP = 0x0208, WM_MOUSEWHEEL = 0x020A;
constexpr int kWheelDelta = 120;  // un cran de molette, pour Windows
constexpr uint64_t kFocusDelayMs = 400;
constexpr uint32_t kScrollFirstRepeatMs = 120, kScrollRepeatMs = 50;  // repeat of the arrows on the city screens
constexpr uint32_t MK_LBUTTON = 1, MK_RBUTTON = 2, MK_MBUTTON = 0x10;

// Cursors. SDL2 gives a cursor a single image, measured in points: on a Retina screen macOS
// enlarges it and blurs it. When SDL2 is sdl2-compat (SDL2 implemented on top of SDL3, as Homebrew
// installs it), SDL3 also takes one image per screen density: its functions are then taken from
// the SDL3 library already loaded, and everything about cursors goes through them.
struct CursorApi {
    struct Surface3 {  // start of SDL3's SDL_Surface (a public structure)
        uint32_t flags, format;
        int w, h, pitch;
        void* pixels;
    };
    Surface3* (*create_surface)(int, int, uint32_t) = nullptr;
    bool (*add_alternate_image)(Surface3*, Surface3*) = nullptr;
    void (*destroy_surface)(Surface3*) = nullptr;
    SDL_Cursor* (*create_color_cursor)(Surface3*, int, int) = nullptr;
    bool (*set_cursor)(SDL_Cursor*) = nullptr;
    SDL_Cursor* (*default_cursor)() = nullptr;
    void (*destroy_cursor)(SDL_Cursor*) = nullptr;
    bool sdl3 = false;

    CursorApi() {
#ifdef __APPLE__
        SDL_version version;
        SDL_GetVersion(&version);
        if (version.patch < 50) return;  // a real SDL2 (sdl2-compat numbers itself 2.32.50 and later)
        void* library = nullptr;
        for (uint32_t i = 0, n = _dyld_image_count(); i < n && !library; ++i) {
            const char* name = _dyld_get_image_name(i);
            if (name && std::strstr(name, "libSDL3")) library = dlopen(name, RTLD_LAZY | RTLD_NOLOAD);
        }
        if (!library) return;
        auto take = [library](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(library, name));
            return fn != nullptr;
        };
        sdl3 = take(create_surface, "SDL_CreateSurface") && take(add_alternate_image, "SDL_AddSurfaceAlternateImage") &&
               take(destroy_surface, "SDL_DestroySurface") && take(create_color_cursor, "SDL_CreateColorCursor") &&
               take(set_cursor, "SDL_SetCursor") && take(default_cursor, "SDL_GetDefaultCursor") &&
               take(destroy_cursor, "SDL_DestroyCursor");
#endif
    }
    static const CursorApi& get() {
        static const CursorApi api;
        return api;
    }
    void set(SDL_Cursor* cursor) const {
        if (sdl3) set_cursor(cursor ? cursor : default_cursor());
        else SDL_SetCursor(cursor ? cursor : SDL_GetDefaultCursor());
    }
    void destroy(SDL_Cursor* cursor) const {
        if (!cursor) return;
        if (sdl3) destroy_cursor(cursor);
        else SDL_FreeCursor(cursor);
    }
};

// The cursor's image brought to w x h pixels: each pixel gets the average of the image pixels it
// covers, weighted by area (premultiplied alpha). Sharp at whole factors, with softened edges
// otherwise, like the game's picture.
std::vector<uint32_t> resample_cursor(const CursorImage& image, int w, int h) {
    std::vector<uint32_t> out(static_cast<size_t>(w) * h, 0);
    const double sx = static_cast<double>(image.width) / w, sy = static_cast<double>(image.height) / h;
    for (int y = 0; y < h; ++y) {
        const double y0 = y * sy, y1 = (y + 1) * sy;
        for (int x = 0; x < w; ++x) {
            const double x0 = x * sx, x1 = (x + 1) * sx;
            double a = 0, r = 0, g = 0, b = 0;
            for (int j = static_cast<int>(y0); j < image.height && j < y1; ++j) {
                const double wy = std::min(y1, j + 1.0) - std::max(y0, static_cast<double>(j));
                for (int i = static_cast<int>(x0); i < image.width && i < x1; ++i) {
                    const double wx = std::min(x1, i + 1.0) - std::max(x0, static_cast<double>(i));
                    const uint32_t p = image.argb[static_cast<size_t>(j) * image.width + i];
                    const double weight = (p >> 24) * wx * wy;
                    a += weight;
                    r += ((p >> 16) & 255) * weight;
                    g += ((p >> 8) & 255) * weight;
                    b += (p & 255) * weight;
                }
            }
            if (a <= 0) continue;
            const auto channel = [](double v) { return static_cast<uint32_t>(std::clamp(std::lround(v), 0L, 255L)); };
            out[static_cast<size_t>(y) * w + x] =
                channel(a / (sx * sy)) << 24 | channel(r / a) << 16 | channel(g / a) << 8 | channel(b / a);
        }
    }
    return out;
}

// Code de touche virtuelle Windows pour une touche SDL, ou 0.
int virtual_key(SDL_Keycode key) {
    if (key >= SDLK_a && key <= SDLK_z) return 'A' + (key - SDLK_a);
    if (key >= SDLK_0 && key <= SDLK_9) return '0' + (key - SDLK_0);
    if (key >= SDLK_F1 && key <= SDLK_F12) return 0x70 + (key - SDLK_F1);
    switch (key) {
        case SDLK_LEFT: return 0x25;
        case SDLK_UP: return 0x26;
        case SDLK_RIGHT: return 0x27;
        case SDLK_DOWN: return 0x28;
        case SDLK_ESCAPE: return 0x1B;
        case SDLK_RETURN: case SDLK_KP_ENTER: return 0x0D;
        case SDLK_SPACE: return 0x20;
        case SDLK_BACKSPACE: return 0x08;
        case SDLK_TAB: return 0x09;
        case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10;
        case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
        case SDLK_LALT: case SDLK_RALT: return 0x12;
        case SDLK_PAUSE: return 0x13;
        case SDLK_PAGEUP: return 0x21;
        case SDLK_PAGEDOWN: return 0x22;
        case SDLK_END: return 0x23;
        case SDLK_HOME: return 0x24;
        case SDLK_INSERT: return 0x2D;
        case SDLK_DELETE: return 0x2E;
        case SDLK_KP_PLUS: return 0x6B;
        case SDLK_KP_MINUS: return 0x6D;
        case SDLK_MINUS: return 0xBD;
        case SDLK_EQUALS: return 0xBB;
        case SDLK_COMMA: return 0xBC;
        case SDLK_PERIOD: return 0xBE;
        case SDLK_LEFTBRACKET: return 0xDB;
        case SDLK_RIGHTBRACKET: return 0xDD;
        case SDLK_BACKQUOTE: return 0xC0;  // VK_OEM_3: ~ shows the units' and buildings' health
        case SDLK_SEMICOLON: return 0xBA;
        case SDLK_QUOTE: return 0xDE;
        case SDLK_BACKSLASH: return 0xDC;
        case SDLK_SLASH: return 0xBF;
        default: return 0;
    }
}

// The key at this position on a US keyboard, for a key whose character has no virtual key of its
// own: with a Cyrillic layout the letter keys give Cyrillic characters, while Windows still gives
// them the virtual keys of their US position (VK_A…, VK_OEM_3 for the key left of 1).
int virtual_key_at(SDL_Scancode scan) {
    if (scan >= SDL_SCANCODE_A && scan <= SDL_SCANCODE_Z) return 'A' + (scan - SDL_SCANCODE_A);
    if (scan >= SDL_SCANCODE_1 && scan <= SDL_SCANCODE_9) return '1' + (scan - SDL_SCANCODE_1);
    switch (scan) {
        case SDL_SCANCODE_0: return '0';
        case SDL_SCANCODE_GRAVE: return 0xC0;
        case SDL_SCANCODE_MINUS: return 0xBD;
        case SDL_SCANCODE_EQUALS: return 0xBB;
        case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
        case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
        case SDL_SCANCODE_BACKSLASH: return 0xDC;
        case SDL_SCANCODE_SEMICOLON: return 0xBA;
        case SDL_SCANCODE_APOSTROPHE: return 0xDE;
        case SDL_SCANCODE_COMMA: return 0xBC;
        case SDL_SCANCODE_PERIOD: return 0xBE;
        case SDL_SCANCODE_SLASH: return 0xBF;
        default: return 0;
    }
}

}  // namespace

Desktop::Desktop() : keys_(256, false) {
#ifdef __APPLE__
    // A letter key held down: macOS opens its accent menu (à, á, â…) instead of repeating the key,
    // which stops W A S D scrolling and leaves a popup over the game. A setting of this application
    // only, made before SDL starts text input.
    CFPreferencesSetAppValue(CFSTR("ApplePressAndHoldEnabled"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
    CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);
#endif
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    start_ticks_ = SDL_GetTicks64();
}

Desktop::~Desktop() {
    if (frame_log_) std::fclose(frame_log_);
    if (std::any_of(cursors_.begin(), cursors_.end(), [](SDL_Cursor* c) { return c != nullptr; })) {
        CursorApi::get().set(nullptr);
        for (SDL_Cursor* c : cursors_) CursorApi::get().destroy(c);
    }
    if (upscaled_) SDL_DestroyTexture(upscaled_);
    if (texture_) SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

double Desktop::screen_aspect() const {
    SDL_DisplayMode mode;
    if (SDL_GetDesktopDisplayMode(0, &mode) != 0 || mode.w <= 0 || mode.h <= 0) return 4.0 / 3.0;
    return static_cast<double>(mode.w) / mode.h;
}

// Size of the window for a picture of width x height: the picture itself, made smaller (same
// proportions) when it would not fit on the screen, menu bar and Dock excluded.
static void fit_window(int& width, int& height) {
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(0, &usable) != 0 || usable.w <= 0 || usable.h <= 0) return;
    const double scale = std::min({1.0, 0.95 * usable.w / width, 0.92 * usable.h / height});
    width = static_cast<int>(width * scale);
    height = static_cast<int>(height * scale);
}

void Desktop::create_window(const std::string& title, int width, int height) {
    if (window_) return;
    if (width > 0 && height > 0) {
        width_ = width;
        height_ = height;
    }
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
    if (headless) flags |= SDL_WINDOW_HIDDEN;
    else if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    int window_w = width_, window_h = height_;
    fit_window(window_w, window_h);
    window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, window_w,
                               window_h, flags);
    if (!window_) throw std::runtime_error(SDL_GetError());
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) throw std::runtime_error(SDL_GetError());
    if (!system_cursor_shown_ && !headless) SDL_ShowCursor(SDL_DISABLE);
}

void Desktop::set_mode(int width, int height) {
    width_ = width;
    height_ = height;
    if (window_ && !fullscreen) {
        fit_window(width, height);
        SDL_SetWindowSize(window_, width, height);
        SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

void Desktop::present(const uint8_t* pixels, int width, int height, int pitch, bool rgb565) {
    if (colors_.empty() || texture_565_ != rgb565) {
        // Table tirée de SDL lui-même : chaque pixel 16 bits garde exactement la couleur qu'il
        // produisait en convertissant chaque image.
        std::vector<uint16_t> all(65536);
        for (size_t c = 0; c < all.size(); ++c) all[c] = static_cast<uint16_t>(c);
        colors_.resize(all.size());
        if (SDL_ConvertPixels(256, 256, rgb565 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_RGB555, all.data(), 512,
                              SDL_PIXELFORMAT_ARGB8888, colors_.data(), 1024) != 0)
            throw std::runtime_error(SDL_GetError());
        texture_565_ = rgb565;
    }
    present_rows(width, height, [&](int y, uint32_t* out) {
        const uint8_t* row = pixels + static_cast<size_t>(y) * pitch;
        for (int x = 0; x < width; ++x) out[x] = colors_[static_cast<uint16_t>(row[2 * x] | row[2 * x + 1] << 8)];
    });
}

void Desktop::present8(const uint8_t* pixels, int width, int height, int pitch, const uint32_t* palette) {
    present_rows(width, height, [&](int y, uint32_t* out) {
        const uint8_t* row = pixels + static_cast<size_t>(y) * pitch;
        for (int x = 0; x < width; ++x) out[x] = palette[row[x]];
    });
}

void Desktop::present32(const uint8_t* pixels, int width, int height, int pitch) {
    present_rows(width, height, [&](int y, uint32_t* out) {
        const uint8_t* row = pixels + static_cast<size_t>(y) * pitch;
        for (int x = 0; x < width; ++x) {
            uint32_t c;
            std::memcpy(&c, row + 4 * x, 4);
            out[x] = 0xFF000000u | (c & 0xFFFFFF);
        }
    });
}

void Desktop::present_rows(int width, int height, const std::function<void(int, uint32_t*)>& fill) {
    const uint64_t started = SDL_GetPerformanceCounter();
    struct Timer {  // measures the whole of present(), whichever way it returns
        double& out;
        uint64_t start;
        ~Timer() { out = 1000.0 * static_cast<double>(SDL_GetPerformanceCounter() - start) / static_cast<double>(SDL_GetPerformanceFrequency()); }
    } timer{last_present_ms_, started};
    if (!window_) create_window("Cossacks", width, height);
    if (!texture_ || texture_w_ != width || texture_h_ != height) {
        if (texture_) SDL_DestroyTexture(texture_);
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!texture_) throw std::runtime_error(SDL_GetError());
        SDL_SetTextureScaleMode(texture_, SDL_ScaleModeNearest);
        if (upscaled_) SDL_DestroyTexture(upscaled_);
        upscaled_ = nullptr;
        texture_w_ = width;
        texture_h_ = height;
    }
    // The picture in ARGB, kept for the frame log and the screenshots.
    frame_.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) fill(y, &frame_[static_cast<size_t>(y) * width]);
    if (last_present_counter_)
        pace.gap_ms_max = std::max(pace.gap_ms_max, 1000.0 * static_cast<double>(started - last_present_counter_) /
                                                        static_cast<double>(SDL_GetPerformanceFrequency()));
    last_present_counter_ = started;
    ++pace.presents;
    if (overlay && !overlay_text.empty()) draw_overlay(width, height);
    if (!frame_log_path.empty()) {
        if (!frame_log_) frame_log_ = std::fopen(frame_log_path.c_str(), "w");
        uint32_t hash = 2166136261u;  // FNV-1a des pixels
        for (uint32_t c : frame_)
            for (int k = 0; k < 3; ++k) hash = (hash ^ ((c >> (8 * k)) & 0xFF)) * 16777619u;
        if (frame_log_) std::fprintf(frame_log_, "%d %dx%d %08x %llu\n", presents_, width, height, hash,
                                     static_cast<unsigned long long>(SDL_GetTicks64()));
    }
    // Fenêtre cachée sans capture de son contenu : rien à dessiner.
    if (!headless || !window_shot_path.empty()) {
        void* out = nullptr;
        int out_pitch = 0;
        if (SDL_LockTexture(texture_, nullptr, &out, &out_pitch) != 0) throw std::runtime_error(SDL_GetError());
        for (int y = 0; y < height; ++y)
            std::memcpy(static_cast<uint8_t*>(out) + static_cast<size_t>(y) * out_pitch, &frame_[static_cast<size_t>(y) * width],
                        static_cast<size_t>(width) * 4);
        SDL_UnlockTexture(texture_);

        // L'image garde ses proportions et se centre dans la fenêtre. À un facteur entier, chaque
        // pixel du jeu devient un carré net. Sinon on agrandit d'abord au facteur entier supérieur,
        // sans lissage, puis on réduit avec lissage : les pixels restent nets et réguliers.
        int out_w = 0, out_h = 0, win_w = 0, win_h = 0;
        SDL_GetRendererOutputSize(renderer_, &out_w, &out_h);
        SDL_GetWindowSize(window_, &win_w, &win_h);
        const double scale = std::min(static_cast<double>(out_w) / width, static_cast<double>(out_h) / height);
        const SDL_Rect target = {static_cast<int>((out_w - width * scale) / 2), static_cast<int>((out_h - height * scale) / 2),
                                 static_cast<int>(width * scale), static_cast<int>(height * scale)};
        const double density = win_w > 0 ? static_cast<double>(out_w) / win_w : 1.0;
        view_scale_ = scale / density;
        view_x_ = target.x / density;
        view_y_ = target.y / density;
        if (view_scale_ != cursor_scale_ || density != cursor_density_) {
            // A new scale: the cursors are rebuilt, the one shown first.
            std::vector<SDL_Cursor*> old(cursors_.size(), nullptr);
            old.swap(cursors_);
            cursor_scale_ = view_scale_;
            cursor_density_ = density;
            apply_cursor();
            for (SDL_Cursor* c : old) CursorApi::get().destroy(c);
        }
        const int factor = static_cast<int>(std::ceil(scale - 1e-6));
        SDL_RenderClear(renderer_);
        if (factor <= 1 || std::abs(scale - factor) < 1e-6) {
            SDL_RenderCopy(renderer_, texture_, nullptr, &target);
        } else {
            if (!upscaled_ || upscaled_factor_ != factor) {
                if (upscaled_) SDL_DestroyTexture(upscaled_);
                upscaled_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_TARGET, width * factor,
                                              height * factor);
                if (!upscaled_) throw std::runtime_error(SDL_GetError());
                SDL_SetTextureScaleMode(upscaled_, SDL_ScaleModeLinear);
                upscaled_factor_ = factor;
            }
            SDL_SetRenderTarget(renderer_, upscaled_);
            SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
            SDL_SetRenderTarget(renderer_, nullptr);
            SDL_RenderCopy(renderer_, upscaled_, nullptr, &target);
        }
        if (!window_shot_path.empty() && (presents_ % 30) == 0) {
            if (SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, out_w, out_h, 24, SDL_PIXELFORMAT_RGB24)) {
                if (SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_RGB24, shot->pixels, shot->pitch) == 0)
                    SDL_SaveBMP(shot, window_shot_path.c_str());
                SDL_FreeSurface(shot);
            }
        }
        const uint64_t before_vsync = SDL_GetPerformanceCounter();
        SDL_RenderPresent(renderer_);
        pace.vsync_ms_max = std::max(pace.vsync_ms_max, 1000.0 * static_cast<double>(SDL_GetPerformanceCounter() - before_vsync) /
                                                            static_cast<double>(SDL_GetPerformanceFrequency()));
    }
    ++presents_;

    if (!screenshot_path.empty() || screenshot_wanted_) {
        last_frame_.resize(static_cast<size_t>(width) * height * 3);
        for (size_t i = 0; i < frame_.size(); ++i) {
            last_frame_[3 * i] = static_cast<uint8_t>(frame_[i] >> 16);
            last_frame_[3 * i + 1] = static_cast<uint8_t>(frame_[i] >> 8);
            last_frame_[3 * i + 2] = static_cast<uint8_t>(frame_[i]);
        }
    }
    if (max_presents && presents_ >= max_presents) stop_ = true;
    const double took = 1000.0 * static_cast<double>(SDL_GetPerformanceCounter() - started) / static_cast<double>(SDL_GetPerformanceFrequency());
    pace.present_ms += took;
    pace.present_ms_max = std::max(pace.present_ms_max, took);
}

namespace {

// A 5 x 7 font for the overlay: digits, capitals and a few signs, one string of 35 cells each.
struct Glyph {
    char c;
    const char* rows;
};
constexpr Glyph kGlyphs[] = {
    {'A', " ### #   ##   #######   ##   ##   #"}, {'B', "#### #   ##   ##### #   ##   ##### "},
    {'C', " ### #   ##    #    #    #   # ### "}, {'D', "#### #   ##   ##   ##   ##   ##### "},
    {'E', "######    #    #### #    #    #####"}, {'F', "######    #    #### #    #    #    "},
    {'G', " ### #   ##    # ####   ##   # ####"}, {'H', "#   ##   ##   #######   ##   ##   #"},
    {'I', " ###   #    #    #    #    #   ### "}, {'J', "  ###   #    #    #    # #  #  ##  "},
    {'K', "#   ##  # # #  ##   # #  #  # #   #"}, {'L', "#    #    #    #    #    #    #####"},
    {'M', "#   ### ### # ## # ##   ##   ##   #"}, {'N', "#   ###  ## # ##  ###   ##   ##   #"},
    {'O', " ### #   ##   ##   ##   ##   # ### "}, {'P', "#### #   ##   ##### #    #    #    "},
    {'Q', " ### #   ##   ##   ## # ##  #  ## #"}, {'R', "#### #   ##   ##### # #  #  # #   #"},
    {'S', " #####    #     ###     #    ##### "}, {'T', "#####  #    #    #    #    #    #  "},
    {'U', "#   ##   ##   ##   ##   ##   # ### "}, {'V', "#   ##   ##   ##   ##   # # #   #  "},
    {'W', "#   ##   ##   ## # ## # ## # # # # "}, {'X', "#   ##   # # #   #   # # #   ##   #"},
    {'Y', "#   ##   # # #   #    #    #    #  "}, {'Z', "#####    #   #   #   #   #    #####"},
    {'0', " ### #   ##  ### # ###  ##   # ### "}, {'1', "  #   ##    #    #    #    #   ### "},
    {'2', " ### #   #    #   #   #   #   #####"}, {'3', "#####   #   #     #     ##   # ### "},
    {'4', "   #   ##  # # #  # #####   #    # "}, {'5', "######    ####     #    ##   # ### "},
    {'6', "  ##  #   #    #### #   ##   # ### "}, {'7', "#####    #   #   #   #    #    #   "},
    {'8', " ### #   ##   # ### #   ##   # ### "}, {'9', " ### #   ##   # ####    #   #  ##  "},
    {'.', "                          ##   ##  "}, {':', "      ##   ##        ##   ##       "},
    {'%', "##   ##  #   #   #   #   #  ##   ##"}, {'/', "         #   #   #   #   #         "},
    {'(', "   #   #   #    #    #     #     # "}, {')', " #     #     #    #    #   #   #   "},
    {'-', "               #####               "}, {'|', "  #    #    #    #    #    #    #  "},
    {',', "                     ##    #   #   "}, {'=', "          #####     #####          "},
};

}  // namespace

void Desktop::draw_overlay(int width, int height) {
    constexpr int kScale = 2, kCell = 6 * kScale, kLine = 9 * kScale, kMargin = 8;
    std::vector<std::string> lines(1);
    for (char c : overlay_text) {
        if (c == '\n') lines.emplace_back();
        else lines.back().push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    size_t longest = 0;
    for (const auto& l : lines) longest = std::max(longest, l.size());
    const int box_w = static_cast<int>(longest) * kCell + 2 * kMargin, box_h = static_cast<int>(lines.size()) * kLine + 2 * kMargin;
    for (int y = 0; y < std::min(box_h, height); ++y)
        for (int x = 0; x < std::min(box_w, width); ++x) {
            uint32_t& p = frame_[static_cast<size_t>(y) * width + x];
            p = 0xFF000000u | ((p >> 2) & 0x3F3F3F);  // darkened
        }
    for (size_t l = 0; l < lines.size(); ++l)
        for (size_t i = 0; i < lines[l].size(); ++i) {
            const Glyph* g = nullptr;
            for (const Glyph& candidate : kGlyphs)
                if (candidate.c == lines[l][i]) g = &candidate;
            if (!g) continue;
            for (int row = 0; row < 7; ++row)
                for (int col = 0; col < 5; ++col) {
                    if (g->rows[row * 5 + col] != '#') continue;
                    for (int dy = 0; dy < kScale; ++dy)
                        for (int dx = 0; dx < kScale; ++dx) {
                            const int x = kMargin + static_cast<int>(i) * kCell + col * kScale + dx;
                            const int y = kMargin + static_cast<int>(l) * kLine + row * kScale + dy;
                            if (x < width && y < height) frame_[static_cast<size_t>(y) * width + x] = 0xFF7CFC00u;
                        }
                }
        }
}

int Desktop::add_cursor(CursorImage image) {
    cursor_images_.push_back(std::move(image));
    cursors_.push_back(nullptr);
    return static_cast<int>(cursor_images_.size()) - 1;
}

void Desktop::set_cursor(int index) {
    if (index < 0 || index >= static_cast<int>(cursor_images_.size())) index = kSystemCursor;
    if (index == cursor_) return;
    cursor_ = index;
    apply_cursor();
}

// Shows the chosen cursor; it waits for the scale to be known (the first frame shown).
void Desktop::apply_cursor() {
    if (headless || !window_ || cursor_scale_ <= 0) return;
    SDL_Cursor* cursor = nullptr;
    if (cursor_ != kSystemCursor) {
        SDL_Cursor*& made = cursors_[static_cast<size_t>(cursor_)];
        if (!made) made = make_cursor(cursor_images_[static_cast<size_t>(cursor_)]);
        cursor = made;
    }
    CursorApi::get().set(cursor);  // null: the system's arrow
}

// A cursor at the scale of the game's picture: cursor_scale_ points per game pixel, and one image
// per screen density when SDL3 allows it.
SDL_Cursor* Desktop::make_cursor(const CursorImage& image) const {
    const CursorApi& api = CursorApi::get();
    const int w = std::max(1, static_cast<int>(std::lround(image.width * cursor_scale_)));
    const int h = std::max(1, static_cast<int>(std::lround(image.height * cursor_scale_)));
    const int hot_x = std::clamp(static_cast<int>(std::lround(image.hot_x * cursor_scale_)), 0, w - 1);
    const int hot_y = std::clamp(static_cast<int>(std::lround(image.hot_y * cursor_scale_)), 0, h - 1);
    const auto fill = [&image](uint8_t* pixels, int pitch, int width, int height) {
        const std::vector<uint32_t> argb = resample_cursor(image, width, height);
        for (int y = 0; y < height; ++y)
            std::memcpy(pixels + static_cast<size_t>(y) * pitch, &argb[static_cast<size_t>(y) * width], static_cast<size_t>(width) * 4);
    };
    if (api.sdl3) {
        CursorApi::Surface3* surface = api.create_surface(w, h, SDL_PIXELFORMAT_ARGB8888);
        if (!surface) return nullptr;
        fill(static_cast<uint8_t*>(surface->pixels), surface->pitch, w, h);
        const int dense_w = static_cast<int>(std::lround(image.width * cursor_scale_ * cursor_density_));
        const int dense_h = static_cast<int>(std::lround(image.height * cursor_scale_ * cursor_density_));
        if (dense_w > w && dense_h > h) {
            if (CursorApi::Surface3* dense = api.create_surface(dense_w, dense_h, SDL_PIXELFORMAT_ARGB8888)) {
                fill(static_cast<uint8_t*>(dense->pixels), dense->pitch, dense_w, dense_h);
                api.add_alternate_image(surface, dense);
                api.destroy_surface(dense);
            }
        }
        SDL_Cursor* cursor = api.create_color_cursor(surface, hot_x, hot_y);
        api.destroy_surface(surface);
        return cursor;
    }
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return nullptr;
    fill(static_cast<uint8_t*>(surface->pixels), surface->pitch, w, h);
    SDL_Cursor* cursor = SDL_CreateColorCursor(surface, hot_x, hot_y);
    SDL_FreeSurface(surface);
    return cursor;
}

// Position de la souris dans l'image du jeu, à partir de sa position dans la fenêtre.
void Desktop::show_system_cursor(bool shown) {
    system_cursor_shown_ = shown;
    if (!headless) SDL_ShowCursor(shown ? SDL_ENABLE : SDL_DISABLE);
}

void Desktop::warp_mouse(int x, int y) {
    mouse_x = x;
    mouse_y = y;
    if (headless || !window_ || view_scale_ <= 0) return;
    SDL_WarpMouseInWindow(window_, static_cast<int>(view_x_ + (x + 0.5) * view_scale_),
                          static_cast<int>(view_y_ + (y + 0.5) * view_scale_));
}

void Desktop::to_game(int window_x, int window_y) {
    const int x = static_cast<int>((window_x - view_x_) / view_scale_), y = static_cast<int>((window_y - view_y_) / view_scale_);
    mouse_x = std::clamp(x, 0, std::max(texture_w_ - 1, 0));
    mouse_y = std::clamp(y, 0, std::max(texture_h_ - 1, 0));
}

void Desktop::toggle_fullscreen() {
    if (!window_ || headless) return;
    fullscreen = !fullscreen;
    SDL_SetWindowFullscreen(window_, fullscreen ? static_cast<Uint32>(SDL_WINDOW_FULLSCREEN_DESKTOP) : 0u);
    if (!fullscreen) {
        SDL_SetWindowSize(window_, width_, height_);
        SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

void Desktop::show_crash_dialog(const std::string& message, const std::string& report, const std::string& file) {
    if (headless) return;
    if (window_) {
        if (fullscreen) toggle_fullscreen();
        SDL_SetWindowGrab(window_, SDL_FALSE);
    }
    SDL_ShowCursor(SDL_ENABLE);
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Close"},
        {0, 2, "Copy report"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Show in Finder"},
    };
    const SDL_MessageBoxData data{SDL_MESSAGEBOX_ERROR, window_, "Cossacks stopped", message.c_str(),
                                  static_cast<int>(std::size(buttons)), buttons, nullptr};
    int pressed = 0;
    if (SDL_ShowMessageBox(&data, &pressed) != 0) return;
    if (pressed == 2) SDL_SetClipboardText(report.c_str());
#ifdef __APPLE__
    if (pressed == 1 && !file.empty()) {
        char open_tool[] = "open", reveal[] = "-R";
        std::string path = file;
        char* argv[] = {open_tool, reveal, path.data(), nullptr};
        pid_t pid = 0;
        posix_spawnp(&pid, "open", nullptr, nullptr, argv, environ);
    }
#endif
}

std::vector<ScriptEvent> parse_script(const std::string& text) {
    using Kind = ScriptEvent::Kind;
    std::vector<ScriptEvent> script;
    std::stringstream all(text);
    std::string item;
    while (std::getline(all, item, ';')) {
        const size_t colon = item.find(':');
        if (colon == std::string::npos) {
            if (item.find_first_not_of(" \t") == std::string::npos) continue;
            throw std::runtime_error("action sans instant : " + item);
        }
        ScriptEvent e;
        e.at = std::stod(item.substr(0, colon));
        std::stringstream rest(item.substr(colon + 1));
        std::string verb;
        rest >> verb;
        auto point = [&] {
            char comma = 0;
            if (!(rest >> e.x >> comma >> e.y)) throw std::runtime_error("position attendue : " + item);
        };
        auto push = [&](Kind kind, double delay) {
            ScriptEvent copy = e;
            copy.kind = kind;
            copy.at += delay;
            script.push_back(copy);
        };
        if (verb == "move") { point(); push(Kind::Move, 0); }
        else if (verb == "click") { point(); push(Kind::Move, 0); push(Kind::LeftDown, 0.15); push(Kind::LeftUp, 0.3); }
        else if (verb == "rclick") { point(); push(Kind::Move, 0); push(Kind::RightDown, 0.15); push(Kind::RightUp, 0.3); }
        // « down » et « up » séparés : un glissement s'écrit down, move…, up (tracé d'une route).
        // Le jeu lit la case survolée une fois par image : on lui laisse une demi-seconde entre
        // l'arrivée de la souris et le bouton, sinon il part de la case d'avant.
        else if (verb == "down") { point(); push(Kind::Move, 0); push(Kind::LeftDown, 0.5); }
        else if (verb == "up") { point(); push(Kind::Move, 0); push(Kind::LeftUp, 0.5); }
        else if (verb == "wheel") { rest >> e.key; push(Kind::Wheel, 0); }
        else if (verb == "blur") push(Kind::Blur, 0);
        else if (verb == "focus") push(Kind::Focus, 0);
        else if (verb == "key") { rest >> e.key; push(Kind::KeyDown, 0); push(Kind::KeyUp, 0.15); }
        else if (verb == "char") { rest >> e.key; push(Kind::Char, 0); }
        else if (verb == "keydown") { rest >> e.key; push(Kind::KeyDown, 0); }  // a key held down
        else if (verb == "keyup") { rest >> e.key; push(Kind::KeyUp, 0); }
        else if (verb == "scan") { rest >> e.key; push(Kind::ScanDown, 0); push(Kind::ScanUp, 0.15); }
        else if (verb == "scandown") { rest >> e.key; push(Kind::ScanDown, 0); }
        else if (verb == "scanup") { rest >> e.key; push(Kind::ScanUp, 0); }
        else if (verb == "shot") { rest >> e.path; push(Kind::Shot, 0); }
        else throw std::runtime_error("action inconnue : " + item);
    }
    return script;
}

void Desktop::add_script(const std::string& text) {
    const std::vector<ScriptEvent> events = parse_script(text);
    script_.insert(script_.end(), events.begin(), events.end());
    std::stable_sort(script_.begin(), script_.end(), [](const ScriptEvent& a, const ScriptEvent& b) { return a.at < b.at; });
    // L'image RGB24 ne se prépare à chaque image que si le script demande une capture : cette
    // conversion coûtait autant que le reste de la présentation dans toute partie scriptée.
    screenshot_wanted_ = std::any_of(script_.begin(), script_.end(), [](const ScriptEvent& e) { return e.kind == ScriptEvent::Kind::Shot; });
}

void Desktop::alert(const std::string& title, const std::string& text) {
    if (headless) return;
    if (fullscreen) toggle_fullscreen();
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), text.c_str(), window_);
}

void Desktop::save_screenshot() { save_screenshot_to(screenshot_path); }

void Desktop::save_screenshot_to(const std::string& path) {
    const std::string& screenshot_path = path;
    if (screenshot_path.empty() || last_frame_.empty()) return;
    SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormatFrom(last_frame_.data(), texture_w_, texture_h_, 24,
                                                           texture_w_ * 3, SDL_PIXELFORMAT_RGB24);
    if (shot) {
        SDL_SaveBMP(shot, screenshot_path.c_str());
        SDL_FreeSurface(shot);
    }
}

bool Desktop::key_down(int virtual_key) const {
    if (virtual_key == 1) return mouse_buttons_ & MK_LBUTTON;
    if (virtual_key == 2) return mouse_buttons_ & MK_RBUTTON;
    return virtual_key >= 0 && virtual_key < 256 && keys_[static_cast<size_t>(virtual_key)];
}

void Desktop::wait(int milliseconds) {
    SDL_Delay(static_cast<Uint32>(milliseconds));
    pace.idle_ms += milliseconds;
}

// Secondes écoulées depuis le lancement, sur l'horloge réelle ou sur celle du programme.
double Desktop::elapsed(uint32_t now) {
    if (!virtual_time) return (SDL_GetTicks64() - start_ticks_) / 1000.0;
    if (!has_first_now_) {
        first_now_ = now;
        has_first_now_ = true;
    }
    return (now - first_now_) / 1000.0;
}

// Prévient le jeu que sa fenêtre passe à l'arrière-plan ou en revient, avec les messages que
// Windows envoie alors. Inactif, le jeu coupe ses sons et attend au lieu de faire tourner la
// ville (boucle principale, 0x005C37C0).
void Desktop::tell_active(bool active, uint32_t now) {
    auto post = [&](uint32_t message, uint32_t wparam) {
        queue.push_back({hwnd, message, wparam, 0, now, mouse_x, mouse_y});
    };
    if (active) {
        post(WM_ACTIVATEAPP, 1);
        post(WM_ACTIVATE, 1);
        post(WM_SETFOCUS, 0);
    } else {
        post(WM_ACTIVATE, 0);
        post(WM_ACTIVATEAPP, 0);
        post(WM_KILLFOCUS, 0);
    }
    inactive_told_ = !active;
}

bool Desktop::pump(uint32_t now) {
    if (max_seconds > 0 && elapsed(now) >= max_seconds) stop_ = true;
    auto post = [&](uint32_t message, uint32_t wparam, uint32_t lparam) {
        queue.push_back({hwnd, message, wparam, lparam, now, mouse_x, mouse_y});
    };
    auto wheel = [&](int notches) {
        const uint32_t delta = static_cast<uint16_t>(static_cast<int16_t>(notches * kWheelDelta));
        post(WM_MOUSEWHEEL, delta << 16 | mouse_buttons_,
             static_cast<uint32_t>(mouse_y) << 16 | (static_cast<uint32_t>(mouse_x) & 0xFFFF));
    };
    auto mouse = [&](uint32_t message) {
        // Comme Windows, on ne garde en file que le dernier déplacement de souris : sinon les
        // déplacements s'accumulent plus vite que le jeu ne les lit et retardent les clics.
        if (message == WM_MOUSEMOVE && !queue.empty() && queue.back().message == WM_MOUSEMOVE) queue.pop_back();
        post(message, mouse_buttons_, static_cast<uint32_t>(mouse_y) << 16 | (static_cast<uint32_t>(mouse_x) & 0xFFFF));
    };
    // Scénario de diagnostic : mêmes messages que ceux d'un joueur.
    const double since_start = elapsed(now);
    for (; script_next_ < script_.size() && script_[script_next_].at <= since_start; ++script_next_) {
        using Kind = ScriptEvent::Kind;
        const ScriptEvent& s = script_[script_next_];
        const uint32_t vk = static_cast<uint32_t>(s.key);
        switch (s.kind) {
            case Kind::Move: mouse_x = s.x; mouse_y = s.y; mouse(WM_MOUSEMOVE); break;
            case Kind::LeftDown: mouse_buttons_ |= MK_LBUTTON; mouse(WM_LBUTTONDOWN); break;
            case Kind::LeftUp: mouse_buttons_ &= ~MK_LBUTTON; mouse(WM_LBUTTONUP); break;
            case Kind::RightDown: mouse_buttons_ |= MK_RBUTTON; mouse(WM_RBUTTONDOWN); break;
            case Kind::RightUp: mouse_buttons_ &= ~MK_RBUTTON; mouse(WM_RBUTTONUP); break;
            case Kind::Wheel: wheel(s.key); break;
            case Kind::Blur: tell_active(false, now); break;
            case Kind::Focus: tell_active(true, now); break;
            case Kind::KeyDown: if (vk < 256) keys_[vk] = true; post(WM_KEYDOWN, vk, 1); break;
            case Kind::KeyUp: if (vk < 256) keys_[vk] = false; post(WM_KEYUP, vk, 0xC0000001u); break;
            case Kind::Char: post(WM_CHAR, vk, 1); break;
            case Kind::Shot: save_screenshot_to(s.path); break;
            case Kind::ScanDown:
            case Kind::ScanUp: {  // picked up below by SDL_PollEvent, like a real key press
                SDL_Event k{};
                k.type = s.kind == Kind::ScanDown ? SDL_KEYDOWN : SDL_KEYUP;
                k.key.keysym.scancode = static_cast<SDL_Scancode>(s.key);
                k.key.keysym.sym = SDL_GetKeyFromScancode(k.key.keysym.scancode);
                SDL_PushEvent(&k);
                break;
            }
        }
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                post(WM_CLOSE, 0, 0);
                break;
            case SDL_MOUSEMOTION:
                to_game(e.motion.x, e.motion.y);
                mouse(WM_MOUSEMOVE);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                const bool down = e.type == SDL_MOUSEBUTTONDOWN;
                to_game(e.button.x, e.button.y);
                uint32_t mask = 0, message = 0;
                if (e.button.button == SDL_BUTTON_LEFT) {
                    mask = MK_LBUTTON;
                    message = down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
                } else if (e.button.button == SDL_BUTTON_RIGHT) {
                    mask = MK_RBUTTON;
                    message = down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
                } else if (e.button.button == SDL_BUTTON_MIDDLE) {
                    mask = MK_MBUTTON;
                    message = down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
                }
                if (!message) break;
                if (down) mouse_buttons_ |= mask;
                else mouse_buttons_ &= ~mask;
                mouse(message);
                break;
            }
            case SDL_MOUSEWHEEL:
                // La molette fait défiler les listes et les textes du jeu.
                if (e.wheel.y) wheel(e.wheel.y);
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                    for (HeldScroll& held : held_scroll_) held.on = false;  // the release may never come
                if (headless || virtual_time) break;
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    focus_lost_ = true;
                    focus_lost_at_ = SDL_GetTicks64();
                } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    focus_lost_ = false;
                    if (inactive_told_) tell_active(true, now);
                }
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                // Les raccourcis ⌘ sont ceux de l'hôte : le jeu ne les voit pas.
                if (e.key.keysym.mod & KMOD_GUI) {
                    const SDL_Keycode key = e.key.keysym.sym;
                    if (e.type == SDL_KEYDOWN && !e.key.repeat && (key == SDLK_f || key == SDLK_RETURN)) toggle_fullscreen();
                    if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_I) overlay = !overlay;
                    break;
                }
                int vk = virtual_key(e.key.keysym.sym);
                if (!vk) vk = virtual_key_at(e.key.keysym.scancode);
                const bool down = e.type == SDL_KEYDOWN;
                // W A S D (by key position, whatever the keyboard layout): the arrows on the city
                // screens. The choice is made when the key goes down and kept until it is released,
                // so that the game always sees the same key on both sides. With Ctrl or Alt held
                // they stay letters: Ctrl+A is a shortcut of the game (docs/engine/interface.md).
                const SDL_Scancode scan = e.key.keysym.scancode;
                const int wasd = scan == SDL_SCANCODE_W ? 0 : scan == SDL_SCANCODE_A ? 1
                               : scan == SDL_SCANCODE_S ? 2 : scan == SDL_SCANCODE_D ? 3 : -1;
                if (wasd >= 0) {
                    if (down && !e.key.repeat)
                        wasd_arrow_[wasd] = city_screen && city_screen() && !(e.key.keysym.mod & (KMOD_CTRL | KMOD_ALT));
                    static constexpr int kArrows[4] = {0x26, 0x25, 0x28, 0x27};  // up, left, down, right
                    if (wasd_arrow_[wasd]) vk = kArrows[wasd];
                    if (!down) wasd_arrow_[wasd] = false;
                }
                static const bool key_log = std::getenv("COSSACKS_KEYLOG") != nullptr;
                if (key_log)
                    std::fprintf(stderr, "[hôte] touche %s scancode %d sym %#x mod %#x repeat %d carte %d -> vk %#x\n",
                                 down ? "down" : "up", static_cast<int>(scan), static_cast<unsigned>(e.key.keysym.sym),
                                 static_cast<unsigned>(e.key.keysym.mod), e.key.repeat, city_screen && city_screen() ? 1 : 0, vk);
                // P on the map: the game's pause key, Pause (VK_PAUSE), which a Mac keyboard lacks.
                if (scan == SDL_SCANCODE_P) {
                    if (down && !e.key.repeat)
                        p_pause_ = city_screen && city_screen() && !(e.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT));
                    if (p_pause_) vk = 0x13;
                    if (!down) p_pause_ = false;
                }
                // The delete key of a Mac keyboard (Backspace) on the map: the engine's Delete
                // (VK_DELETE), which blows up the selected buildings and kills the selected units
                // (checked); the Mac's own forward delete needs fn. Off the map, and while a text is
                // typed there (no arrow polling then), it stays Backspace.
                if (scan == SDL_SCANCODE_BACKSPACE) {
                    if (down && !e.key.repeat)
                        back_delete_ = city_screen && city_screen() && !(e.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT));
                    if (back_delete_) vk = 0x2E;
                    if (!down) back_delete_ = false;
                }
                if (!vk) break;
                if (vk >= 0x25 && vk <= 0x28) {  // arrows: repeated by the host on the city screens
                    HeldScroll& held = held_scroll_[vk - 0x25];
                    if (down && e.key.repeat && held.on) break;  // replaced by the host's own repeat
                    if (down && !e.key.repeat) {
                        held.on = repeat_held_arrows && city_screen && city_screen();
                        held.next = now + kScrollFirstRepeatMs;
                    }
                    if (!down) held.on = false;
                }
                keys_[static_cast<size_t>(vk)] = down;
                post(down ? WM_KEYDOWN : WM_KEYUP, static_cast<uint32_t>(vk), down ? 1u : 0xC0000001u);
                // Touches de contrôle : Windows envoie aussi un WM_CHAR.
                if (down && (vk == 0x0D || vk == 0x08 || vk == 0x1B || vk == 0x09)) post(WM_CHAR, static_cast<uint32_t>(vk), 1);
                break;
            }
            case SDL_TEXTINPUT:
                if (SDL_GetModState() & KMOD_GUI) break;
                // Le jeu attend du Windows-1252 : « é » arrive comme un seul octet. Un caractère
                // qui n'y existe pas est ignoré.
                // A W A S D key that scrolls the map types nothing.
                if (e.text.text[0] && !e.text.text[1]) {
                    static const char kWasd[] = "wasdWASD";
                    const char* found = std::strchr(kWasd, e.text.text[0]);
                    if (found && wasd_arrow_[(found - kWasd) % 4]) break;
                    if ((e.text.text[0] == 'p' || e.text.text[0] == 'P') && p_pause_) break;
                }
                for (const char* c = e.text.text; *c;) {
                    const unsigned char lead = static_cast<unsigned char>(*c);
                    const size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
                    const std::string one = from_utf8(std::string(c, strnlen(c, length)));
                    if (!one.empty() && (one[0] != '?' || lead == '?')) post(WM_CHAR, static_cast<uint8_t>(one[0]), 1);
                    c += strnlen(c, length);
                }
                break;
            default:
                break;
        }
    }
    // Repeat of the held scrolling keys (HeldScroll): one WM_KEYDOWN at a time, at most one per
    // pass; away from the city screens, the system's repeat takes over again.
    for (int i = 0; i < 4; ++i) {
        HeldScroll& held = held_scroll_[i];
        if (!held.on || static_cast<int32_t>(now - held.next) < 0) continue;
        if (!(city_screen && city_screen())) {
            held.on = false;
            continue;
        }
        post(WM_KEYDOWN, static_cast<uint32_t>(0x25 + i), 0x40000001u);  // bit 30: the key was already down
        held.next = now + kScrollRepeatMs;
    }
    if (focus_lost_ && !inactive_told_ && SDL_GetTicks64() - focus_lost_at_ >= kFocusDelayMs) tell_active(false, now);
    return !stop_;
}

}  // namespace zeus::win32
