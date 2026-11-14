// Runs the original Cossacks engine (dmcr.exe) in the emulated x86 processor.
//
//   cossacks-run PATH/dmcr.exe [options]
//     --write-dir DIR          receives everything the game writes (default: run/cossacks); the
//                              installation is never modified
//     --language DIR           a language pack: files that replace the installation's (ALL.GSC…),
//                              and codage.txt, the code page of its texts (1251 for Cyrillic)
//     --trace                  prints every system call
//     --trace-only PREFIX      traces only the functions whose name starts so (ddraw, kernel32.dll!Create…)
//     --break-on FUNCTION      stops the program when it calls this function (dll!name)
//     --deterministic          fixed clock and no sound: reproducible runs
//     --no-native              leaves to the emulator the engine's functions rewritten for the host
//     --counts                 prints at the end the number of calls and time spent per function
//     --headless               hidden window, no sound
//     --mute                   no sound
//     --fullscreen             fullscreen from the start (⌘F or ⌘Enter to toggle)
//     --frames N, --seconds S  stops after N presented frames or S seconds
//     --screenshot OUT.bmp     saves the last presented frame
//     --window-shot OUT.bmp    saves the window contents, scaling included
//     --script "T:ACTION; …"   plays actions at T seconds: click X,Y ; rclick X,Y ; move X,Y ;
//                              down X,Y ; up X,Y ; wheel N ; key CODE ; char CODE ; shot OUT.bmp
//     --profile-from S         the profile counts only from S seconds on (program clock)
//     --frame-log OUT.txt      one line per presented frame: number, size, hash of its pixels
//     --profile OUT.tsv        execution profile of the game code, per block (much slower)
//     --frame-args ADDR        at each pass on ADDR (hexadecimal, inside a function with a
//                              frame), counts the 6 arguments [ebp+8…] of the function
//     --watch-pixel T:X,Y      from T seconds on, lists the instructions that write the byte of
//                              pixel (X,Y) in the engine's drawing buffer (ScreenPtr), and what

#include <array>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <tuple>
#include <vector>
#include <stdexcept>
#include <string>

#include "version.h"
#include "win32/crash_report.h"
#include "win32/process.h"

