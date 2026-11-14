// Processus Windows minimal autour du code d'origine : mémoire, pile, imports, tas, poignées.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "win32/audio.h"
#include "win32/cpu.h"
#include "win32/desktop.h"
#include "win32/pe_image.h"
#include "win32/vfs.h"

namespace zeus::win32 {

// Chemin sous lequel le programme émulé croit être installé.
inline const char* const kGuestDir = "C:\\Cossacks";

// A PE module of the program: the executable, or a DLL of the game folder placed in memory before
// the start (modules.cpp). A DLL's bytes, relocated and with its imports bound, are kept as loaded
// so that each new LoadLibraryA after its last FreeLibrary gives it fresh data, as Windows does.
struct Module {
    std::string name;        // file name, lower case (ai\\ukraine.dll for a DLL of a sub-folder)
    std::string guest_path;  // C:\\Cossacks\\…
    PeImage image;
    bool dll = false;
    int references = 0;      // LoadLibraryA minus FreeLibrary
    std::vector<uint8_t> pristine;
};

class Process {
public:
    // `game_dir` : installation du jeu, jamais modifiée. `write_dir` : reçoit tout ce que le
    // programme écrit (sauvegardes, réglages) et masque les fichiers de même nom de game_dir.
    Process(std::filesystem::path game_dir, std::filesystem::path write_dir);

    void load(const std::filesystem::path& exe);
    // Places a DLL of the game folder in memory (relocated into the area of DLLs, imports bound),
    // without running it: LoadLibraryA then only calls its DllMain. To call after load().
    void preload_dll(const std::filesystem::path& dll);
    // Module functions of KERNEL32 (modules.cpp).
    uint32_t load_library(const std::string& windows_name);   // 0 if unknown; may call DllMain
    bool free_library(uint32_t handle);
    uint32_t proc_address(uint32_t handle, const std::string& name_or_ordinal);
    uint32_t module_handle(const std::string& windows_name) const;  // 0 if not loaded
    std::string module_path(uint32_t handle) const;            // guest path, empty if unknown
    const Module* module_at(uint32_t address) const;           // module containing this address
    const std::vector<std::unique_ptr<Module>>& modules() const { return modules_; }
    // Exécute depuis le point d'entrée jusqu'à la fin du programme ; renvoie son code de sortie.
    int run();

    Cpu& cpu() { return cpu_; }
    const PeImage& image() const { return image_; }
    const std::filesystem::path& game_dir() const { return game_dir_; }
    const std::filesystem::path& write_dir() const { return write_dir_; }

    // Déclare une fonction système. `arg_bytes` : octets d'arguments dépilés (stdcall).
    void def(const std::string& dll, const std::string& name, uint32_t arg_bytes,
             std::function<uint32_t()> fn);
    // An imported variable rather than a function (msvcrt!_adjust_fdiv): its slot gets `address`.
    void def_data(const std::string& dll, const std::string& name, uint32_t address) {
        data_imports_[dll + "!" + name] = address;
    }
    uint32_t arg(int n) const { return cpu_.arg(n); }
    // Fonction hôte hors table d'import (méthode d'un objet COM, par exemple) : renvoie
    // l'adresse émulée à placer dans une table de pointeurs de fonctions.
    uint32_t host_function(const std::string& name, uint32_t arg_bytes, std::function<uint32_t()> fn);

    // Bibliothèque dont les fonctions non écrites, de nom décoré « _Nom@N », renvoient 0 au lieu
    // d'arrêter le programme (son et vidéo, en attendant leur réalisation).
    void allow_silent_stubs(const std::string& dll) { silent_dlls_.push_back(dll); }

    Vfs& vfs() { return vfs_; }
    // Remplacement d'octets, de même longueur, dans le contenu d'un fichier de l'installation tel
    // que le programme le lit (le fichier lui-même n'est pas touché). `file` : nom sans dossier,
    // en minuscules.
    struct FilePatch {
        std::string file, from, to;
    };
    void add_file_patch(FilePatch patch);
    const std::vector<FilePatch>& file_patches() const { return file_patches_; }
    // Fenêtre et écran, créés à la première utilisation.
    Desktop& desktop();
    // Sortie son, ouverte à la première utilisation ; nul si elle n'a pas pu l'être.
    Mixer* mixer();
    // Millisecondes écoulées, base commune de GetTickCount et timeGetTime.
    uint32_t ticks() const;
    // The same clock as ticks(), read without advancing it (the fixed clock advances at every call of ticks()).
    uint32_t peek_ticks() const;

