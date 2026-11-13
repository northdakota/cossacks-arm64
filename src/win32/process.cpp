#include "win32/process.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace zeus::win32 {
namespace {

// Disposition de la mémoire émulée.
constexpr uint32_t kHeapBase = 0x10000000, kHeapSize = 0x10000000;   // 256 Mo
constexpr uint32_t kStackBase = 0x0FE00000, kStackSize = 0x00200000; // 2 Mo
constexpr uint32_t kTeb = 0x7FFDE000, kPeb = 0x7FFDF000;
constexpr uint32_t kExitAddress = 0xF000FFF0;  // adresse de retour du point d'entrée
// DLLs of the game folder, each at its own 64 KB-aligned base: they all prefer 0x10000000 (the
// heap here) or 0x00400000 (the executable), so they are relocated.
constexpr uint32_t kDllBase = 0x20000000, kDllEnd = 0x30000000;

}  // namespace

Process::Process(std::filesystem::path game_dir, std::filesystem::path write_dir)
    : next_dll_base_(kDllBase), game_dir_(std::move(game_dir)), write_dir_(std::move(write_dir)),
      vfs_(game_dir_, write_dir_, kGuestDir), heap_next_(kHeapBase) {
    cpu_.map(kHeapBase, kHeapSize);
    cpu_.map(kStackBase, kStackSize);
    cpu_.map(kTeb, 0x2000);
    // Bloc d'environnement du fil : chaîne des gestionnaires d'exception, bornes de la pile.
    cpu_.write_u32(kTeb + 0x00, 0xFFFFFFFF);
    cpu_.write_u32(kTeb + 0x04, kStackBase + kStackSize);
    cpu_.write_u32(kTeb + 0x08, kStackBase);
    cpu_.write_u32(kTeb + 0x18, kTeb);
    cpu_.write_u32(kTeb + 0x30, kPeb);
    cpu_.set_fs_base(kTeb);
}

