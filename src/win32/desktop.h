// Fenêtre, file de messages et écran du programme émulé, réalisés avec SDL.
#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Cursor;

namespace zeus::win32 {

// Message Windows en attente de lecture par le programme.
struct Message {
    uint32_t hwnd = 0, message = 0, wparam = 0, lparam = 0, time = 0;
    int32_t x = 0, y = 0;
};

// Action de diagnostic déclenchée à un instant donné, à la place du joueur.
struct ScriptEvent {
    enum class Kind { Move, LeftDown, LeftUp, RightDown, RightUp, Wheel, KeyDown, KeyUp, Char, Blur, Focus, Shot,
                      ScanDown, ScanUp };
    double at = 0;  // secondes depuis le lancement
    Kind kind = Kind::Move;
    int x = 0, y = 0, key = 0;  // key : touche, caractère ou crans de molette (ScanDown, ScanUp: SDL scancode)
    std::string path;  // pour Shot : fichier BMP à écrire
};

// A cursor of the program (a CURSOR resource of the executable), decoded: non-premultiplied
// ARGB8888, rows from top to bottom; hot spot in pixels of the image.
struct CursorImage {
    int width = 0, height = 0, hot_x = 0, hot_y = 0;
    std::vector<uint32_t> argb;
};

// Actions décrites par un texte de scénario (voir Desktop::add_script), dans l'ordre du texte.
// Une action mal écrite lève std::runtime_error.
std::vector<ScriptEvent> parse_script(const std::string& text);

class Desktop {
public:
    Desktop();
    ~Desktop();
    Desktop(const Desktop&) = delete;
    Desktop& operator=(const Desktop&) = delete;

    // Réglages de diagnostic, à fixer avant le lancement du programme.
    bool headless = false;        // fenêtre cachée
    bool fullscreen = false;      // plein écran dès l'ouverture ; ⌘F ou ⌘Entrée pour basculer
    std::string screenshot_path;  // enregistre la dernière image affichée
    std::string window_shot_path; // enregistre le contenu de la fenêtre, mise à l'échelle comprise
    int max_presents = 0;         // arrête le programme après ce nombre d'images (0 = jamais)
    double max_seconds = 0;       // arrête le programme après cette durée (0 = jamais)
    bool virtual_time = false;    // durées et scénario comptés sur l'horloge du programme
    std::string frame_log_path;   // une ligne par image affichée : son empreinte
    // Ajoute au scénario les actions décrites par `text`, de la forme
    // « 5:click 400,300; 7:rclick 10,10; 8:move 5,5; 9:key 27; 9.5:char 65; 10:shot f.bmp ».
    // Autres actions : down X,Y et up X,Y (glissement), wheel N (crans de molette), blur et
    // focus (fenêtre à l'arrière-plan puis de retour).
    // Also keydown CODE and keyup CODE (a Windows key held down, without repeat), and scan CODE,
    // scandown CODE and scanup CODE (a physical key by SDL scancode, handled by the host like a
    // real key press: W A S D and the host's repeat of held scrolling keys apply).
    void add_script(const std::string& text);

    // Taille de l'écran annoncée au programme tant qu'il n'a pas choisi de mode d'affichage.
    static constexpr int kScreenWidth = 1024, kScreenHeight = 768;

    void create_window(const std::string& title, int width, int height);
    // Change le mode d'affichage : la fenêtre du programme, plein écran, en prend la taille.
    void set_mode(int width, int height);
    // Rapport largeur / hauteur de l'écran du Mac (4/3 s'il est inconnu).
    double screen_aspect() const;
    int width() const { return width_; }
    int height() const { return height_; }

    // Affiche une image 16 bits (5-5-5 ou 5-6-5) de `pitch` octets par ligne.
    void present(const uint8_t* pixels, int width, int height, int pitch, bool rgb565);
    // An 8-bit picture through a palette of 256 ARGB8888 colours, or a 32-bit picture (x8r8g8b8).
    void present8(const uint8_t* pixels, int width, int height, int pitch, const uint32_t* palette);
    void present32(const uint8_t* pixels, int width, int height, int pitch);
    int presents() const { return presents_; }
    double last_present_ms() const { return last_present_ms_; }  // time the last present() took
    void save_screenshot();
    // Boîte de message de l'hôte (sans effet si la fenêtre est cachée).
    void alert(const std::string& title, const std::string& text);
    void save_screenshot_to(const std::string& path);

    // Convertit les événements SDL en messages Windows. Renvoie faux quand il faut s'arrêter.
    bool pump(uint32_t now);
    void wait(int milliseconds);
    std::deque<Message> queue;
    uint32_t hwnd = 0x00010010;  // la seule fenêtre du programme
    uint32_t wndproc = 0;
    int mouse_x = 0, mouse_y = 0;
    bool key_down(int virtual_key) const;
    bool should_stop() const { return stop_; }
    // True on the city screens: W A S D act there as the arrows, and the host repeats held
    // scrolling keys itself. Without an answer, neither happens.
    std::function<bool()> city_screen;
    // Zeus scrolls by one step per WM_KEYDOWN, so the host repeats held arrows itself; Cossacks
    // reads the arrows' state at every frame (GetKeyState) and needs no repeat.
    bool repeat_held_arrows = true;

