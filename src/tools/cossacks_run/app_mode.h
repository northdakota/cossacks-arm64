// Launched as an app (double-click on Cossacks on Mac.app, no arguments): the game's folder is
// found or asked for once and remembered, and the game's data lives in
// ~/Library/Application Support/Cossacks on Mac.
#pragma once

#include <string>

namespace cossacks {

// True when this program runs from inside an .app bundle.
bool in_app_bundle();

// The game's dmcr.exe and the write folder for a launch from the app: the remembered folder, else
// a GOG copy found in a CrossOver bottle, else the player's choice in a dialog. Also sends the
// error output to last-run.log there. False if the player gave up.
bool app_mode_paths(std::string& exe, std::string& write_dir);

// A dialog with an error message (when the program cannot start).
void app_error(const std::string& message);

}  // namespace cossacks