namespace {

struct Options {
    std::string exe, write_dir = "run/cossacks", language, trace_only, break_on, screenshot, window_shot, script, profile, watch_pixel, frame_args, frame_log;
    double profile_from = 0;
    bool trace = false, headless = false, counts = false, mute = false, fullscreen = false, deterministic = false, native = true;
    int frames = 0;
    double seconds = 0;
};

int run(const Options& o) {
    namespace fs = std::filesystem;
    using namespace zeus::win32;
    Process process(fs::path(o.exe).parent_path(), o.write_dir);
    process.trace = o.trace || !o.trace_only.empty();
    process.trace_only = o.trace_only;
    process.break_on = o.break_on;
    process.time_calls = o.counts;
    Desktop& desktop = process.desktop();
    desktop.headless = o.headless;
    desktop.fullscreen = o.fullscreen;
    desktop.virtual_time = o.deterministic;
    process.fixed_clock = o.deterministic;
    desktop.screenshot_path = o.screenshot;
    desktop.window_shot_path = o.window_shot;
    desktop.frame_log_path = o.frame_log;
    desktop.max_presents = o.frames;
    desktop.max_seconds = o.seconds;
    desktop.add_script(o.script);
    process.mute = o.mute || o.headless;
    process.pace_log = std::getenv("COSSACKS_PACE") != nullptr;  // pacing figures every second
    desktop.overlay = std::getenv("COSSACKS_OVERLAY") != nullptr;  // the technical overlay from the start (⌘I)
    install_kernel32(process);
    install_user32(process);
    install_ddraw(process);
    install_dsound(process);
    install_winmm(process);
    install_system(process);
    process.load(o.exe);
    if (o.native) install_native_functions(process);
    if (!o.language.empty()) process.vfs().add_language_pack(o.language);
    // DLLs the engine loads itself: the AI players, unrar.dll, and the scripts of the missions
    // (Mission.dll of each mission folder, the historical battles, the user's missions).
    const fs::path game = fs::path(o.exe).parent_path();
    std::error_code ignored;
    auto is_dll = [](const fs::path& path) {
        std::string extension = path.extension().string();
        for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return extension == ".dll";
    };
    for (const auto& entry : fs::directory_iterator(game, ignored)) {
        std::string name = entry.path().filename().string();
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name == "ai" && entry.is_directory())
            for (const auto& dll : fs::directory_iterator(entry.path(), ignored))
                if (is_dll(dll.path())) process.preload_dll(dll.path());
        if ((name == "missions" || name == "history_battl" || name == "usermissions") && entry.is_directory()) {
            std::vector<fs::path> dlls;  // in a fixed order, for reproducible addresses
            for (const auto& dll : fs::recursive_directory_iterator(entry.path(), ignored))
                if (dll.is_regular_file() && is_dll(dll.path())) dlls.push_back(dll.path());
            std::sort(dlls.begin(), dlls.end());
            for (const fs::path& dll : dlls) process.preload_dll(dll);
        }
        if (name == "unrar.dll") process.preload_dll(entry.path());
    }
    // W A S D scroll like the arrows where the engine scrolls the map with them: on the battle map,
    // outside a text box. The engine reads the arrows there at every frame (GetKeyState(VK_RIGHT)
    // at 0x4ed81b, skipped while a text is typed: flags 0x8fbb55 and 0x1026234); the host takes
    // "it read them less than 250 ms ago" as the sign. (GameInProgress stays 0 in a battle against
    // the computer.) The bytes are checked first: another build of dmcr.exe gets no W A S D.
    desktop.repeat_held_arrows = false;
    {
        constexpr uint32_t kScrollPoll = 0x4ed81b;
        const uint8_t expected[] = {0x6A, 0x27, 0xFF, 0x15, 0xE8, 0xC1, 0x5C, 0x00};  // push 0x27; call [GetKeyState]
        if (std::memcmp(process.cpu().host(kScrollPoll), expected, sizeof expected) == 0) {
            auto last_poll = std::make_shared<uint32_t>(0);
            process.cpu().add_code_hook(kScrollPoll, [&process, last_poll](Cpu&) { *last_poll = process.peek_ticks(); });
            desktop.city_screen = [&process, last_poll] { return *last_poll && process.peek_ticks() - *last_poll < 250; };
        } else {
            std::fprintf(stderr, "[hôte] dmcr.exe inconnu : pas de W A S D\n");
        }
    }
    // Ends of the engine's frames, for the pacing of the screen (Process::frame_done): after the copy
    // of the whole screen to the primary surface (DialogsSystem::RefreshView, 0x4720d2; every frame
    // of a battle, 26 to 32 per second), after the copy of the changed rectangles (0x4c5a06) and at
    // the end of FlipPages. Each address is checked against its expected bytes.
    {
        struct End {
            uint32_t address;
            std::vector<uint8_t> bytes;
        };
        const End ends[] = {{0x4720d7, {0x83, 0xC4, 0x10, 0xE8}},                       // add esp, 0x10; call
                            {0x4c5a0b, {0xC6, 0x05, 0x1C, 0xCD, 0x71, 0x00, 0x00}},     // mov byte [0x71cd1c], 0
                            {0x45fb27, {0x5F, 0x5E, 0x5B, 0x8B, 0xE5}}};                 // pop edi; pop esi; pop ebx; mov esp, ebp
        for (const End& e : ends)
            if (std::memcmp(process.cpu().host(e.address), e.bytes.data(), e.bytes.size()) == 0)
                process.cpu().add_code_hook(e.address, [&process](Cpu&) { process.frame_done(); });
            else
                std::fprintf(stderr, "[hôte] fin d'image %#x : octets inattendus\n", e.address);
    }
    if (std::getenv("COSSACKS_HEAP_STATS"))  // the heap handed out, every minute of the program's clock
        process.on_poll.push_back([&process, next = std::make_shared<uint32_t>(60000)] {
            if (process.peek_ticks() < *next) return;
            *next += 60000;
            std::fprintf(stderr, "[heap] %u s: %.1f MB handed out  [fpu] control %#06x\n", process.peek_ticks() / 1000,
                         (process.heap_end() - 0x10000000u) / 1048576.0, process.cpu().fpu_control());
        });
    if (!o.profile.empty()) {
        process.cpu().start_profile();
        if (o.profile_from > 0)
            process.on_poll.push_back([&process, from = o.profile_from, done = std::make_shared<bool>(false)] {
                if (*done || process.peek_ticks() - 100000 < from * 1000) return;
                *done = true;
                process.cpu().clear_profile();
                std::fprintf(stderr, "[profile] counting from now on\n");
            });
    }
    auto frame_args = std::make_shared<std::map<std::array<uint32_t, 6>, uint64_t>>();
    if (!o.frame_args.empty())
        process.cpu().add_code_hook(static_cast<uint32_t>(std::stoul(o.frame_args, nullptr, 16)), [frame_args](Cpu& c) {
            std::array<uint32_t, 6> a{};
            for (uint32_t i = 0; i < 6; ++i) a[i] = c.read_u32(c.reg(Cpu::Reg::EBP) + 8 + 4 * i);
            ++(*frame_args)[a];
        });
    // Diagnostics: game frames (calls of FlipPages) and presented frames, per second of real time.
    if (std::getenv("COSSACKS_FPS")) {
        struct Fps {
            uint64_t flips = 0, presents_at_start = 0;
            std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
            double longest_ms = 0;
            std::chrono::steady_clock::time_point last_flip;
        };
        auto fps = std::make_shared<Fps>();
        auto on_frame = [&process, fps](Cpu&) {
            const auto now = std::chrono::steady_clock::now();
            if (fps->flips) fps->longest_ms = std::max(fps->longest_ms, std::chrono::duration<double, std::milli>(now - fps->last_flip).count());
            fps->last_flip = now;
            ++fps->flips;
            const double elapsed = std::chrono::duration<double>(now - fps->since).count();
            if (elapsed >= 1.0) {
                const int presents = process.desktop().presents();
                std::fprintf(stderr, "[fps] %.1f game frames/s, %.1f presented/s, longest gap %.1f ms\n", fps->flips / elapsed,
                             (presents - fps->presents_at_start) / elapsed, fps->longest_ms);
                fps->flips = 0;
                fps->presents_at_start = static_cast<uint64_t>(presents);
                fps->since = now;
                fps->longest_ms = 0;
            }
        };
        if (const char* list = std::getenv("COSSACKS_FPS_AT")) {  // hexadecimal addresses, comma-separated
            for (const char* at = list; *at;) {
                char* end = nullptr;
                process.cpu().add_code_hook(static_cast<uint32_t>(std::strtoul(at, &end, 16)), on_frame);
                at = *end ? end + 1 : end;
            }
        } else {
            process.cpu().add_code_hook(0x45f964, on_frame);  // FlipPages: whole screen
            process.cpu().add_code_hook(0x4c5a0b, on_frame);  // after the copy of the changed rectangles
        }
    }
    // Diagnostics: the two pointers ShowGPLayers gives the blitter for a sprite frame, and their bytes.
    if (const char* want = std::getenv("COSSACKS_GP_DUMP")) {
        unsigned gp = 0, frame = 0;
        std::sscanf(want, "%x:%x", &gp, &frame);
        auto done = std::make_shared<int>(0);
        process.cpu().add_code_hook(0x4808b0, [gp, frame, done](Cpu& c) {
            const uint32_t ebp = c.reg(Cpu::Reg::EBP);
            if (c.read_u32(ebp + 0x10) != gp || c.read_u32(ebp + 0x14) != frame || *done >= 3) return;
            const uint32_t a = c.read_u32(ebp - 0x1c), b = c.read_u32(ebp - 0x14);
            std::fprintf(stderr, "[gp] %#x:%u A=%#x B=%#x\n", gp, frame, a, b);
            char name[64];
            std::snprintf(name, sizeof name, "run/gp_%x_%u_%d.bin", gp, frame, (*done)++);
            if (std::FILE* f = std::fopen(name, "wb")) {
                std::fwrite(c.host(a), 1, 8192, f);
                std::fwrite(c.host(b), 1, 8192, f);
                std::fclose(f);
            }
        });
    }
    // Diagnostics: the LZ decoding of a sprite frame by ShowGPLayers (0x480864 → 0x485070):
    // arguments (output, input, length, dictionary) and the decoded bytes.
    if (const char* want = std::getenv("COSSACKS_LZ_DUMP")) {
        unsigned gp = 0, frame = 0;
        std::sscanf(want, "%x:%x", &gp, &frame);
        auto pending = std::make_shared<std::array<uint32_t, 4>>();
        auto count = std::make_shared<int>(0);
        auto before = [gp, frame, pending, count](Cpu& c) {
            const uint32_t ebp = c.reg(Cpu::Reg::EBP), esp = c.reg(Cpu::Reg::ESP);
            (*pending)[0] = 0;
            if (c.read_u32(ebp + 0x10) != gp || c.read_u32(ebp + 0x14) != frame || *count >= 2) return;
            for (uint32_t i = 0; i < 4; ++i) (*pending)[i] = c.read_u32(esp + 4 * i);
            std::fprintf(stderr, "[lz] frame header %#x, data offset %#x, GP base %#x\n", c.read_u32(ebp - 0x1c),
                         c.read_u32(ebp - 0x4), c.read_u32(ebp - 0x24));
        };
        process.cpu().add_code_hook(0x480864, before);
        process.cpu().add_code_hook(0x4824fd, before);
        auto after = [gp, frame, pending, count](Cpu& c) {
            if (!(*pending)[0]) return;
            const auto& a = *pending;
            std::fprintf(stderr, "[lz] %#x:%u out %#x in %#x length %u dict %#x\n", gp, frame, a[0], a[1], a[2], a[3]);
            char name[64];
            std::snprintf(name, sizeof name, "run/lz_%x_%u_%d.bin", gp, frame, (*count)++);
            if (std::FILE* f = std::fopen(name, "wb")) {
                std::fwrite(c.host(a[0]), 1, a[2] & 0xFFFFF, f);
                std::fclose(f);
            }
            (*pending)[0] = 0;
        };
        process.cpu().add_code_hook(0x480869, after);
        process.cpu().add_code_hook(0x482502, after);
    }
    // Diagnostics: who writes one pixel of the drawing buffer (instruction, value, caller).
    auto writers = std::make_shared<std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint64_t>>();
    if (!o.watch_pixel.empty()) {
        double at = 0;
        int x = 0, y = 0;
        unsigned absolute = 0;
        // « 0xADDRESS »: watched from the start (code already translated by the emulator would
        // not see a watch added later).
        if (std::sscanf(o.watch_pixel.c_str(), "0x%x", &absolute) == 1) {
            if (const char* len = std::getenv("COSSACKS_WATCH_LEN")) {  // a whole range: instructions only
                const uint32_t n = static_cast<uint32_t>(std::strtoul(len, nullptr, 0));
                process.cpu().add_write_hook(absolute, absolute + n, [writers](Cpu& c, uint32_t, int size, uint32_t) {
                    ++(*writers)[{c.reg(Cpu::Reg::EIP), static_cast<uint32_t>(size) << 16, c.read_u32(c.reg(Cpu::Reg::EBP) + 4)}];
                });
            } else
            process.cpu().add_write_hook(absolute - 15, absolute + 1, [writers, absolute](Cpu& c, uint32_t address, int size, uint32_t value) {
                if (absolute < address || absolute >= address + static_cast<uint32_t>(size)) return;
                const uint32_t shift = absolute - address;
                const uint32_t byte = shift < 4 ? (value >> (8 * shift)) & 0xFF : 0x1000 + static_cast<uint32_t>(size);  // value given only for the low 4 bytes
                static const uint32_t arg = std::getenv("COSSACKS_WATCH_ARG") ? static_cast<uint32_t>(std::strtoul(std::getenv("COSSACKS_WATCH_ARG"), nullptr, 16)) : 4;
                ++(*writers)[{c.reg(Cpu::Reg::EIP), byte | static_cast<uint32_t>(size) << 16, c.read_u32(c.reg(Cpu::Reg::EBP) + arg)}];
            });
        } else if (std::sscanf(o.watch_pixel.c_str(), "%lf:%d,%d", &at, &x, &y) != 3) throw std::runtime_error("--watch-pixel T:X,Y");
        auto armed = std::make_shared<bool>(absolute != 0);
        process.on_poll.push_back([&process, armed, writers, at, x, y] {
            if (*armed || process.peek_ticks() - 100000 < at * 1000) return;
            *armed = true;
            Cpu& cpu = process.cpu();
            const auto& ex = process.image().exports;
            auto var = [&](const char* name) {
                const auto f = ex.find(name);
                return f == ex.end() ? 0u : cpu.read_u32(f->second);
            };
            const uint32_t screen = var("?ScreenPtr@@3PAXA"), pitch = var("?SCRSizeX@@3HA");
            std::fprintf(stderr, "[watch] ScreenPtr %#x RealScreenPtr %#x SCRSize %ux%u RSCRSize %ux%u ScrWidth %u ScrHeight %u Pitch %u RealL %ux%u Wind %d,%d\n",
                         screen, var("?RealScreenPtr@@3PAXA"), pitch, var("?SCRSizeY@@3HA"), var("?RSCRSizeX@@3HA"),
                         var("?RSCRSizeY@@3HA"), var("?ScrWidth@@3HA"), var("?ScrHeight@@3HA"), var("?Pitch@@3HA"),
                         var("?RealLx@@3HA"), var("?RealLy@@3HA"), static_cast<int32_t>(var("?WindX@@3HA")),
                         static_cast<int32_t>(var("?WindY@@3HA")));
            const uint32_t target = screen + static_cast<uint32_t>(y) * pitch + static_cast<uint32_t>(x);
            std::fprintf(stderr, "[watch] pixel %d,%d at %#x\n", x, y, target);
            cpu.add_write_hook(target & ~3u, (target & ~3u) + 4, [writers, target](Cpu& c, uint32_t address, int size, uint32_t value) {
                if (target < address || target >= address + static_cast<uint32_t>(size)) return;
                const uint32_t byte = (value >> (8 * (target - address))) & 0xFF;
                ++(*writers)[{c.reg(Cpu::Reg::EIP), byte, c.read_u32(c.reg(Cpu::Reg::EBP) + 4)}];
            });
        });
    }

