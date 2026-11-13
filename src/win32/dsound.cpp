// DSOUND: the game's sound effects, over the host's mixer (audio.cpp).
//
// IDirectSound and IDirectSoundBuffer are COM objects in the emulated memory, like DirectDraw's
// (ddraw.cpp). A secondary buffer keeps its bytes in the emulated memory, where the game writes
// them between Lock and Unlock; Play turns them into a Sound of the mixer (kept until the next
// Unlock) and plays it on the buffer's own voice. DuplicateSoundBuffer shares the bytes, with a
// voice of its own, so that the same sound can play several times at once.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

#include "win32/process.h"

namespace zeus::win32 {
namespace {

constexpr uint32_t DS_OK = 0, DSERR_INVALIDPARAM = 0x80070057, E_NOINTERFACE = 0x80004002;
constexpr uint32_t DSBCAPS_PRIMARYBUFFER = 0x1;
constexpr uint32_t DSBPLAY_LOOPING = 0x1, DSBSTATUS_PLAYING = 0x1, DSBSTATUS_LOOPING = 0x4;
constexpr uint32_t DSBLOCK_FROMWRITECURSOR = 0x1, DSBLOCK_ENTIREBUFFER = 0x2;

// The bytes of a buffer and their format, shared by a buffer and its duplicates.
struct Data {
    uint32_t address = 0, size = 0;
    int channels = 1, rate = 22050, bits = 8;
    uint32_t version = 0;               // + 1 at each Unlock
    uint32_t sound_version = 0xFFFFFFFF;
    std::shared_ptr<Sound> sound;       // the bytes as last turned into a sound
};

struct Buffer {
    std::shared_ptr<Data> data;
    bool primary = false;
    int voice = 0;                      // mixer voice, 0 without a mixer
    int32_t volume = 0, pan = 0;        // hundredths of dB
    uint32_t frequency = 0;             // 0: the format's own
    bool looping = false;
    int refs = 1;
};

struct State {
    std::map<uint32_t, Buffer> buffers;  // object -> buffer
    uint32_t buffer_vtable = 0, sound_vtable = 0;
};

struct Method {
    const char* name;
    uint32_t args;
    std::function<uint32_t()> fn;  // empty: not written, stops the program naming it
};

uint32_t make_vtable(Process& p, const std::string& interface, const std::vector<Method>& methods) {
    const uint32_t table = p.alloc(static_cast<uint32_t>(methods.size() * 4));
    for (size_t i = 0; i < methods.size(); ++i) {
        std::function<uint32_t()> fn = methods[i].fn;
        if (!fn) fn = []() -> uint32_t { throw std::runtime_error("méthode non fournie"); };
        p.cpu().write_u32(table + 4 * static_cast<uint32_t>(i),
                          p.host_function(interface + "::" + methods[i].name, methods[i].args * 4, std::move(fn)));
    }
    return table;
}

uint32_t make_object(Process& p, uint32_t vtable) {
    const uint32_t object = p.alloc(8);
    p.cpu().write_u32(object, vtable);
    return object;
}

// Hundredths of dB (-10000 to 0) to the mixer's 0..127.
int mixer_volume(int32_t volume) {
    if (volume <= -10000) return 0;
    return static_cast<int>(std::lround(127.0 * std::pow(10.0, std::min(volume, 0) / 2000.0)));
}

// Pan in hundredths of dB (negative: the right side attenuated) to the mixer's 0..127 (64: centre).
int mixer_pan(int32_t pan) {
    if (pan >= 0) return static_cast<int>(std::lround(127.0 - 63.5 * std::pow(10.0, -std::min(pan, 10000) / 2000.0)));
    return static_cast<int>(std::lround(63.5 * std::pow(10.0, std::max(pan, -10000) / 2000.0)));
}

std::shared_ptr<Sound> to_sound(Cpu& cpu, const Data& d) {
    auto s = std::make_shared<Sound>();
    s->rate = d.rate;
    s->channels = d.channels;
    const uint8_t* bytes = cpu.host(d.address);
    if (d.bits == 8) {
        s->samples.resize(d.size);
        for (uint32_t i = 0; i < d.size; ++i) s->samples[i] = static_cast<int16_t>((bytes[i] - 128) << 8);
    } else {
        s->samples.resize(d.size / 2);
        std::memcpy(s->samples.data(), bytes, s->samples.size() * 2);
    }
    s->ready = true;
    return s;
}

}  // namespace

void install_dsound(Process& p) {
    auto st = std::make_shared<State>();
    Cpu& cpu = p.cpu();
    auto buffer = [st](uint32_t object) -> Buffer& {
        const auto found = st->buffers.find(object);
        if (found == st->buffers.end()) throw std::runtime_error("tampon de son inconnu");
        return found->second;
    };
    auto new_buffer = [&p, st](Buffer b) {
        if (!b.primary)
            if (Mixer* m = p.mixer()) b.voice = m->add_voice();
        const uint32_t object = make_object(p, st->buffer_vtable);
        st->buffers[object] = std::move(b);
        return object;
    };
    auto apply = [&p](const Buffer& b) {
        Mixer* m = p.mixer();
        if (!m || !b.voice) return;
        m->set_volume(b.voice, mixer_volume(b.volume));
        m->set_pan(b.voice, mixer_pan(b.pan));
        m->set_rate(b.voice, static_cast<int>(b.frequency));
    };

    // --- IDirectSoundBuffer --------------------------------------------------------------
    st->buffer_vtable = make_vtable(p, "IDirectSoundBuffer", {
        {"QueryInterface", 3, [&p, &cpu] {
            if (p.arg(2)) cpu.write_u32(p.arg(2), 0);
            return E_NOINTERFACE;  // no 3D sound
        }},
        {"AddRef", 1, [&p, buffer] { return static_cast<uint32_t>(++buffer(p.arg(0)).refs); }},
        {"Release", 1, [&p, st] {
            const auto found = st->buffers.find(p.arg(0));
            if (found == st->buffers.end()) return 0u;
            if (--found->second.refs > 0) return static_cast<uint32_t>(found->second.refs);
            if (found->second.voice)
                if (Mixer* m = p.mixer()) m->remove_voice(found->second.voice);
            // The bytes belong to the last buffer that shares them.
            if (found->second.data && found->second.data.use_count() == 1 && found->second.data->address)
                p.release(found->second.data->address);
            st->buffers.erase(found);
            return 0u;
        }},
        {"GetCaps", 2, [&p, &cpu, buffer] {
            const Buffer& b = buffer(p.arg(0));
            cpu.write_u32(p.arg(1) + 4, b.primary ? DSBCAPS_PRIMARYBUFFER : 0xE0);  // CTRLVOLUME|PAN|FREQUENCY
            cpu.write_u32(p.arg(1) + 8, b.data ? b.data->size : 0);
            return DS_OK;
        }},
        {"GetCurrentPosition", 3, [&p, &cpu, buffer] {
            const Buffer& b = buffer(p.arg(0));
            uint32_t position = 0;
            if (b.data && b.voice)
                if (Mixer* m = p.mixer()) {
                    const uint32_t block = static_cast<uint32_t>(b.data->channels * b.data->bits / 8);
                    position = static_cast<uint32_t>(m->position(b.voice) * b.data->size) / block * block;
                }
            if (p.arg(1)) cpu.write_u32(p.arg(1), position);
            if (p.arg(2)) cpu.write_u32(p.arg(2), position);
            return DS_OK;
        }},
        {"GetFormat", 4, [&p, &cpu, buffer] {
            const Buffer& b = buffer(p.arg(0));
            if (!b.data) return DSERR_INVALIDPARAM;
            const uint32_t wfx = p.arg(1);
            const uint16_t block = static_cast<uint16_t>(b.data->channels * b.data->bits / 8);
            if (wfx && p.arg(2) >= 16) {
                cpu.write_u16(wfx, 1);
                cpu.write_u16(wfx + 2, static_cast<uint16_t>(b.data->channels));
                cpu.write_u32(wfx + 4, static_cast<uint32_t>(b.data->rate));
                cpu.write_u32(wfx + 8, static_cast<uint32_t>(b.data->rate) * block);
                cpu.write_u16(wfx + 12, block);
                cpu.write_u16(wfx + 14, static_cast<uint16_t>(b.data->bits));
            }
            if (p.arg(3)) cpu.write_u32(p.arg(3), 16);
            return DS_OK;
        }},
        {"GetVolume", 2, [&p, &cpu, buffer] {
            cpu.write_u32(p.arg(1), static_cast<uint32_t>(buffer(p.arg(0)).volume));
            return DS_OK;
        }},
        {"GetPan", 2, [&p, &cpu, buffer] {
            cpu.write_u32(p.arg(1), static_cast<uint32_t>(buffer(p.arg(0)).pan));
            return DS_OK;
        }},
        {"GetFrequency", 2, [&p, &cpu, buffer] {
            const Buffer& b = buffer(p.arg(0));
            cpu.write_u32(p.arg(1), b.frequency ? b.frequency : b.data ? static_cast<uint32_t>(b.data->rate) : 22050u);
            return DS_OK;
        }},
        {"GetStatus", 2, [&p, &cpu, buffer] {
            const Buffer& b = buffer(p.arg(0));
            uint32_t status = 0;
            if (b.voice)
                if (Mixer* m = p.mixer(); m && m->state(b.voice) == Mixer::State::Playing)
                    status = DSBSTATUS_PLAYING | (b.looping ? DSBSTATUS_LOOPING : 0);
            cpu.write_u32(p.arg(1), status);
            return DS_OK;
        }},
        {"Initialize", 3, {}},
        {"Lock", 8, [&p, &cpu, buffer] {
            // (offset, bytes, &part1, &size1, &part2, &size2, flags): the region may wrap around.
            const Buffer& b = buffer(p.arg(0));
            if (!b.data) return DSERR_INVALIDPARAM;
            const Data& d = *b.data;
            uint32_t offset = p.arg(1), bytes = p.arg(2);
            const uint32_t flags = p.arg(7);
            if (flags & DSBLOCK_FROMWRITECURSOR) offset = 0;
            if (flags & DSBLOCK_ENTIREBUFFER) bytes = d.size;
            if (offset >= d.size || bytes > d.size) return DSERR_INVALIDPARAM;
            const uint32_t first = std::min(bytes, d.size - offset);
            cpu.write_u32(p.arg(3), d.address + offset);
            cpu.write_u32(p.arg(4), first);
            if (p.arg(5)) cpu.write_u32(p.arg(5), bytes > first ? d.address : 0);
            if (p.arg(6)) cpu.write_u32(p.arg(6), bytes - first);
            return DS_OK;
        }},
        {"Play", 4, [&p, &cpu, buffer, apply] {
            Buffer& b = buffer(p.arg(0));
            b.looping = (p.arg(3) & DSBPLAY_LOOPING) != 0;
            Mixer* m = p.mixer();
            if (b.primary || !m || !b.voice) return DS_OK;
            Data& d = *b.data;
            if (d.sound_version != d.version || !d.sound) {
                d.sound = to_sound(cpu, d);
                d.sound_version = d.version;
            }
            apply(b);
            m->set_loops(b.voice, b.looping ? 0 : 1);
            const Mixer::State state = m->state(b.voice);
            if (state == Mixer::State::Playing) return DS_OK;  // already playing: only the looping changes
            m->set_sound(b.voice, d.sound);
            if (state == Mixer::State::Stopped) m->resume(b.voice);  // where Stop left it
            else m->start(b.voice);
            return DS_OK;
        }},
        {"SetCurrentPosition", 2, [&p, buffer] {
            const Buffer& b = buffer(p.arg(0));
            if (b.data && b.voice && b.data->size)
                if (Mixer* m = p.mixer()) {
                    if (p.arg(1) == 0 && m->state(b.voice) != Mixer::State::Playing) m->end(b.voice);  // next Play starts over
                    else m->set_position(b.voice, static_cast<double>(p.arg(1)) / b.data->size);
                }
            return DS_OK;
        }},
        {"SetFormat", 2, [] { return DS_OK; }},  // the primary buffer's: the mixer has its own
        {"SetVolume", 2, [&p, buffer, apply] {
            Buffer& b = buffer(p.arg(0));
            b.volume = static_cast<int32_t>(p.arg(1));
            apply(b);
            return DS_OK;
        }},
        {"SetPan", 2, [&p, buffer, apply] {
            Buffer& b = buffer(p.arg(0));
            b.pan = static_cast<int32_t>(p.arg(1));
            apply(b);
            return DS_OK;
        }},
        {"SetFrequency", 2, [&p, buffer, apply] {
            Buffer& b = buffer(p.arg(0));
            b.frequency = p.arg(1);
            apply(b);
            return DS_OK;
        }},
        {"Stop", 1, [&p, buffer] {
            const Buffer& b = buffer(p.arg(0));
            if (b.voice)
                if (Mixer* m = p.mixer(); m && m->state(b.voice) == Mixer::State::Playing) m->stop(b.voice);
            return DS_OK;
        }},
        {"Unlock", 5, [&p, buffer] {
            Buffer& b = buffer(p.arg(0));
            if (b.data) ++b.data->version;
            return DS_OK;
        }},
        {"Restore", 1, [] { return DS_OK; }},
    });

    // --- IDirectSound ----------------------------------------------------------------------
    st->sound_vtable = make_vtable(p, "IDirectSound", {
        {"QueryInterface", 3, [&p, &cpu] {
            if (p.arg(2)) cpu.write_u32(p.arg(2), 0);
            return E_NOINTERFACE;
        }},
        {"AddRef", 1, [] { return 2u; }},
        {"Release", 1, [] { return 0u; }},
        {"CreateSoundBuffer", 4, [&p, &cpu, new_buffer] {
            // DSBUFFERDESC: size, flags, buffer bytes, reserved, WAVEFORMATEX*
            const uint32_t desc = p.arg(1);
            Buffer b;
            b.primary = (cpu.read_u32(desc + 4) & DSBCAPS_PRIMARYBUFFER) != 0;
            if (!b.primary) {
                auto d = std::make_shared<Data>();
                d->size = cpu.read_u32(desc + 8);
                const uint32_t wfx = cpu.read_u32(desc + 16);
                if (!wfx || d->size == 0 || d->size > (64u << 20)) return DSERR_INVALIDPARAM;
                if (cpu.read_u16(wfx) != 1) return DSERR_INVALIDPARAM;  // PCM only
                d->channels = cpu.read_u16(wfx + 2);
                d->rate = static_cast<int>(cpu.read_u32(wfx + 4));
                d->bits = cpu.read_u16(wfx + 14);
                if ((d->bits != 8 && d->bits != 16) || d->channels < 1 || d->channels > 2) return DSERR_INVALIDPARAM;
                d->address = p.alloc(d->size);
                b.data = std::move(d);
            }
            const uint32_t object = new_buffer(std::move(b));
            if (p.trace) std::fprintf(stderr, "    tampon de son %#x\n", object);
            cpu.write_u32(p.arg(2), object);
            return DS_OK;
        }},
        {"GetCaps", 2, [&p, &cpu] {
            const uint32_t caps = p.arg(1), size = cpu.read_u32(caps);
            if (size > 4 && size <= 256) cpu.fill(caps + 4, 0, size - 4);
            cpu.write_u32(caps + 4, 0x0F00);  // DSCAPS_SECONDARY{MONO,STEREO,8BIT,16BIT}
            return DS_OK;
        }},
        {"DuplicateSoundBuffer", 3, [&p, &cpu, buffer, new_buffer] {
            const Buffer& original = buffer(p.arg(1));
            Buffer b;
            b.data = original.data;
            b.primary = original.primary;
            b.volume = original.volume;
            b.pan = original.pan;
            b.frequency = original.frequency;
            cpu.write_u32(p.arg(2), new_buffer(std::move(b)));
            return DS_OK;
        }},
        {"SetCooperativeLevel", 3, [] { return DS_OK; }},
        {"Compact", 1, [] { return DS_OK; }},
        {"GetSpeakerConfig", 2, [&p, &cpu] {
            cpu.write_u32(p.arg(1), 4);  // DSSPEAKER_STEREO
            return DS_OK;
        }},
        {"SetSpeakerConfig", 2, [] { return DS_OK; }},
        {"Initialize", 2, [] { return DS_OK; }},
    });

    p.def("dsound.dll", "DirectSoundCreate", 3 * 4, [&p, &cpu, st] {
        cpu.write_u32(p.arg(1), make_object(p, st->sound_vtable));
        return DS_OK;
    });
}

}  // namespace zeus::win32
