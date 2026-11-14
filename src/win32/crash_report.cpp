#include "win32/crash_report.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <sys/sysctl.h>
#endif

#include "win32/process.h"

namespace zeus::win32 {
namespace {

std::string hex32(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08x", value);
    return text;
}

// "dmcr.exe+0x1234" for an address inside a module, empty otherwise.
std::string where(const Process& p, uint32_t address) {
    if (address >= 0xF0000000 && address < 0xF0011000) return "host function";  // imports and trampolines
    const Module* m = p.module_at(address);
    if (!m) return {};
    char text[128];
    std::snprintf(text, sizeof text, "%s+0x%x", m->name.c_str(), address - m->image.image_base);
    return text;
}

std::string sysctl_string(const char* name) {
#ifdef __APPLE__
    char value[256] = {};
    size_t size = sizeof value - 1;
    if (sysctlbyname(name, value, &size, nullptr, 0) == 0) return value;
#else
    (void)name;
#endif
    return "unknown";
}

std::string sha256_of(const std::filesystem::path& file) {
#ifdef __APPLE__
    std::ifstream in(file, std::ios::binary);
    if (!in) return "unknown";
    const std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.data(), static_cast<CC_LONG>(data.size()), digest);
    std::string text;
    char byte[3];
    for (unsigned char b : digest) {
        std::snprintf(byte, sizeof byte, "%02x", b);
        text += byte;
    }
    return text;
#else
    (void)file;
    return "unknown";
#endif
}

std::string first_line_of(const std::filesystem::path& file) {
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    return line.empty() ? "(none)" : line;
}

}  // namespace

std::string crash_report(Process& p, const std::string& error, const std::string& launcher_version, int frames_presented) {
    Cpu& cpu = p.cpu();
    std::ostringstream out;
    const std::time_t now = std::time(nullptr);
    char date[64];
    std::strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S %z", std::localtime(&now));

    out << "Cossacks on Mac: crash report\n"
        << "Date: " << date << "\n"
        << "Launcher: " << launcher_version << "\n"
        << "macOS: " << sysctl_string("kern.osproductversion") << ", " << sysctl_string("machdep.cpu.brand_string") << "\n";
    const std::string exe = p.modules().empty() ? std::string("dmcr.exe") : p.modules().front()->name;
    out << "Game: " << exe << " SHA-256 " << sha256_of(p.game_dir() / exe) << "\n"
        << "Settings (mode.dat): " << first_line_of(p.write_dir() / "mode.dat") << "\n"
        << "Program clock: " << p.peek_ticks() / 1000 << " s, frames presented: " << frames_presented << "\n"
        << "Heap: " << (p.heap_end() - 0x10000000u) / 1048576 << " MB of 256 handed out\n\n";

    out << "Error: " << error << "\n\n";

    out << "Registers:\n";
    const std::pair<const char*, Cpu::Reg> regs[] = {{"EIP", Cpu::Reg::EIP}, {"ESP", Cpu::Reg::ESP}, {"EBP", Cpu::Reg::EBP},
                                                     {"EAX", Cpu::Reg::EAX}, {"EBX", Cpu::Reg::EBX}, {"ECX", Cpu::Reg::ECX},
                                                     {"EDX", Cpu::Reg::EDX}, {"ESI", Cpu::Reg::ESI}, {"EDI", Cpu::Reg::EDI}};
    for (const auto& [name, reg] : regs) {
        const uint32_t value = cpu.reg(reg);
        const std::string at = where(p, value);
        out << "  " << name << " " << hex32(value) << (at.empty() ? "" : "  " + at) << "\n";
    }

    char fpu[64];
    std::snprintf(fpu, sizeof fpu, "  FPU control 0x%04x, status 0x%04x\n", cpu.fpu_control(), cpu.fpu_status());
    out << fpu;

    out << "\nStack (from ESP):\n";
    const uint32_t esp = cpu.reg(Cpu::Reg::ESP);
    for (uint32_t i = 0; i < 24; ++i) {
        const uint32_t address = esp + 4 * i;
        if (address < esp) break;  // wrapped around
        const uint32_t value = cpu.read_u32(address);
        const std::string at = where(p, value);
        out << "  [esp+0x" << std::hex << 4 * i << std::dec << "] " << hex32(value) << (at.empty() ? "" : "  " + at) << "\n";
    }

    out << "\nLast system calls (oldest first; the failing one, if it is missing, is not among them):\n";
    const uint64_t total = p.recent_calls_total;
    const uint64_t count = total < Process::kRecentCalls ? total : Process::kRecentCalls;
    for (uint64_t i = total - count; i < total; ++i) {
        const Process::RecentCall& c = p.recent_calls[i % Process::kRecentCalls];
        const std::string name = c.name < p.call_names.size() ? p.call_names[c.name] : "?";
        const std::string at = where(p, c.caller);
        out << "  " << name << " from " << hex32(c.caller) << (at.empty() ? "" : " (" + at + ")") << "\n";
    }

    out << "\nModules in memory:\n";
    for (const auto& m : p.modules()) {
        if (m->dll && m->references <= 0) continue;  // placed in advance, not loaded by the game
        out << "  " << hex32(m->image.image_base) << " size 0x" << std::hex << m->image.image_size << std::dec << "  "
            << m->name << "\n";
    }
    return out.str();
}

std::filesystem::path save_crash_report(const std::filesystem::path& folder, const std::string& report) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::time_t now = std::time(nullptr);
    char name[64];
    std::strftime(name, sizeof name, "crash-%Y%m%d-%H%M%S.txt", std::localtime(&now));
    const std::filesystem::path file = folder / name;
    std::ofstream out(file);
    if (!out) return {};
    out << report;
    return out ? file : std::filesystem::path();
}

}  // namespace zeus::win32
