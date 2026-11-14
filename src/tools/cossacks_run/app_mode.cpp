#include "tools/cossacks_run/app_mode.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace cossacks {
namespace {

constexpr uintmax_t kGogExeSize = 2249480;  // GOG's dmcr.exe (SHA-256 b4eea674…), the build the launcher knows

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// dmcr.exe in `folder`, whatever its case; empty if there is none.
fs::path find_exe(const fs::path& folder) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec))
        if (entry.is_regular_file(ec) && lower(entry.path().filename().string()) == "dmcr.exe") return entry.path();
    return {};
}

fs::path data_folder() {
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : "/tmp") / "Library" / "Application Support" / "Cossacks on Mac";
}

std::string quoted_for_shell(const std::string& text) {
    std::string out = "'";
    for (char c : text) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

std::string applescript_string(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
}

// Runs a one-line AppleScript and returns what it prints (empty if it failed or was cancelled).
std::string applescript(const std::string& script) {
    const std::string command = "/usr/bin/osascript -e " + quoted_for_shell(script) + " 2>/dev/null";
    std::string out;
    if (FILE* p = popen(command.c_str(), "r")) {
        char buffer[512];
        while (fgets(buffer, sizeof buffer, p)) out += buffer;
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// A GOG copy in a CrossOver bottle (GOG Galaxy's or the installer's usual folders).
fs::path find_in_crossover() {
    std::error_code ec;
    const char* home = std::getenv("HOME");
    if (!home) return {};
    const fs::path bottles = fs::path(home) / "Library" / "Application Support" / "CrossOver" / "Bottles";
    for (const auto& bottle : fs::directory_iterator(bottles, ec)) {
        const fs::path c = bottle.path() / "drive_c";
        for (const fs::path& games : {c / "GOG Games", c / "Program Files (x86)" / "GOG Galaxy" / "Games",
                                      c / "Program Files" / "GOG Galaxy" / "Games"})
            for (const auto& game : fs::directory_iterator(games, ec)) {
                const fs::path exe = find_exe(game.path());
                if (!exe.empty() && fs::file_size(exe, ec) == kGogExeSize) return exe;
            }
    }
    return {};
}

}  // namespace

bool in_app_bundle() {
#ifdef __APPLE__
    char path[4096];
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0) return false;
    return std::string(path).find(".app/Contents/MacOS/") != std::string::npos;
#else
    return false;
#endif
}

void app_error(const std::string& message) {
    applescript("display dialog " + applescript_string(message) +
                " with title \"Cossacks on Mac\" buttons {\"OK\"} default button 1 with icon stop");
}

bool app_mode_paths(std::string& exe, std::string& write_dir) {
    const fs::path data = data_folder();
    std::error_code ec;
    fs::create_directories(data, ec);
    write_dir = data.string();
    std::freopen((data / "last-run.log").string().c_str(), "w", stderr);

    const fs::path remembered = data / "game-folder.txt";
    std::string folder;
    {
        std::ifstream in(remembered);
        std::getline(in, folder);
    }
    fs::path found = folder.empty() ? fs::path() : find_exe(folder);
    if (found.empty()) found = find_in_crossover();
    while (found.empty()) {
        const std::string chosen = applescript(
            "POSIX path of (choose folder with prompt " +
            applescript_string("Choose the folder of Cossacks: Back to War (GOG edition), the one with dmcr.exe.") + ")");
        if (chosen.empty()) return false;  // cancelled
        found = find_exe(chosen);
        if (found.empty())
            app_error("There is no dmcr.exe in that folder. Choose the folder where Cossacks: Back to War is "
                      "installed (with GOG Galaxy in CrossOver, for instance), or where its GOG installer was "
                      "unpacked with innoextract (the folder app).");
        else if (fs::file_size(found, ec) != kGogExeSize)
            app_error("This dmcr.exe is not the GOG edition's: the game may run without some of the launcher's "
                      "features (W A S D, smooth frames, speed-ups).");
    }
    std::ofstream(remembered) << found.parent_path().string() << '\n';
    exe = found.string();
    return true;
}

}  // namespace cossacks