    // Tas du programme émulé.
    uint32_t alloc(uint32_t size, bool zero = true);
    void release(uint32_t address);
    // HeapReAlloc: grows or shrinks the block in place when it can (in place only: 0 otherwise),
    // else moves it; with `zero`, the bytes added are zero. 0 if the heap is full.
    uint32_t realloc(uint32_t address, uint32_t size, bool zero, bool in_place_only = false);
    uint32_t alloc_size(uint32_t address) const;  // 0 si l'adresse n'est pas un bloc alloué
    uint32_t heap_end() const { return heap_next_; }  // fin de la partie du tas déjà distribuée
    // Copie une chaîne dans le tas et renvoie son adresse.
    uint32_t alloc_string(const std::string& text);

    void exit(int code);

    // Envoie ces messages à la procédure de fenêtre, l'un après l'autre, avant que la fonction
    // système en cours ne rende la main au programme avec la valeur `result`.
    void send_messages(std::vector<Message> messages, uint32_t result);

    // The screen as the program last wrote it: Cossacks keeps a pointer into the primary surface
    // and writes there directly, without Unlock; on Windows that memory is the video memory (and
    // GSC's MDRAW.DLL presents it from a thread of its own). The host shows it at about 60 Hz,
    // from the calls the program makes anyway (messages, clock).
    std::function<void()> refresh_screen;
    // Called whenever the program reads its messages (PeekMessageA, GetMessageA): media that end
    // on their own (CD music) post their notifications from here.
    std::vector<std::function<void()>> on_poll;
    uint32_t last_refresh = 0;
    // Pacing by the game's own frames, when the host knows where they end (frame_done(), called
    // from a hook of the game): the finished frame is shown once, when the game starts waiting for
    // the next one (show_ready_frame(), from its clock loop), so that a frame shows whole, each one
    // for the same time, and the wait for the screen falls in the game's own idle time. The
    // periodic refresh then only covers what does not go through those frames.
    bool knows_frames = false, frame_ready = false;
    // A new frame is finished. If the last one was never shown (the game had no time to wait for
    // its next frame), the screen is shown now: the game is behind and does not wait.
    void frame_done() {
        knows_frames = true;
        ++pace_frames;
        // Not faster than a 120 Hz screen: menus finish over a hundred frames a second.
        if (frame_ready && refresh_screen && peek_ticks() - last_refresh >= 8) {
            ++pace_shown_on_frame;
            refresh_screen();
        }
        frame_ready = true;
    }
    // The game starts waiting for its next frame (from its clock loop): the finished frame shows,
    // at most 120 times a second.
    void frame_wait();
    void show_ready_frame() {
        if (!frame_ready || !refresh_screen || peek_ticks() - last_refresh < 8) return;
        frame_ready = false;
        ++pace_shown_on_wait;
        refresh_screen();
    }
    void maybe_refresh() {
        report_pace();
        if (refresh_screen && peek_ticks() - last_refresh >= (knows_frames ? 100u : 16u)) {
            ++pace_shown_on_timer;
            refresh_screen();
        }
    }
    // Pacing figures, once a second: on the overlay (⌘I), and on the error output if pace_log.
    bool pace_log = false;
    int pace_frames = 0, pace_shown_on_wait = 0, pace_shown_on_frame = 0, pace_shown_on_timer = 0;
    double pace_sleep_ms = 0;  // the host's own sleeps in the game's clock loop
    void report_pace();