    int code = 0;
    std::string error;
    try {
        code = process.run();
    } catch (const std::exception& e) {
        error = e.what();
    }
    // Stopped on an error: silence at once, then a report the player can send us.
    if (!error.empty()) {
        process.stop_media();
        const std::string report = crash_report(process, error, COSSACKS_VERSION, desktop.presents());
        const fs::path file = save_crash_report(fs::path(o.write_dir) / "Crash reports", report);
        std::fprintf(stderr, "crash report: %s\n", file.empty() ? "(could not be written)" : file.string().c_str());
        std::string message = "The game stopped on an error the launcher cannot handle yet:\n\n" + error + "\n\n";
        message += file.empty() ? std::string("The report could not be saved; use Copy report.")
                                : "A report was saved as " + file.filename().string() + ", in the folder Crash reports of the "
                                  "game's write folder.";
        message += "\nPlease send it with a few words on what you were doing.";
        desktop.show_crash_dialog(message, report, file.string());
    }
    desktop.save_screenshot();
    if (!o.profile.empty()) {
        std::ofstream out(o.profile);
        for (const auto& [address, block] : process.cpu().profile())
            out << std::hex << address << std::dec << '\t' << block.nanoseconds << '\t' << block.executions << '\t'
                << block.bytes << '\n';
    }
    for (const auto& [key, count] : *frame_args)
        std::fprintf(stderr, "[args] %d %d %#x %#x %#x %#x : %llu\n", static_cast<int32_t>(key[0]), static_cast<int32_t>(key[1]), key[2],
                     key[3], key[4], key[5], static_cast<unsigned long long>(count));
    for (const auto& [key, count] : *writers)
        std::fprintf(stderr, "[watch] eip %#010x size %u value %3u caller %#010x : %llu writes\n", std::get<0>(key), std::get<1>(key) >> 16, std::get<1>(key) & 0xFFFF,
                     std::get<2>(key), static_cast<unsigned long long>(count));
    uint64_t total = 0;
    for (const auto& [name, count] : process.call_counts) total += count;
    if (o.counts)
        for (const auto& [name, count] : process.call_counts) {
            const auto time = process.call_times.find(name);
            std::fprintf(stderr, "%10llu  %8.1f ms  %s\n", static_cast<unsigned long long>(count),
                         time == process.call_times.end() ? 0.0 : time->second / 1e6, name.c_str());
        }
    if (std::getenv("COSSACKS_HEAP_STATS"))
        std::fprintf(stderr, "[heap] %.1f MB of the heap handed out\n", (process.heap_end() - 0x10000000u) / 1048576.0);
    std::fprintf(stderr, "%llu system calls to %zu distinct functions, %d frames presented\n",
                 static_cast<unsigned long long>(total), process.call_counts.size(), desktop.presents());
    if (!error.empty()) {
        std::fprintf(stderr, "stopped: %s\n", error.c_str());
        std::fprintf(stderr, "  eip %#010x  esp %#010x  return address %#010x\n",
                     process.cpu().reg(Cpu::Reg::EIP), process.cpu().reg(Cpu::Reg::ESP),
                     process.cpu().read_u32(process.cpu().reg(Cpu::Reg::ESP)));
        return 1;
    }
    std::fprintf(stderr, "the program ended with code %d\n", code);
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2 || argv[1][0] == '-') {
            std::fprintf(stderr, "usage: cossacks-run PATH/dmcr.exe [options]\n");
            return 2;
        }
        Options o;
        o.exe = argv[1];
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value after " + a);
                return argv[++i];
            };
            if (a == "--trace") o.trace = true;
            else if (a == "--trace-only") o.trace_only = value();
            else if (a == "--break-on") o.break_on = value();
            else if (a == "--headless") o.headless = true;
            else if (a == "--counts") o.counts = true;
            else if (a == "--mute") o.mute = true;
            else if (a == "--fullscreen") o.fullscreen = true;
            else if (a == "--deterministic") o.deterministic = true;
            else if (a == "--no-native") o.native = false;
            else if (a == "--language") o.language = value();
            else if (a == "--write-dir") o.write_dir = value();
            else if (a == "--profile") o.profile = value();
            else if (a == "--profile-from") o.profile_from = std::stod(value());
            else if (a == "--watch-pixel") o.watch_pixel = value();
            else if (a == "--frame-args") o.frame_args = value();
            else if (a == "--frame-log") o.frame_log = value();
            else if (a == "--screenshot") o.screenshot = value();
            else if (a == "--window-shot") o.window_shot = value();
            else if (a == "--script") o.script = value();
            else if (a == "--frames") o.frames = std::stoi(value());
            else if (a == "--seconds") o.seconds = std::stod(value());
            else throw std::runtime_error("unknown option: " + a);
        }
        return run(o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cossacks-run: %s\n", e.what());
        return 1;
    }
}
