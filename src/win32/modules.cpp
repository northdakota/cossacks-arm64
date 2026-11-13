// Modules of the emulated program: the executable and the DLLs of the game folder (AI players,
// missions, unrar.dll), and the module functions of KERNEL32 over them.
//
// Every DLL the game may load is placed in memory before the start (Process::preload_dll), so that
// the page tables built at the first run already know its code. LoadLibraryA only puts the bytes
// back as loaded (fresh data after a FreeLibrary, as Windows rereads the file) and calls DllMain.
// The system DLLs have no image: a handle names them, and GetProcAddress gives the host's
// functions.

#include <cctype>
#include <cstdio>
#include <cstring>

#include "win32/kernel32_state.h"

namespace zeus::win32 {
namespace {

constexpr uint32_t DLL_PROCESS_DETACH = 0, DLL_PROCESS_ATTACH = 1;
constexpr uint32_t kSystemHandle = 0x7F000000, kSystemStride = 0x10000;
constexpr uint32_t ERROR_MOD_NOT_FOUND = 126, ERROR_PROC_NOT_FOUND = 127;

// System DLLs: their handles are kSystemHandle + index * kSystemStride.
const char* const kSystemDlls[] = {"kernel32.dll", "user32.dll",  "gdi32.dll",  "advapi32.dll", "ws2_32.dll",
                                   "wsock32.dll",  "dsound.dll",  "ddraw.dll",  "winmm.dll",    "ole32.dll",
                                   "shell32.dll",  "msvcrt.dll",  "dplayx.dll", "ntdll.dll",    "oleaut32.dll"};
constexpr size_t kSystemCount = sizeof kSystemDlls / sizeof kSystemDlls[0];

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

std::string file_name(const std::string& path) {
    const size_t cut = path.find_last_of("\\/");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

int system_index(const std::string& name) {
    std::string n = lower(file_name(name));
    if (n.find('.') == std::string::npos) n += ".dll";
    for (size_t i = 0; i < kSystemCount; ++i)
        if (n == kSystemDlls[i]) return static_cast<int>(i);
    return -1;
}

}  // namespace

const Module* Process::module_at(uint32_t address) const {
    for (const auto& m : modules_)
        if (address >= m->image.image_base && address - m->image.image_base < m->image.image_size) return m.get();
    return nullptr;
}

// A module of the game folder named by `windows_name` (relative to the current folder or
// absolute), or by its file name alone, whatever its folder, if the name has no folder.
static Module* find_module(const std::vector<std::unique_ptr<Module>>& modules, const Vfs& vfs,
                           const std::string& windows_name) {
    const std::string wanted = lower(trim(windows_name));
    const bool bare = wanted.find_first_of("\\/") == std::string::npos;
    const std::string absolute = lower(vfs.absolute(wanted));
    for (const auto& m : modules) {
        if (lower(m->guest_path) == absolute) return m.get();
    }
    if (bare)
        for (const auto& m : modules)
            if (lower(file_name(m->name)) == wanted || lower(file_name(m->name)) == wanted + ".dll") return m.get();
    return nullptr;
}

uint32_t Process::load_library(const std::string& windows_name) {
    if (const int sys = system_index(trim(windows_name)); sys >= 0 && trim(windows_name).find_first_of("\\/") == std::string::npos)
        return kSystemHandle + static_cast<uint32_t>(sys) * kSystemStride;
    Module* m = find_module(modules_, vfs_, windows_name);
    if (!m) {
        std::fprintf(stderr, "[hôte] LoadLibraryA(%s) : module introuvable\n", windows_name.c_str());
        last_error = ERROR_MOD_NOT_FOUND;
        return 0;
    }
    const uint32_t base = m->image.image_base;
    if (m->references++ > 0 || !m->dll) return base;
    // First load, or a load after the last FreeLibrary: the bytes as loaded, then DllMain.
    std::memcpy(cpu_.host(base), m->pristine.data(), m->pristine.size());
    if (trace) std::fprintf(stderr, "    LoadLibraryA(%s) -> %#x\n", m->name.c_str(), base);
    if (!m->image.entry_point) return base;
    cpu_.call_guest(m->image.entry_point, {base, DLL_PROCESS_ATTACH, 0}, [m, base](uint32_t ok) {
        if (ok) return base;
        std::fprintf(stderr, "[hôte] %s : DllMain a refusé le chargement\n", m->name.c_str());
        m->references = 0;
        return 0u;
    });
    return base;
}

bool Process::free_library(uint32_t handle) {
    if (handle >= kSystemHandle && handle < kSystemHandle + kSystemCount * kSystemStride) return true;
    for (const auto& m : modules_) {
        if (m->image.image_base != handle || !m->dll) continue;
        if (m->references <= 0) return false;
        if (--m->references == 0 && m->image.entry_point) {
            if (trace) std::fprintf(stderr, "    FreeLibrary(%s)\n", m->name.c_str());
            cpu_.call_guest(m->image.entry_point, {handle, DLL_PROCESS_DETACH, 0}, [](uint32_t) { return 1u; });
        }
        return true;
    }
    return false;
}

uint32_t Process::proc_address(uint32_t handle, const std::string& name) {
    if (handle >= kSystemHandle && handle < kSystemHandle + kSystemCount * kSystemStride) {
        const std::string dll = kSystemDlls[(handle - kSystemHandle) / kSystemStride];
        if (!functions_.count(dll + "!" + name)) {
            if (trace) std::fprintf(stderr, "    GetProcAddress(%s, %s) : absente de l'hôte\n", dll.c_str(), name.c_str());
            last_error = ERROR_PROC_NOT_FOUND;
            return 0;
        }
        PeImport imp;
        imp.dll = dll;
        imp.function = name;
        return import_address(imp);
    }
    for (const auto& m : modules_) {
        if (m->image.image_base != handle) continue;
        if (!name.empty() && name[0] == '#') {
            const auto found = m->image.exports_by_ordinal.find(static_cast<uint32_t>(std::stoul(name.substr(1))));
            if (found != m->image.exports_by_ordinal.end()) return found->second;
        } else if (const auto found = m->image.exports.find(name); found != m->image.exports.end()) {
            return found->second;
        }
        last_error = ERROR_PROC_NOT_FOUND;
        return 0;
    }
    last_error = ERROR_MOD_NOT_FOUND;
    return 0;
}

uint32_t Process::module_handle(const std::string& windows_name) const {
    if (const int sys = system_index(windows_name); sys >= 0)
        return kSystemHandle + static_cast<uint32_t>(sys) * kSystemStride;
    const Module* m = find_module(modules_, vfs_, windows_name);
    return m && m->references > 0 ? m->image.image_base : 0u;
}

std::string Process::module_path(uint32_t handle) const {
    if (handle == 0) return modules_.empty() ? std::string() : modules_.front()->guest_path;
    for (const auto& m : modules_)
        if (m->image.image_base == handle) return m->guest_path;
    if (handle >= kSystemHandle && handle < kSystemHandle + kSystemCount * kSystemStride)
        return std::string("C:\\WINDOWS\\SYSTEM32\\") + kSystemDlls[(handle - kSystemHandle) / kSystemStride];
    return {};
}

namespace kernel32 {

void install_modules(Process& p) {
    Cpu& cpu = p.cpu();
    auto def = [&](const char* name, uint32_t args, std::function<uint32_t()> fn) {
        p.def("kernel32.dll", name, args * 4, std::move(fn));
    };
    def("LoadLibraryA", 1, [&p, &cpu] { return p.load_library(cpu.read_string(p.arg(0))); });
    def("FreeLibrary", 1, [&p] { return p.free_library(p.arg(0)) ? 1u : 0u; });
    def("GetProcAddress", 2, [&p, &cpu] {
        const uint32_t name = p.arg(1);
        const std::string wanted = name < 0x10000 ? "#" + std::to_string(name) : cpu.read_string(name);
        const uint32_t address = p.proc_address(p.arg(0), wanted);
        if (p.trace) std::fprintf(stderr, "    GetProcAddress(%#x, %s) -> %#x\n", p.arg(0), wanted.c_str(), address);
        return address;
    });
    def("GetModuleHandleA", 1, [&p, &cpu] {
        return p.arg(0) == 0 ? p.image().image_base : p.module_handle(cpu.read_string(p.arg(0)));
    });
    def("GetModuleFileNameA", 3, [&p, &cpu] {
        const std::string name = p.module_path(p.arg(0) == p.image().image_base ? 0 : p.arg(0));
        const uint32_t size = p.arg(2);
        if (size == 0 || name.empty()) return 0u;
        const std::string cut = name.substr(0, size - 1);
        cpu.write_string(p.arg(1), cut);
        return static_cast<uint32_t>(cut.size());
    });
}

}  // namespace kernel32
}  // namespace zeus::win32
