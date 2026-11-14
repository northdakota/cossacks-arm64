// WINMM, imported by Cossacks through MINMM.DLL (GOG's wrapper, which plays the CD music from
// Tracks\Track_NN.ogg): the clock, mmio (RIFF files: the game reads its WAV sounds with it),
// the mixer and MCI (CD music, AVI videos).
//
// MCI "CDAudio" plays the tracks of the CD from Tracks\Track_NN.ogg (as GOG's MINMM.DLL does),
// decoded by FFmpeg, and tells the game when a track ends (MM_MCINOTIFY): the game then starts the
// next one. The mixer has the CD line (the music's volume) and the wave line, its volumes kept in
// the write folder (mixer.txt). The other MCI devices refuse for now.
// TODO(cossacks): "AVIVideo" (Cinepak) through FFmpeg.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "win32/process.h"

namespace zeus::win32 {
namespace {

constexpr uint32_t MMSYSERR_NOERROR = 0, MMSYSERR_BADDEVICEID = 2, MMIOERR_CHUNKNOTFOUND = 265;
constexpr uint32_t MCIERR_INVALID_DEVICE_ID = 257, MCIERR_CANNOT_LOAD_DRIVER = 266;
constexpr uint32_t MMIO_FINDCHUNK = 0x10, MMIO_FINDRIFF = 0x20, MMIO_FINDLIST = 0x40;
constexpr uint32_t kRiff = 0x46464952, kList = 0x5453494C;  // "RIFF", "LIST"

struct MmioFile {
    std::vector<uint8_t> data;
    uint32_t position = 0;
    uint32_t u32(uint32_t at) const {
        if (at + 4 > data.size()) return 0;
        uint32_t v;
        std::memcpy(&v, &data[at], 4);
        return v;
    }
};

constexpr uint32_t kCdDevice = 1;
constexpr uint32_t MM_MCINOTIFY = 0x3B9, MCI_NOTIFY_SUCCESSFUL = 1;
constexpr uint32_t MCI_OPEN = 0x803, MCI_CLOSE = 0x804, MCI_PLAY = 0x806, MCI_SEEK = 0x807, MCI_STOP = 0x808,
                   MCI_PAUSE = 0x809, MCI_SET = 0x80D, MCI_STATUS = 0x814, MCI_RESUME = 0x855;
constexpr uint32_t MCI_NOTIFY = 0x1, MCI_FROM = 0x4, MCI_TO = 0x8, MCI_SET_TIME_FORMAT = 0x400;
// GOG's MINMM.DLL plays only with MCI_TO; the engine then retries without it, which MINMM ignores.
constexpr uint32_t MCI_FORMAT_TMSF = 10;

constexpr uint32_t kMixer = 0x6000, kVolumeControl = 0x50030001;  // MIXERCONTROL_CONTROLTYPE_VOLUME
constexpr uint32_t kLineCd = 0x1005, kLineWave = 0x1008;  // MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC, _SRC_WAVEOUT
constexpr uint32_t kMixerInvalidLine = 1024, kMixerInvalidControl = 1025;  // MIXERR_INVALLINE, _INVALCONTROL

struct LineInfo {
    uint32_t type;
    const char* name;
};
const LineInfo* line_info(uint32_t type) {
    static const LineInfo lines[] = {{4, "Speakers"}, {kLineCd, "CD Audio"}, {kLineWave, "Wave"}};
    for (const LineInfo& l : lines)
        if (l.type == type) return &l;
    return nullptr;
}

struct State {
    std::map<uint32_t, MmioFile> files;
    // Volume of each mixer line (0 to 65535), by line (component type). The CD line is the music's.
    std::map<uint32_t, uint32_t> volumes = {{4, 65535}, {kLineCd, 49151}, {kLineWave, 65535}};
    uint32_t next = 0x5000;
    // The CD player: one voice of the mixer, the track it plays, the window to tell at its end.
    bool cd_open = false;
    uint32_t time_format = 0;
    int voice = 0, track = 0;
    uint32_t notify_window = 0;  // 0: nobody waits for the end
    bool playing = false;
};

// The mixer's volumes outlive the program on Windows (the system keeps them), and the engine relies
// on it: its music volume is the CD line's, read from the mixer when the Options screen opens
// (0x5748b9: volume × 100 / 65536) and saved nowhere else. They are kept in the write folder.
const std::pair<uint32_t, const char*> kSavedLines[] = {{4, "speakers"}, {kLineCd, "cd"}, {kLineWave, "wave"}};

void load_volumes(State& st, const std::filesystem::path& file) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        for (const auto& [type, name] : kSavedLines)
            if (line.compare(0, eq, name) == 0) {
                char* end = nullptr;
                const unsigned long value = std::strtoul(line.c_str() + eq + 1, &end, 10);
                if (end != line.c_str() + eq + 1) st.volumes[type] = static_cast<uint32_t>(std::min(value, 65535ul));
            }
    }
}