void Process::load(const std::filesystem::path& exe) {
    auto module = std::make_unique<Module>();
    module->image = load_pe(exe, cpu_);
    module->name = exe.filename().string();
    for (char& c : module->name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    module->guest_path = std::string(kGuestDir) + "\\" + exe.filename().string();
    module->references = 1;
    image_ = module->image;
    modules_.push_back(std::move(module));
    cpu_.write_u32(kPeb + 0x08, image_.image_base);
    bind_imports(image_);
}

void Process::preload_dll(const std::filesystem::path& dll) {
    const uint32_t size = pe_image_size(dll);
    if (next_dll_base_ + size > kDllEnd || next_dll_base_ + size < next_dll_base_)
        throw std::runtime_error("zone des DLL pleine : " + dll.string());
    auto module = std::make_unique<Module>();
    module->image = load_pe(dll, cpu_, next_dll_base_);
    next_dll_base_ = (next_dll_base_ + size + 0xFFFF) & ~0xFFFFu;
    module->dll = true;
    std::error_code ignored;
    const std::filesystem::path relative = std::filesystem::relative(dll, game_dir_, ignored);
    std::string windows = relative.empty() ? dll.filename().string() : relative.string();
    std::replace(windows.begin(), windows.end(), '/', '\\');
    module->guest_path = std::string(kGuestDir) + "\\" + windows;
    for (char& c : windows) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    module->name = windows;
    bind_imports(module->image);
    const PeImage& im = module->image;
    module->pristine.assign(cpu_.host(im.image_base), cpu_.host(im.image_base) + ((im.image_size + 0xFFF) & ~0xFFFu));
    modules_.push_back(std::move(module));
}

void Process::def(const std::string& dll, const std::string& name, uint32_t arg_bytes,
                  std::function<uint32_t()> fn) {
    HostFunction f;
    f.name = dll + "!" + name;
    f.arg_bytes = arg_bytes;
    f.call = [this, key = f.name, fn = std::move(fn)](Cpu&) {
        ++call_counts[key];
        if (traced(key)) std::fprintf(stderr, "  appel %s depuis %#x\n", key.c_str(), cpu_.read_u32(cpu_.reg(Cpu::Reg::ESP)));
        if (!break_on.empty() && key == break_on) throw std::runtime_error("arrêt demandé");
        if (!time_calls) return fn();
        const auto start = std::chrono::steady_clock::now();
        const uint32_t result = fn();
        call_times[key] += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        return result;
    };
    functions_[f.name] = std::move(f);
}

uint32_t Process::host_function(const std::string& name, uint32_t arg_bytes, std::function<uint32_t()> fn) {
    HostFunction f;
    f.name = name;
    f.arg_bytes = arg_bytes;
    f.call = [this, name, fn = std::move(fn)](Cpu&) {
        ++call_counts[name];
        if (traced(name)) std::fprintf(stderr, "  appel %s depuis %#x\n", name.c_str(), cpu_.read_u32(cpu_.reg(Cpu::Reg::ESP)));
        return fn();
    };
    return cpu_.add_host_function(std::move(f));
}

Desktop& Process::desktop() {
    if (!desktop_) desktop_ = std::make_unique<Desktop>();
    return *desktop_;
}

Mixer* Process::mixer() {
    if (fixed_clock) return nullptr;
    if (!mixer_ && !mixer_failed_) {
        try {
            mixer_ = std::make_unique<Mixer>();
            mixer_->muted = mute;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[hôte] pas de sortie son : %s\n", e.what());
            mixer_failed_ = true;
        }
    }
    return mixer_.get();
}

uint32_t Process::peek_ticks() const {
    if (fixed_clock) return 100000 + fixed_ticks_;
    using namespace std::chrono;
    return static_cast<uint32_t>(100000 + duration_cast<milliseconds>(steady_clock::now() - start_).count());
}

uint32_t Process::ticks() const {
    if (fixed_clock) return 100000 + ++fixed_ticks_;
    using namespace std::chrono;
    return static_cast<uint32_t>(100000 + duration_cast<milliseconds>(steady_clock::now() - start_).count());
}

void Process::bind_imports(const PeImage& image) {
    for (const PeImport& imp : image.imports) cpu_.write_u32(imp.slot, import_address(imp));
}

// Address that an import slot receives: a function exported by another module of the game
// (a DLL importing from DMCR.EXE), or a host function, one per « dll!function ».
uint32_t Process::import_address(const PeImport& imp) {
    for (const auto& m : modules_) {
        const std::string file = m->name.substr(m->name.find_last_of('\\') == std::string::npos ? 0 : m->name.find_last_of('\\') + 1);
        if (file != imp.dll) continue;
        const auto& ex = m->image;
        if (imp.function[0] == '#') {
            const auto found = ex.exports_by_ordinal.find(static_cast<uint32_t>(std::stoul(imp.function.substr(1))));
            if (found != ex.exports_by_ordinal.end()) return found->second;
        } else if (const auto found = ex.exports.find(imp.function); found != ex.exports.end()) {
            return found->second;
        }
        throw std::runtime_error("import introuvable : " + imp.dll + "!" + imp.function);
    }
    const std::string key = imp.dll + "!" + imp.function;
    if (const auto data = data_imports_.find(key); data != data_imports_.end()) return data->second;
    if (const auto known = import_thunks_.find(key); known != import_thunks_.end()) return known->second;
    {
        auto found = functions_.find(key);
        HostFunction f;
        if (found != functions_.end()) {
            f = found->second;
        } else {
            f.name = key;
            const size_t at = imp.function.rfind('@');
            const bool silent = std::find(silent_dlls_.begin(), silent_dlls_.end(), imp.dll) != silent_dlls_.end();
            if (silent && at != std::string::npos && imp.function[0] == '_') {
                // Son ou vidéo pas encore réalisés : la fonction ne fait rien et renvoie 0.
                f.arg_bytes = static_cast<uint32_t>(std::stoul(imp.function.substr(at + 1)));
                f.call = [this, key, words = f.arg_bytes / 4](Cpu&) -> uint32_t {
                    if (++call_counts[key] == 1) std::fprintf(stderr, "[hôte] %s : sans effet pour l'instant\n", key.c_str());
                    if (traced(key)) {
                        std::fprintf(stderr, "  appel %s(", key.c_str());
                        for (uint32_t i = 0; i < words; ++i) std::fprintf(stderr, i ? ", %#x" : "%#x", arg(static_cast<int>(i)));
                        std::fprintf(stderr, ")\n");
                    }
                    return 0;
                };
            } else {
                // Fonction pas encore écrite : l'appeler arrête le programme en la nommant.
                f.call = [key](Cpu&) -> uint32_t { throw std::runtime_error("fonction système non fournie"); };
            }
        }
        const uint32_t address = cpu_.add_host_function(std::move(f));
        import_thunks_[key] = address;
        return address;
    }
}

uint32_t Process::alloc(uint32_t size, bool zero) {
    const uint32_t rounded = (std::max<uint32_t>(size, 1) + 15) & ~15u;
    uint32_t address = 0;
    auto reuse = free_blocks_.find(rounded);
    if (reuse != free_blocks_.end()) {
        address = reuse->second;
        free_blocks_.erase(reuse);
        if (zero) cpu_.fill(address, 0, rounded);
    } else {
        if (heap_next_ + rounded > kHeapBase + kHeapSize || heap_next_ + rounded < heap_next_) return 0;
        address = heap_next_;  // mémoire jamais utilisée : déjà à zéro
        heap_next_ += rounded;
    }
    blocks_[address] = size;
    return address;
}

void Process::release(uint32_t address) {
    auto found = blocks_.find(address);
    if (found == blocks_.end()) return;
    free_blocks_.emplace((std::max<uint32_t>(found->second, 1) + 15) & ~15u, address);
    blocks_.erase(found);
}

uint32_t Process::alloc_size(uint32_t address) const {
    auto found = blocks_.find(address);
    return found == blocks_.end() ? 0 : found->second;
}

uint32_t Process::alloc_string(const std::string& text) {
    const uint32_t address = alloc(static_cast<uint32_t>(text.size() + 1));
    cpu_.write_string(address, text);
    return address;
}

namespace {

void send_from(Process& p, std::shared_ptr<std::vector<Message>> list, size_t index, uint32_t result) {
    const Message& m = (*list)[index];
    p.cpu().call_guest(p.desktop().wndproc, {m.hwnd, m.message, m.wparam, m.lparam},
                       [&p, list, index, result](uint32_t) {
                           if (index + 1 < list->size()) send_from(p, list, index + 1, result);
                           return result;
                       });
}

}  // namespace

void Process::send_messages(std::vector<Message> messages, uint32_t result) {
    if (messages.empty() || !desktop().wndproc) return;
    send_from(*this, std::make_shared<std::vector<Message>>(std::move(messages)), 0, result);
}

void Process::add_file_patch(FilePatch patch) {
    if (patch.from.size() != patch.to.size()) throw std::runtime_error("correctif de fichier de longueur variable");
    file_patches_.push_back(std::move(patch));
}

void Process::frame_wait() {
    // No frame limiter here: the second clock read in a row happens several times per frame in a
    // battle, and sleeping there costs most of its frames (measured: 12 instead of 60 a second).
    show_ready_frame();
}

void Process::report_pace() {
    static auto since = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double, std::milli>(now - since).count();
    if (elapsed < 1000 || !desktop_) return;
    Desktop::Pace& d = desktop_->pace;
    const double s = elapsed / 1000.0;
    const double idle = std::min(100.0, 100.0 * (d.idle_ms + pace_sleep_ms) / elapsed);
    const double present = std::min(100.0, 100.0 * d.present_ms / elapsed);
    char text[512];
    std::snprintf(text, sizeof text,
                  "GAME %.1f FPS  SHOWN %.1f/S (WAIT %d, FRAME %d, TIMER %d)\n"
                  "PRESENT AVG %.1f MAX %.1f MS, VSYNC MAX %.1f MS, LONGEST GAP %.0f MS\n"
                  "EMULATION %.0f%%  PRESENT %.0f%%  IDLE %.0f%%",
                  pace_frames / s, d.presents / s, pace_shown_on_wait, pace_shown_on_frame, pace_shown_on_timer,
                  d.presents ? d.present_ms / d.presents : 0.0, d.present_ms_max, d.vsync_ms_max, d.gap_ms_max,
                  std::max(0.0, 100.0 - idle - present), present, idle);
    desktop_->overlay_text = text;
    if (pace_log) {
        std::string line = text;
        for (char& c : line)
            if (c == '\n') c = ' ';
        std::fprintf(stderr, "[pace] %s\n", line.c_str());
    }
    d = Desktop::Pace{};
    pace_frames = pace_shown_on_wait = pace_shown_on_frame = pace_shown_on_timer = 0;
    pace_sleep_ms = 0;
    since = now;
}

void Process::exit(int code) {
    exit_code_ = code;
    cpu_.request_stop();
}

int Process::run() {
    uint32_t esp = kStackBase + kStackSize - 0x100;
    esp -= 4;
    cpu_.write_u32(esp, kExitAddress);
    cpu_.set_reg(Cpu::Reg::ESP, esp);
    cpu_.set_reg(Cpu::Reg::EBP, 0);
    cpu_.run(image_.entry_point, kExitAddress);
    return exit_code_;
}

}  // namespace zeus::win32