    uint32_t last_error = 0;
    bool trace = false;
    bool mute = false;  // son mélangé mais inaudible
    // Horloge fictive : chaque lecture de l'heure avance d'une milliseconde, rien n'attend et le
    // son est coupé. Deux exécutions du même scénario donnent alors les mêmes images, ce qui
    // permet de comparer deux versions de l'hôte image par image.
    bool fixed_clock = false;
    void advance_clock(uint32_t milliseconds) const { fixed_ticks_ += milliseconds; }
    std::string break_on;    // diagnostic : arrête le programme à l'appel de cette fonction
    std::string trace_only;  // si non vide : ne trace que les fonctions dont le nom commence ainsi
    bool traced(const std::string& name) const {
        return trace && (trace_only.empty() || name.compare(0, trace_only.size(), trace_only) == 0);
    }
    std::map<std::string, uint64_t> call_counts;
    // The last system calls (function, return address), for the crash report: names are numbered
    // once, when declared; each call costs one store.
    struct RecentCall {
        uint32_t name = 0, caller = 0;
    };
    static constexpr size_t kRecentCalls = 64;
    std::vector<std::string> call_names;
    std::array<RecentCall, kRecentCalls> recent_calls{};
    uint64_t recent_calls_total = 0;
    void note_call(uint32_t name) {
        recent_calls[recent_calls_total++ % kRecentCalls] = {name, cpu_.read_u32(cpu_.reg(Cpu::Reg::ESP))};
    }
    // Silences the sound at once (the crash report path: nothing should keep playing).
    void stop_media() { mixer_.reset(); }
    // Temps passé dans chaque fonction système, en nanosecondes (mesuré si `time_calls`).
    bool time_calls = false;
    std::map<std::string, uint64_t> call_times;

private:
    void bind_imports(const PeImage& image);
    uint32_t import_address(const PeImport& imp);
    std::vector<std::unique_ptr<Module>> modules_;   // the executable first
    std::map<std::string, uint32_t> import_thunks_;  // « dll!fonction » -> host function address
    std::map<std::string, uint32_t> data_imports_;   // « dll!variable » -> address
    uint32_t next_dll_base_;

    std::filesystem::path game_dir_, write_dir_;
    mutable Cpu cpu_;
    Vfs vfs_;
    std::vector<std::string> silent_dlls_;
    std::vector<FilePatch> file_patches_;
    PeImage image_;
    std::map<std::string, HostFunction> functions_;  // clé « dll!fonction », dll en minuscules
    std::map<uint32_t, uint32_t> blocks_;            // adresse -> taille
    // Free blocks, by address (to merge neighbours) and by capacity (best fit). Capacities are
    // sizes rounded up to 16 bytes. Memory below heap_high_ has been used and may not be zero.
    std::map<uint32_t, uint32_t> free_by_address_;     // adresse -> capacité
    std::multimap<uint32_t, uint32_t> free_by_size_;   // capacité -> adresse
    uint32_t heap_high_ = 0;
    void add_free(uint32_t address, uint32_t capacity);
    void remove_free(uint32_t address, uint32_t capacity);
    uint32_t heap_next_ = 0;
    std::unique_ptr<Desktop> desktop_;
    std::unique_ptr<Mixer> mixer_;
    bool mixer_failed_ = false;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    int exit_code_ = 0;
    mutable uint32_t fixed_ticks_ = 0;
};

// Chaque module déclare les fonctions d'une bibliothèque Windows.
void install_kernel32(Process& process);
void install_user32(Process& process);   // et GDI32, OLE32, SHELL32
void install_ddraw(Process& process);    // DirectDrawCreate, also as MDRAW.DLL (GSC's DirectDraw wrapper)
void install_dsound(Process& process);   // DirectSoundCreate
void install_winmm(Process& process);    // WINMM, also as MINMM.DLL (GOG's wrapper): time, mmio, mixer, MCI
void install_system(Process& process);   // ADVAPI32 registry, network refusals, ICHAT.DLL, MSVCRT
// Functions of the engine rewritten for the host (native.cpp); to call after load().
void install_native_functions(Process& process);

// printf-style formatting of a format string of the program, its arguments from `args` on
// (wsprintfA, sprintf).
std::string format_guest(Cpu& cpu, const std::string& format, uint32_t args);

}  // namespace zeus::win32