void save_volumes(const State& st, const std::filesystem::path& file) {
    std::ofstream out(file);
    for (const auto& [type, name] : kSavedLines)
        if (const auto found = st.volumes.find(type); found != st.volumes.end()) out << name << '=' << found->second << '\n';
}

int cd_volume(const State& st) {
    const auto found = st.volumes.find(kLineCd);
    return found == st.volumes.end() ? 96 : static_cast<int>(found->second * 127 / 65535);
}

}  // namespace

void install_winmm(Process& p) {
    auto st = std::make_shared<State>();
    Cpu& cpu = p.cpu();
    const std::filesystem::path volumes_file = p.write_dir() / "mixer.txt";
    load_volumes(*st, volumes_file);
    // End of a CD track: the window that asked for it is told (MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL).
    p.on_poll.push_back([&p, st] {
        if (!st->playing) return;
        Mixer* mixer = p.mixer();
        if (!mixer || !st->voice || mixer->state(st->voice) != Mixer::State::Done) return;
        st->playing = false;
        if (st->notify_window) {
            p.desktop().queue.push_back({st->notify_window, MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL, kCdDevice, p.ticks(), 0, 0});
            st->notify_window = 0;
        }
    });
    // MINMM.DLL: GOG's wrapper; CSEMU.DLL: the Steam edition's (mci, mmio and mixer functions).
    for (const char* dll : {"winmm.dll", "minmm.dll", "csemu.dll"}) {
        auto def = [&](const char* name, uint32_t args, std::function<uint32_t()> fn) {
            p.def(dll, name, args * 4, std::move(fn));
        };
        def("timeGetTime", 0, [&p] { return p.ticks(); });

        // --- mmio: a whole file read in memory; MMCKINFO = ckid, cksize, fccType, dwDataOffset, dwFlags
        def("mmioOpenA", 3, [&p, &cpu, st] {
            const std::string name = cpu.read_string(p.arg(0));
            const auto path = p.vfs().find(name);
            if (p.trace) std::fprintf(stderr, "    mmioOpen « %s »%s\n", name.c_str(), path ? "" : " : absent");
            if (!path || (p.arg(2) & 3) != 0) return 0u;  // only MMIO_READ
            std::ifstream in(*path, std::ios::binary);
            MmioFile f;
            f.data.assign(std::istreambuf_iterator<char>(in), {});
            const uint32_t handle = st->next++;
            st->files[handle] = std::move(f);
            return handle;
        });
        def("mmioClose", 2, [&p, st] {
            st->files.erase(p.arg(0));
            return MMSYSERR_NOERROR;
        });
        def("mmioRead", 3, [&p, &cpu, st] {
            const auto found = st->files.find(p.arg(0));
            if (found == st->files.end()) return 0xFFFFFFFFu;
            MmioFile& f = found->second;
            const uint32_t n = std::min<uint32_t>(p.arg(2), static_cast<uint32_t>(f.data.size()) - std::min<uint32_t>(f.position, static_cast<uint32_t>(f.data.size())));
            if (n) cpu.write(p.arg(1), &f.data[f.position], n);
            f.position += n;
            return n;
        });
        def("mmioDescend", 4, [&p, &cpu, st] {
            const auto found = st->files.find(p.arg(0));
            if (found == st->files.end()) return MMIOERR_CHUNKNOTFOUND;
            MmioFile& f = found->second;
            const uint32_t ck = p.arg(1), parent = p.arg(2), flags = p.arg(3);
            const uint32_t end = parent ? cpu.read_u32(parent + 12) + cpu.read_u32(parent + 4)
                                        : static_cast<uint32_t>(f.data.size());
            const uint32_t want_id = cpu.read_u32(ck), want_type = cpu.read_u32(ck + 8);
            for (uint32_t at = f.position; at + 8 <= end;) {
                const uint32_t id = f.u32(at), size = f.u32(at + 4);
                const bool form = id == kRiff || id == kList;
                const uint32_t type = form ? f.u32(at + 8) : 0;
                bool match = true;
                if (flags & MMIO_FINDCHUNK) match = id == want_id;
                else if (flags & MMIO_FINDRIFF) match = id == kRiff && type == want_type;
                else if (flags & MMIO_FINDLIST) match = id == kList && type == want_type;
                if (match) {
                    cpu.write_u32(ck, id);
                    cpu.write_u32(ck + 4, size);
                    cpu.write_u32(ck + 8, type);
                    cpu.write_u32(ck + 12, at + 8);
                    cpu.write_u32(ck + 16, 0);
                    f.position = at + 8 + (form ? 4 : 0);
                    return MMSYSERR_NOERROR;
                }
                at += 8 + ((size + 1) & ~1u);
            }
            return MMIOERR_CHUNKNOTFOUND;
        });
        def("mmioAscend", 3, [&p, &cpu, st] {
            const auto found = st->files.find(p.arg(0));
            if (found == st->files.end()) return MMIOERR_CHUNKNOTFOUND;
            const uint32_t ck = p.arg(1);
            found->second.position = cpu.read_u32(ck + 12) + ((cpu.read_u32(ck + 4) + 1) & ~1u);
            return MMSYSERR_NOERROR;
        });

        // --- mixer: one device, with the CD line (the music's volume) and the wave line ---------------
        // MIXERLINEA (168 bytes): cbStruct, dwDestination, dwSource, dwLineID, fdwLine, dwUser,
        // dwComponentType, cChannels, cConnections, cControls, szShortName[16], szName[64], Target.
        // MIXERCONTROLA (148 bytes): cbStruct, dwControlID, dwControlType, fdwControl, cMultipleItems,
        // szShortName[16], szName[64], Bounds (min, max, …), Metrics (cSteps, …).
        def("mixerGetNumDevs", 0, [] { return 1u; });
        def("mixerOpen", 5, [&p, &cpu] {
            if (p.arg(1) != 0) return MMSYSERR_BADDEVICEID;
            cpu.write_u32(p.arg(0), kMixer);
            return MMSYSERR_NOERROR;
        });
        def("mixerClose", 1, [] { return MMSYSERR_NOERROR; });
        def("mixerGetLineInfoA", 3, [&p, &cpu] {
            // One destination, the speakers (4), whose sources are the CD and the wave output. The
            // engine finds the CD line among the speakers' sources (0x575c55).
            const uint32_t line = p.arg(1), how = p.arg(2) & 0xF;
            uint32_t type = 0;
            if (how == 3) type = cpu.read_u32(line + 24);                    // MIXER_GETLINEINFOF_COMPONENTTYPE
            else if (how == 2) type = cpu.read_u32(line + 12);               // LINEID
            else if (how == 0) type = cpu.read_u32(line + 4) == 0 ? 4 : 0;   // DESTINATION
            else if (how == 1) {                                             // SOURCE of a destination
                const uint32_t source = cpu.read_u32(line + 8);
                type = cpu.read_u32(line + 4) != 0 ? 0 : source == 0 ? kLineCd : source == 1 ? kLineWave : 0;
            }
            const LineInfo* info = line_info(type);
            if (!info) return kMixerInvalidLine;
            const bool destination = info->type == 4;
            cpu.fill(line + 16, 0, 152);
            cpu.write_u32(line + 4, 0);             // dwDestination
            if (destination) cpu.write_u32(line + 8, 0);
            else cpu.write_u32(line + 8, info->type == kLineCd ? 0 : 1);
            cpu.write_u32(line + 12, info->type);   // the component type serves as the line id
            cpu.write_u32(line + 16, destination ? 1u : 0x80000001u);  // ACTIVE, and SOURCE for a source
            cpu.write_u32(line + 24, info->type);
            cpu.write_u32(line + 28, 2);
            cpu.write_u32(line + 32, destination ? 2 : 0);  // cConnections: the two sources
            cpu.write_u32(line + 36, 1);            // one control: the volume
            cpu.write_string(line + 40, info->name);
            cpu.write_string(line + 56, info->name);
            return MMSYSERR_NOERROR;
        });
        def("mixerGetLineControlsA", 3, [&p, &cpu] {
            const uint32_t lc = p.arg(1), how = p.arg(2) & 0xF;
            uint32_t line = cpu.read_u32(lc + 4);
            if (how == 1) line = cpu.read_u32(lc + 8) >> 4;  // ONEBYID: control id = line << 4 | 1
            const LineInfo* info = line_info(line);
            if (!info || (how == 2 && cpu.read_u32(lc + 8) != kVolumeControl)) return kMixerInvalidControl;
            const uint32_t control = cpu.read_u32(lc + 20);
            cpu.fill(control, 0, 148);
            cpu.write_u32(control, 148);
            cpu.write_u32(control + 4, info->type << 4 | 1);
            cpu.write_u32(control + 8, kVolumeControl);
            cpu.write_string(control + 20, "Volume");
            cpu.write_string(control + 36, "Volume");
            cpu.write_u32(control + 104, 65535);  // Bounds.dwMaximum
            cpu.write_u32(control + 124, 65535);  // Metrics.cSteps
            return MMSYSERR_NOERROR;
        });
        // MIXERCONTROLDETAILS: cbStruct, dwControlID, cChannels, cMultipleItems, cbDetails, paDetails
        // (one MIXERCONTROLDETAILS_UNSIGNED per channel).
        def("mixerGetControlDetailsA", 3, [&p, &cpu, st] {
            const uint32_t d = p.arg(1);
            const auto found = st->volumes.find(cpu.read_u32(d + 4) >> 4);
            if (found == st->volumes.end()) return kMixerInvalidControl;
            const uint32_t channels = std::max<uint32_t>(cpu.read_u32(d + 8), 1);
            for (uint32_t c = 0; c < channels && c < 8; ++c) cpu.write_u32(cpu.read_u32(d + 20) + 4 * c, found->second);
            return MMSYSERR_NOERROR;
        });
        def("mixerSetControlDetails", 3, [&p, &cpu, st, volumes_file] {
            const uint32_t d = p.arg(1);
            const auto found = st->volumes.find(cpu.read_u32(d + 4) >> 4);
            if (found == st->volumes.end()) return kMixerInvalidControl;
            // Kept in whole hundredths, each stored as the smallest value that reads back as the same
            // hundredth: the engine writes percent × 65535 / 100 and reads volume × 100 / 65536, so
            // with the exact value every visit to the Options screen lowered the music by 1 %.
            const uint32_t asked = std::min<uint32_t>(cpu.read_u32(cpu.read_u32(d + 20)), 65535);
            const uint32_t percent = (asked * 100 + 32768) / 65536;
            const uint32_t volume = std::min<uint32_t>((percent * 65536 + 99) / 100, 65535);
            if (volume == found->second) return MMSYSERR_NOERROR;  // the Options screen sets it every frame
            found->second = volume;
            save_volumes(*st, volumes_file);
            if (p.trace) std::fprintf(stderr, "    mixeur : ligne %#x volume %u\n", found->first, found->second);
            if (found->first == kLineCd && st->voice)
                if (Mixer* m = p.mixer()) m->set_volume(st->voice, cd_volume(*st));
            return MMSYSERR_NOERROR;
        });

        // --- MCI: the CD player; the other devices refuse --------------------------------------------
        def("mciSendCommandA", 4, [&p, &cpu, st] {
            const uint32_t device = p.arg(0), message = p.arg(1), flags = p.arg(2), params = p.arg(3);
            if (message == MCI_OPEN) {
                // MCI_OPEN_PARMS: callback, device id, device type (a name, or a number with MCI_OPEN_TYPE_ID)
                const uint32_t type = cpu.read_u32(params + 8);
                const std::string name = (flags & 0x1000) || type < 0x10000 ? std::to_string(type) : cpu.read_string(type);
                std::string lower = name;
                for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lower == "cdaudio" || lower == "516") {  // MCI_DEVTYPE_CD_AUDIO
                    st->cd_open = true;
                    cpu.write_u32(params + 4, kCdDevice);
                    return 0u;
                }
                std::fprintf(stderr, "[hôte] MCI : ouverture de « %s » refusée pour l'instant\n", name.c_str());
                return MCIERR_CANNOT_LOAD_DRIVER;
            }
            if (device != kCdDevice || !st->cd_open) return MCIERR_INVALID_DEVICE_ID;
            Mixer* mixer = p.mixer();
            auto stop = [&] {
                if (mixer && st->voice) mixer->end(st->voice);
                st->playing = false;
                st->notify_window = 0;
            };
            if (p.trace)
                std::fprintf(stderr, "    CD : message %#x, indicateurs %#x, de %#x à %#x\n", message, flags,
                             params ? cpu.read_u32(params + 4) : 0, params ? cpu.read_u32(params + 8) : 0);
            switch (message) {
                case MCI_SET:
                    if (flags & MCI_SET_TIME_FORMAT) st->time_format = cpu.read_u32(params + 4);
                    return 0u;
                case MCI_PLAY: {
                    if (!(flags & MCI_TO)) return 0u;
                    // MCI_PLAY_PARMS: callback, from, to. In TMSF the track is the low byte; « to » is
                    // where to stop: the next track, or later for several tracks in a row (one here).
                    int track = st->track ? st->track : 2;
                    if (flags & MCI_FROM) track = static_cast<int>(st->time_format == MCI_FORMAT_TMSF ? cpu.read_u32(params + 4) & 0xFF : cpu.read_u32(params + 4));
                    stop();
                    st->track = track;
                    char file[64];
                    std::snprintf(file, sizeof file, "Tracks\\Track_%02d.ogg", track);
                    auto path = p.vfs().find(file);
                    if (!path) {
                        // GOG's set stops at Track_19, while Back to War's new nations ask for
                        // later tracks (Bavaria: 20); GOG's MINMM.DLL then plays nothing. The host
                        // plays one of the existing tracks instead, the same one for the same number.
                        std::vector<int> existing;
                        for (int t = 2; t < 100; ++t) {
                            std::snprintf(file, sizeof file, "Tracks\\Track_%02d.ogg", t);
                            if (p.vfs().find(file)) existing.push_back(t);
                        }
                        if (existing.empty()) return 0u;
                        const int replacement = existing[static_cast<size_t>(track - 2 < 0 ? 0 : track - 2) % existing.size()];
                        std::snprintf(file, sizeof file, "Tracks\\Track_%02d.ogg", replacement);
                        path = p.vfs().find(file);
                        std::fprintf(stderr, "[hôte] CD : pas de piste %d, la piste %d la remplace\n", track, replacement);
                    }
                    if (mixer) {
                        if (!st->voice) st->voice = mixer->add_voice();
                        mixer->reset(st->voice);
                        mixer->set_sound(st->voice, decode_file_async(*path));
                        mixer->set_volume(st->voice, cd_volume(*st));
                        mixer->start(st->voice);
                    }
                    st->playing = true;
                    if (flags & MCI_NOTIFY) st->notify_window = cpu.read_u32(params) & 0xFFFF ? cpu.read_u32(params) : p.desktop().hwnd;
                    return 0u;
                }
                case MCI_STOP:
                case MCI_SEEK:
                    stop();
                    return 0u;
                case MCI_PAUSE:
                    if (mixer && st->voice) mixer->stop(st->voice);
                    return 0u;
                case MCI_RESUME:
                    if (mixer && st->voice) mixer->resume(st->voice);
                    return 0u;
                case MCI_CLOSE:
                    stop();
                    st->cd_open = false;
                    return 0u;
                case MCI_STATUS:
                default:
                    std::fprintf(stderr, "[hôte] CD : message MCI %#x non fourni\n", message);
                    return MCIERR_INVALID_DEVICE_ID;
            }
        });
        def("mciGetErrorStringA", 3, [&p, &cpu] {
            if (p.arg(2)) cpu.write_string(p.arg(1), std::string("MCI error ").substr(0, p.arg(2) - 1));
            return 1u;
        });
    }
}

}  // namespace zeus::win32
