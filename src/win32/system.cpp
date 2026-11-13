// Smaller system libraries of Cossacks, and the KERNEL32 functions with no state of their own:
//   - ADVAPI32: the registry, kept in memory and in a file of the write folder;
//   - network (WS2_32, DPLAYX, CoCreateInstance of DirectPlay), refused: no multiplayer for now;
//   - ICHAT.DLL, GSC's internet lobby, which the engine imports statically: functions without effect;
//   - MSVCRT, for the one AI module linked against it (AI\Denmark.dll);
//   - formatted output (wsprintfA, sprintf) of the program's own format strings.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

#include "win32/kernel32_state.h"

namespace zeus::win32 {

std::string format_guest(Cpu& cpu, const std::string& format, uint32_t args) {
    std::string out;
    for (size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') {
            out.push_back(format[i]);
            continue;
        }
        // %[flags][width][.precision][length]conversion
        std::string spec = "%";
        size_t j = i + 1;
        while (j < format.size() && std::strchr("-+ #0", format[j])) spec.push_back(format[j++]);
        if (j < format.size() && format[j] == '*') {
            spec += std::to_string(static_cast<int32_t>(cpu.read_u32(args)));
            args += 4;
            ++j;
        }
        while (j < format.size() && std::isdigit(static_cast<unsigned char>(format[j]))) spec.push_back(format[j++]);
        if (j < format.size() && format[j] == '.') {
            spec.push_back(format[j++]);
            if (j < format.size() && format[j] == '*') {
                spec += std::to_string(static_cast<int32_t>(cpu.read_u32(args)));
                args += 4;
                ++j;
            }
            while (j < format.size() && std::isdigit(static_cast<unsigned char>(format[j]))) spec.push_back(format[j++]);
        }
        bool wide64 = false;
        while (j < format.size() && std::strchr("hlLI", format[j])) {
            if (format.compare(j, 3, "I64") == 0) {
                wide64 = true;
                j += 3;
            } else if (format.compare(j, 2, "ll") == 0) {
                wide64 = true;
                j += 2;
            } else {
                ++j;
            }
        }
        if (j >= format.size()) break;
        const char conversion = format[j];
        char buffer[512];
        switch (conversion) {
            case '%': out.push_back('%'); break;
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
                if (wide64) {
                    const uint64_t v = cpu.read_u32(args) | static_cast<uint64_t>(cpu.read_u32(args + 4)) << 32;
                    args += 8;
                    std::snprintf(buffer, sizeof buffer, (spec + "ll" + conversion).c_str(), v);
                } else {
                    const uint32_t v = cpu.read_u32(args);
                    args += 4;
                    if (conversion == 'd' || conversion == 'i')
                        std::snprintf(buffer, sizeof buffer, (spec + conversion).c_str(), static_cast<int32_t>(v));
                    else
                        std::snprintf(buffer, sizeof buffer, (spec + conversion).c_str(), v);
                }
                out += buffer;
                break;
            }
            case 'p':
                std::snprintf(buffer, sizeof buffer, "%08X", cpu.read_u32(args));
                args += 4;
                out += buffer;
                break;
            case 's': case 'S': {
                const uint32_t text = cpu.read_u32(args);
                args += 4;
                const std::string s = text ? cpu.read_string(text) : "(null)";
                std::snprintf(buffer, sizeof buffer, (spec + 's').c_str(), s.c_str());
                out += buffer;
                break;
            }
            case 'f': case 'e': case 'E': case 'g': case 'G': {
                const uint64_t bits = cpu.read_u32(args) | static_cast<uint64_t>(cpu.read_u32(args + 4)) << 32;
                args += 8;
                double v;
                std::memcpy(&v, &bits, 8);
                std::snprintf(buffer, sizeof buffer, (spec + conversion).c_str(), v);
                out += buffer;
                break;
            }
            default:
                out += spec;
                out.push_back(conversion);
        }
        i = j;
    }
    return out;
}