    // Cursors of the program: add_cursor keeps an image and returns its number; set_cursor shows it
    // at the scale of the game's picture (kSystemCursor: the system's arrow).
    static constexpr int kSystemCursor = -1;
    // Pacing figures since the last reset (Process::report_pace): presents, time they took (and
    // the part spent waiting for the screen in SDL_RenderPresent), longest time between two
    // presents, time spent waiting in wait().
    struct Pace {
        int presents = 0;
        double present_ms = 0, present_ms_max = 0, vsync_ms_max = 0, gap_ms_max = 0, idle_ms = 0;
    };
    Pace pace;
    // Technical overlay (⌘I): lines of text drawn over the picture, set by the host.
    bool overlay = false;
    std::string overlay_text;

    // The system's cursor over the window, shown or hidden (ShowCursor's counter): Cossacks draws
    // its own cursor into the picture and hides the system's.
    void show_system_cursor(bool shown);
    // Moves the system's cursor to a point of the game's picture (SetCursorPos), so that it does
    // not part from the game's own cursor.
    void warp_mouse(int x, int y);
    int add_cursor(CursorImage image);
    void set_cursor(int index);

    // After the program stopped on an error: leaves full screen, gives the cursor back and shows
    // `message` with three buttons: Close, Copy report (puts `report` on the clipboard) and Show in
    // Finder (`file`). Nothing in headless mode.
    void show_crash_dialog(const std::string& message, const std::string& report, const std::string& file);

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    SDL_Texture* upscaled_ = nullptr;  // image agrandie d'un facteur entier, avant lissage
    int upscaled_factor_ = 0;
    // Rectangle de la fenêtre occupé par l'image du jeu, en points de fenêtre.
    double view_x_ = 0, view_y_ = 0, view_scale_ = 1;
    void to_game(int window_x, int window_y);
    void toggle_fullscreen();
    // Cursors: built on demand for the current scale (screen points per game pixel, screen pixels
    // per point), rebuilt when it changes.
    std::vector<CursorImage> cursor_images_;
    std::vector<SDL_Cursor*> cursors_;  // one per image, null until built
    int cursor_ = kSystemCursor;
    bool system_cursor_shown_ = true;
    double cursor_scale_ = 0, cursor_density_ = 0;  // 0: no frame shown yet, scale unknown
    void apply_cursor();
    SDL_Cursor* make_cursor(const CursorImage& image) const;
    bool texture_565_ = false;
    // Common end of every present: `fill` writes row y of the picture in ARGB8888.
    void present_rows(int width, int height, const std::function<void(int, uint32_t*)>& fill);
    void draw_overlay(int width, int height);  // overlay_text into frame_
    uint64_t last_present_counter_ = 0;
    std::vector<uint32_t> frame_;  // the last picture, ARGB8888
    // Couleur ARGB8888 de chaque pixel 16 bits du jeu (555 ou 565) : la texture se remplit en un
    // passage au format du rendu, sans la conversion générale de SDL à chaque image.
    std::vector<uint32_t> colors_;
    int width_ = kScreenWidth, height_ = kScreenHeight, texture_w_ = 0, texture_h_ = 0;
    int presents_ = 0;
    double last_present_ms_ = 0;
    bool stop_ = false, screenshot_wanted_ = false;
    uint32_t mouse_buttons_ = 0;
    std::vector<uint8_t> last_frame_;  // RGB24 de la dernière image, pour la capture
    std::vector<bool> keys_;
    bool wasd_arrow_[4] = {};  // W A S D: arrow chosen when the key goes down, kept until it is released
    bool p_pause_ = false;     // P sent as Pause (on the map), until it is released
    bool back_delete_ = false; // Backspace (a Mac's delete key) sent as Delete (on the map), until released
    // Arrows (and W A S D) held on the city screens: the game takes one scrolling step per
    // WM_KEYDOWN, and the system's repeat starts only after a long delay (a step, a pause, then the
    // scrolling). The host therefore repeats these keys itself, after 120 ms and then every 50 ms
    // (the pace of the game's own scrolling when the mouse is at the edge), and ignores the
    // system's repeat for them.
    struct HeldScroll {
        bool on = false;
        uint32_t next = 0;  // time of the next repeated WM_KEYDOWN
    };
    HeldScroll held_scroll_[4];  // left, up, right and down arrows (0x25 to 0x28)
    uint64_t start_ticks_ = 0;
    uint32_t first_now_ = 0;
    bool has_first_now_ = false;
    double elapsed(uint32_t now);
    std::FILE* frame_log_ = nullptr;
    std::vector<ScriptEvent> script_;  // trié par instant
    size_t script_next_ = 0;
    // Passage de la fenêtre à l'arrière-plan : le jeu en est prévenu après un court délai, pour
    // ne pas réagir aux pertes de focus passagères (bascule en plein écran).
    bool focus_lost_ = false, inactive_told_ = false;
    uint64_t focus_lost_at_ = 0;
    void tell_active(bool active, uint32_t now);
};

}  // namespace zeus::win32
