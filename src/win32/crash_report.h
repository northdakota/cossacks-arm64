// What the host writes when the program stops on an error (a system function not provided yet, an
// emulation fault…): a text report a player can send us, with no path or name of their own in it.
#pragma once

#include <filesystem>
#include <string>

namespace zeus::win32 {

class Process;

// The report: launcher and game versions, system, settings, the error, the registers and the
// stack (code addresses named by module), the last system calls, the modules in memory.
std::string crash_report(Process& process, const std::string& error, const std::string& launcher_version,
                         int frames_presented);

// Writes it into `folder` (created if needed) as crash-YYYYMMDD-HHMMSS.txt; returns its path,
// empty if it could not be written.
std::filesystem::path save_crash_report(const std::filesystem::path& folder, const std::string& report);

}  // namespace zeus::win32