namespace {

constexpr uint32_t ERROR_SUCCESS = 0, ERROR_FILE_NOT_FOUND = 2, ERROR_MORE_DATA = 234;

// The registry: keys by lower-case full path (root\sub\key), values by lower-case name.
struct Registry {
    struct Value {
        uint32_t type = 0;
        std::vector<uint8_t> data;
    };
    std::map<std::string, std::map<std::string, Value>> keys;
    std::map<uint32_t, std::string> open;  // handle -> key path
    uint32_t next_handle = 0x80000100;
    std::filesystem::path file;

    static std::string root(uint32_t handle) {
        switch (handle) {
            case 0x80000000: return "hkcr";
            case 0x80000001: return "hkcu";
            case 0x80000002: return "hklm";
            case 0x80000003: return "hku";
            default: return {};
        }
    }
    std::string path_of(uint32_t parent, const std::string& sub) const {
        std::string base = root(parent);
        if (base.empty()) {
            const auto found = open.find(parent);
            if (found == open.end()) return {};
            base = found->second;
        }
        return sub.empty() ? base : base + "\\" + kernel32::lower(sub);
    }
    // Saved as text: one line per key ("[path]"), then "name=type:hex" per value.
    void load() {
        std::ifstream in(file);
        std::string line, key;
        while (std::getline(in, line)) {
            if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
                key = line.substr(1, line.size() - 2);
                keys[key];
            } else if (const size_t eq = line.rfind('='); eq != std::string::npos && !key.empty()) {
                const size_t colon = line.find(':', eq);
                if (colon == std::string::npos) continue;
                Value v;
                v.type = static_cast<uint32_t>(std::stoul(line.substr(eq + 1, colon - eq - 1)));
                for (size_t k = colon + 1; k + 1 < line.size(); k += 2)
                    v.data.push_back(static_cast<uint8_t>(std::stoul(line.substr(k, 2), nullptr, 16)));
                keys[key][line.substr(0, eq)] = v;
            }
        }
    }
    void save() const {
        std::error_code ignored;
        std::filesystem::create_directories(file.parent_path(), ignored);
        std::ofstream out(file);
        for (const auto& [key, values] : keys) {
            out << '[' << key << "]\n";
            for (const auto& [name, v] : values) {
                out << name << '=' << v.type << ':';
                char hex[3];
                for (uint8_t b : v.data) {
                    std::snprintf(hex, sizeof hex, "%02x", b);
                    out << hex;
                }
                out << '\n';
            }
        }
    }
};

void install_registry(Process& p) {
    Cpu& cpu = p.cpu();
    auto reg = std::make_shared<Registry>();
    reg->file = p.write_dir() / "registry.txt";
    reg->load();
    auto def = [&](const char* name, uint32_t args, std::function<uint32_t()> fn) {
        p.def("advapi32.dll", name, args * 4, std::move(fn));
    };
    auto open_key = [reg](const std::string& path) {
        const uint32_t handle = reg->next_handle++;
        reg->open[handle] = path;
        return handle;
    };
    def("RegOpenKeyExA", 5, [&p, &cpu, reg, open_key] {
        const std::string path = reg->path_of(p.arg(0), p.arg(1) ? cpu.read_string(p.arg(1)) : "");
        if (p.trace) std::fprintf(stderr, "    registre : ouvrir %s\n", path.c_str());
        if (path.empty() || !reg->keys.count(path)) return ERROR_FILE_NOT_FOUND;
        cpu.write_u32(p.arg(4), open_key(path));
        return ERROR_SUCCESS;
    });
    def("RegCreateKeyExA", 9, [&p, &cpu, reg, open_key] {
        const std::string path = reg->path_of(p.arg(0), p.arg(1) ? cpu.read_string(p.arg(1)) : "");
        if (path.empty()) return ERROR_FILE_NOT_FOUND;
        const bool existed = reg->keys.count(path) != 0;
        reg->keys[path];
        cpu.write_u32(p.arg(7), open_key(path));
        if (p.arg(8)) cpu.write_u32(p.arg(8), existed ? 2u : 1u);  // REG_OPENED_EXISTING_KEY / REG_CREATED_NEW_KEY
        if (!existed) reg->save();
        return ERROR_SUCCESS;
    });
    def("RegCloseKey", 1, [&p, reg] {
        reg->open.erase(p.arg(0));
        return ERROR_SUCCESS;
    });
    def("RegQueryValueExA", 6, [&p, &cpu, reg] {
        const auto key = reg->open.find(p.arg(0));
        if (key == reg->open.end()) return 6u;  // ERROR_INVALID_HANDLE
        const std::string name = kernel32::lower(p.arg(1) ? cpu.read_string(p.arg(1)) : "");
        const auto& values = reg->keys[key->second];
        const auto found = values.find(name);
        if (p.trace) std::fprintf(stderr, "    registre : lire %s\\%s%s\n", key->second.c_str(), name.c_str(),
                                  found == values.end() ? " (absente)" : "");
        if (found == values.end()) return ERROR_FILE_NOT_FOUND;
        if (p.arg(3)) cpu.write_u32(p.arg(3), found->second.type);
        const uint32_t size = static_cast<uint32_t>(found->second.data.size());
        if (!p.arg(5)) return ERROR_SUCCESS;
        const uint32_t room = cpu.read_u32(p.arg(5));
        cpu.write_u32(p.arg(5), size);
        if (!p.arg(4)) return ERROR_SUCCESS;
        if (room < size) return ERROR_MORE_DATA;
        if (size) cpu.write(p.arg(4), found->second.data.data(), size);
        return ERROR_SUCCESS;
    });
    def("RegSetValueExA", 6, [&p, &cpu, reg] {
        const auto key = reg->open.find(p.arg(0));
        if (key == reg->open.end()) return 6u;
        Registry::Value v;
        v.type = p.arg(3);
        v.data.resize(p.arg(5));
        if (!v.data.empty()) cpu.read(p.arg(4), v.data.data(), v.data.size());
        reg->keys[key->second][kernel32::lower(p.arg(1) ? cpu.read_string(p.arg(1)) : "")] = std::move(v);
        reg->save();
        return ERROR_SUCCESS;
    });
}

// No network for now: Winsock does not start, DirectPlay is not installed.
void install_network(Process& p) {
    constexpr uint32_t WSASYSNOTREADY = 10091, SOCKET_ERROR = 0xFFFFFFFF;
    auto def = [&](const char* ordinal, uint32_t args, std::function<uint32_t()> fn) {
        p.def("ws2_32.dll", ordinal, args * 4, std::move(fn));
    };
    def("#115", 2, [] { return WSASYSNOTREADY; });  // WSAStartup
    def("#116", 0, [] { return SOCKET_ERROR; });    // WSACleanup
    def("#111", 0, [] { return WSASYSNOTREADY; });  // WSAGetLastError
    def("#57", 2, [&p] {                            // gethostname
        p.cpu().write_string(p.arg(0), "cossacks");
        return 0u;
    });
    // Every other function: an error, as if the network were down.
    const std::pair<const char*, uint32_t> refused[] = {
        {"#1", 3}, {"#2", 3}, {"#3", 1}, {"#4", 3}, {"#6", 3}, {"#7", 5}, {"#10", 3}, {"#11", 1}, {"#13", 2},
        {"#16", 4}, {"#17", 6}, {"#18", 5}, {"#19", 4}, {"#20", 6}, {"#21", 5}, {"#22", 2}, {"#23", 3},
        {"#52", 1}, {"#109", 1}, {"#151", 2}, {"#12", 1}};
    for (const auto& [ordinal, args] : refused) def(ordinal, args, [] { return SOCKET_ERROR; });
    def("#8", 1, [&p] { return __builtin_bswap32(p.arg(0)); });                                      // htonl
    def("#9", 1, [&p] { return static_cast<uint32_t>(__builtin_bswap16(static_cast<uint16_t>(p.arg(0)))); });  // htons
    def("#15", 1, [&p] { return static_cast<uint32_t>(__builtin_bswap16(static_cast<uint16_t>(p.arg(0)))); }); // ntohs
    p.def("dplayx.dll", "#4", 5 * 4, [] { return 0x80004005u; });  // DirectPlayLobbyCreateA: E_FAIL
    p.def("ole32.dll", "CoCreateInstance", 5 * 4, [&p] {
        if (p.arg(4)) p.cpu().write_u32(p.arg(4), 0);
        return 0x80040154u;  // REGDB_E_CLASSNOTREG
    });
}

// ICHAT.DLL: GSC's internet chat and lobby, imported by name by dmcr.exe. All __cdecl.
void install_ichat(Process& p) {
    for (const char* name : {"?ChatProcess@@YAXXZ", "?GetGameID@@YAXPAD@Z", "?GoHomeAnyway@@YAXXZ", "?LeaveGSCRoom@@YAXXZ",
                             "?Process_GSC_ChatWindow@@YAH_NPAURoomInfo@@@Z", "?ReportAliveState@@YAXHPAH@Z",
                             "?ReportGSCGame@@YAXHHPAUOnePlayerReport@@@Z", "?SendPrivateMessage@@YAXPAD0@Z",
                             "?SendVictoryState@@YAXHE@Z", "?StartGSCGame@@YAXPAD0HPAHPAPAD11@Z"})
        p.def("ichat.dll", name, 0, [&p, name] {
            if (p.trace) std::fprintf(stderr, "    iChat %s : sans effet\n", name);
            return 0u;
        });
}

void install_msvcrt(Process& p) {
    Cpu& cpu = p.cpu();
    p.def("msvcrt.dll", "malloc", 0, [&p] { return p.alloc(p.arg(0), false); });
    p.def("msvcrt.dll", "free", 0, [&p] {
        p.release(p.arg(0));
        return 0u;
    });
    p.def("msvcrt.dll", "sprintf", 0, [&p, &cpu] {
        const std::string text = format_guest(cpu, cpu.read_string(p.arg(1)), cpu.reg(Cpu::Reg::ESP) + 12);
        cpu.write_string(p.arg(0), text);
        return static_cast<uint32_t>(text.size());
    });
    // _initterm(begin, end): calls each non-null function pointer of the table, in order.
    p.def("msvcrt.dll", "_initterm", 0, [&p, &cpu] {
        auto table = std::make_shared<std::vector<uint32_t>>();
        for (uint32_t at = p.arg(0); at < p.arg(1); at += 4)
            if (const uint32_t fn = cpu.read_u32(at)) table->push_back(fn);
        auto next = std::make_shared<std::function<uint32_t(size_t)>>();
        *next = [&cpu, table, next](size_t i) -> uint32_t {
            if (i >= table->size()) return 0u;
            cpu.call_guest((*table)[i], {}, [next, i](uint32_t) { return (*next)(i + 1); });
            return 0u;
        };
        return (*next)(0);
    });
    // _adjust_fdiv is a variable (non-zero on a Pentium with the FDIV bug): zero.
    p.def_data("msvcrt.dll", "_adjust_fdiv", p.alloc(4));
}

void install_kernel32_misc(Process& p) {
    Cpu& cpu = p.cpu();
    auto def = [&](const char* name, uint32_t args, std::function<uint32_t()> fn) {
        p.def("kernel32.dll", name, args * 4, std::move(fn));
    };
    auto tls = std::make_shared<std::vector<uint32_t>>();  // 0: free slot, else in use
    constexpr uint32_t kTeb = 0x7FFDE000, kTlsSlots = kTeb + 0xE10;

    def("lstrlenA", 1, [&p, &cpu] { return p.arg(0) ? static_cast<uint32_t>(cpu.read_string(p.arg(0), 1u << 20).size()) : 0u; });
    def("lstrcpyA", 2, [&p, &cpu] {
        cpu.write_string(p.arg(0), cpu.read_string(p.arg(1), 1u << 20));
        return p.arg(0);
    });
    def("MulDiv", 3, [&p] {
        const int64_t d = static_cast<int32_t>(p.arg(2));
        if (d == 0) return 0xFFFFFFFFu;
        const int64_t n = static_cast<int64_t>(static_cast<int32_t>(p.arg(0))) * static_cast<int32_t>(p.arg(1));
        return static_cast<uint32_t>(static_cast<int32_t>((n + (((n < 0) != (d < 0)) ? -d / 2 : d / 2)) / d));
    });
    def("SetLastError", 1, [&p] {
        p.last_error = p.arg(0);
        return 0u;
    });
    def("GetCurrentProcessId", 0, [] { return 0x100u; });
    def("GetCurrentThreadId", 0, [] { return 0x104u; });
    def("GetCurrentThread", 0, [] { return 0xFFFFFFFEu; });
    def("GetComputerNameA", 2, [&p, &cpu] {
        cpu.write_string(p.arg(0), "COSSACKS");
        cpu.write_u32(p.arg(1), 8);
        return 1u;
    });
    def("GetWindowsDirectoryA", 2, [&p, &cpu] {
        cpu.write_string(p.arg(0), "C:\\WINDOWS");
        return 10u;
    });
    def("GetLogicalDriveStringsA", 2, [&p, &cpu] {
        const char drives[] = "C:\\\0";
        if (p.arg(0) < sizeof drives) return static_cast<uint32_t>(sizeof drives);
        cpu.write(p.arg(1), drives, sizeof drives);
        return static_cast<uint32_t>(sizeof drives - 1);
    });
    def("GetDiskFreeSpaceExA", 4, [&p, &cpu] {
        constexpr uint64_t kFree = 8ull << 30, kTotal = 64ull << 30;
        for (int i = 1; i <= 3; ++i) {
            if (!p.arg(i)) continue;
            const uint64_t v = i == 2 ? kTotal : kFree;
            cpu.write_u32(p.arg(i), static_cast<uint32_t>(v));
            cpu.write_u32(p.arg(i) + 4, static_cast<uint32_t>(v >> 32));
        }
        return 1u;
    });
    def("SetConsoleCtrlHandler", 2, [] { return 1u; });
    def("AreFileApisANSI", 0, [] { return 1u; });
    def("IsValidCodePage", 1, [] { return 1u; });
    def("IsValidLocale", 2, [] { return 1u; });
    def("GetUserDefaultLCID", 0, [] { return 0x409u; });
    def("EnumSystemLocalesA", 2, [] { return 1u; });
    def("HeapValidate", 3, [] { return 1u; });
    def("DebugBreak", 0, []() -> uint32_t { throw std::runtime_error("DebugBreak"); });
    def("FatalAppExitA", 2, [&p, &cpu]() -> uint32_t {
        throw std::runtime_error("FatalAppExit : " + (p.arg(1) ? cpu.read_string(p.arg(1)) : std::string()));
    });
    def("RaiseException", 4, [&p]() -> uint32_t {
        char text[64];
        std::snprintf(text, sizeof text, "exception %#x levée par le programme", p.arg(0));
        throw std::runtime_error(text);
    });
    def("CreateProcessA", 10, [&p, &cpu] {
        std::fprintf(stderr, "[hôte] CreateProcessA(%s) refusé\n", p.arg(0) ? cpu.read_string(p.arg(0)).c_str() : "");
        p.last_error = 2;
        return 0u;
    });
    def("WinExec", 2, [&p, &cpu] {
        std::fprintf(stderr, "[hôte] WinExec(%s) refusé\n", cpu.read_string(p.arg(0)).c_str());
        return 2u;  // ERROR_FILE_NOT_FOUND
    });

    // --- critical sections and interlocked counters: a single thread -----------------------
    for (const char* name : {"InitializeCriticalSection", "EnterCriticalSection", "LeaveCriticalSection", "DeleteCriticalSection"})
        def(name, 1, [] { return 0u; });
    def("InterlockedIncrement", 1, [&p, &cpu] {
        const uint32_t v = cpu.read_u32(p.arg(0)) + 1;
        cpu.write_u32(p.arg(0), v);
        return v;
    });
    def("InterlockedDecrement", 1, [&p, &cpu] {
        const uint32_t v = cpu.read_u32(p.arg(0)) - 1;
        cpu.write_u32(p.arg(0), v);
        return v;
    });

    // --- thread-local storage: the 64 slots of the TEB ---------------------------------------
    def("TlsAlloc", 0, [&cpu, tls] {
        for (uint32_t i = 0; i < 64; ++i) {
            if (tls->size() <= i) tls->resize(i + 1, 0);
            if (!(*tls)[i]) {
                (*tls)[i] = 1;
                cpu.write_u32(kTlsSlots + 4 * i, 0);
                return i;
            }
        }
        return 0xFFFFFFFFu;  // TLS_OUT_OF_INDEXES
    });
    def("TlsFree", 1, [&p, tls] {
        if (p.arg(0) < tls->size()) (*tls)[p.arg(0)] = 0;
        return 1u;
    });
    def("TlsGetValue", 1, [&p, &cpu] {
        p.last_error = 0;
        return p.arg(0) < 64 ? cpu.read_u32(kTlsSlots + 4 * p.arg(0)) : 0u;
    });
    def("TlsSetValue", 2, [&p, &cpu] {
        if (p.arg(0) >= 64) return 0u;
        cpu.write_u32(kTlsSlots + 4 * p.arg(0), p.arg(1));
        return 1u;
    });

    // --- global memory: a block of the heap, its own handle ----------------------------------
    def("GlobalAlloc", 2, [&p] { return p.alloc(p.arg(1), (p.arg(0) & 0x40) != 0); });  // GMEM_ZEROINIT
    def("GlobalFree", 1, [&p] {
        p.release(p.arg(0));
        return 0u;
    });
    def("GlobalLock", 1, [&p] { return p.arg(0); });
    def("GlobalUnlock", 1, [] { return 0u; });
    def("GlobalHandle", 1, [&p] { return p.arg(0); });
    def("GlobalMemoryStatus", 1, [&p, &cpu] {
        const uint32_t m = p.arg(0);
        cpu.write_u32(m, 32);
        cpu.write_u32(m + 4, 30);                    // percent in use
        cpu.write_u32(m + 8, 0x7FFFFFFF);            // total physical
        cpu.write_u32(m + 12, 0x40000000);           // available physical
        cpu.write_u32(m + 16, 0x7FFFFFFF);           // total page file
        cpu.write_u32(m + 20, 0x40000000);
        cpu.write_u32(m + 24, 0x7FFE0000);           // total virtual
        cpu.write_u32(m + 28, 0x40000000);
        return 0u;
    });
    def("VirtualQuery", 3, [&p, &cpu] {
        // One region per call, the page of the address, committed and read-write.
        const uint32_t info = p.arg(1), page = p.arg(0) & ~0xFFFu;
        cpu.fill(info, 0, 28);
        cpu.write_u32(info, page);
        cpu.write_u32(info + 4, page);
        cpu.write_u32(info + 8, 4);
        cpu.write_u32(info + 12, 0x1000);
        cpu.write_u32(info + 16, 0x1000);  // MEM_COMMIT
        cpu.write_u32(info + 20, 4);       // PAGE_READWRITE
        cpu.write_u32(info + 24, 0x20000); // MEM_PRIVATE
        return 28u;
    });

    // --- file times: 100 ns since 1601-01-01; local time is UTC (no time zone is announced) ----
    constexpr uint64_t kEpochDelta = 116444736000000000ull;  // 1601 -> 1970, in 100 ns
    auto read_filetime = [&cpu](uint32_t a) { return cpu.read_u32(a) | static_cast<uint64_t>(cpu.read_u32(a + 4)) << 32; };
    auto write_filetime = [&cpu](uint32_t a, uint64_t v) {
        cpu.write_u32(a, static_cast<uint32_t>(v));
        cpu.write_u32(a + 4, static_cast<uint32_t>(v >> 32));
    };
    def("FileTimeToLocalFileTime", 2, [&p, read_filetime, write_filetime] {
        write_filetime(p.arg(1), read_filetime(p.arg(0)));
        return 1u;
    });
    def("LocalFileTimeToFileTime", 2, [&p, read_filetime, write_filetime] {
        write_filetime(p.arg(1), read_filetime(p.arg(0)));
        return 1u;
    });
    def("FileTimeToSystemTime", 2, [&p, &cpu, read_filetime] {
        const uint64_t ft = read_filetime(p.arg(0));
        if (ft < kEpochDelta) return 0u;
        const std::time_t seconds = static_cast<std::time_t>((ft - kEpochDelta) / 10000000);
        std::tm t{};
        gmtime_r(&seconds, &t);
        const uint16_t fields[8] = {static_cast<uint16_t>(t.tm_year + 1900), static_cast<uint16_t>(t.tm_mon + 1),
                                    static_cast<uint16_t>(t.tm_wday), static_cast<uint16_t>(t.tm_mday),
                                    static_cast<uint16_t>(t.tm_hour), static_cast<uint16_t>(t.tm_min),
                                    static_cast<uint16_t>(t.tm_sec), static_cast<uint16_t>((ft / 10000) % 1000)};
        for (int i = 0; i < 8; ++i) cpu.write_u16(p.arg(1) + 2 * static_cast<uint32_t>(i), fields[i]);
        return 1u;
    });
    def("SystemTimeToFileTime", 2, [&p, &cpu, write_filetime] {
        std::tm t{};
        t.tm_year = cpu.read_u16(p.arg(0)) - 1900;
        t.tm_mon = cpu.read_u16(p.arg(0) + 2) - 1;
        t.tm_mday = cpu.read_u16(p.arg(0) + 6);
        t.tm_hour = cpu.read_u16(p.arg(0) + 8);
        t.tm_min = cpu.read_u16(p.arg(0) + 10);
        t.tm_sec = cpu.read_u16(p.arg(0) + 12);
        const int64_t seconds = static_cast<int64_t>(timegm(&t));
        write_filetime(p.arg(1), kEpochDelta + static_cast<uint64_t>(seconds) * 10000000 + cpu.read_u16(p.arg(0) + 14) * 10000ull);
        return 1u;
    });
    def("DosDateTimeToFileTime", 3, [&p, write_filetime] {
        const uint32_t date = p.arg(0) & 0xFFFF, time = p.arg(1) & 0xFFFF;
        std::tm t{};
        t.tm_year = static_cast<int>((date >> 9) + 80);
        t.tm_mon = static_cast<int>(((date >> 5) & 15) - 1);
        t.tm_mday = static_cast<int>(date & 31);
        t.tm_hour = static_cast<int>(time >> 11);
        t.tm_min = static_cast<int>((time >> 5) & 63);
        t.tm_sec = static_cast<int>((time & 31) * 2);
        write_filetime(p.arg(2), kEpochDelta + static_cast<uint64_t>(timegm(&t)) * 10000000);
        return 1u;
    });
    def("GetLocaleInfoA", 4, [&p, &cpu] {
        // Only what the C runtime asks: the ANSI and OEM code pages, as text.
        const uint32_t type = p.arg(1) & 0xFFFF;
        const std::string value = type == 0x1004 ? "1252" : type == 0xB ? "437" : "";
        if (value.empty()) return 0u;
        if (p.arg(3) == 0) return static_cast<uint32_t>(value.size() + 1);
        cpu.write_string(p.arg(2), value);
        return static_cast<uint32_t>(value.size() + 1);
    });
    def("GetLocaleInfoW", 4, [] { return 0u; });
}

}  // namespace

void install_system(Process& p) {
    install_registry(p);
    install_network(p);
    install_ichat(p);
    install_msvcrt(p);
    install_kernel32_misc(p);
}

}  // namespace zeus::win32
