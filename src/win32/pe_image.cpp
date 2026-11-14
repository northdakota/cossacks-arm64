#include "win32/pe_image.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace zeus::win32 {
namespace {

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

class Reader {
public:
    explicit Reader(std::vector<uint8_t> data) : d_(std::move(data)) {}
    size_t size() const { return d_.size(); }
    const uint8_t* at(size_t offset, size_t length) const {
        if (offset + length > d_.size()) fail("lecture hors du fichier PE");
        return &d_[offset];
    }
    uint16_t u16(size_t o) const {
        const uint8_t* p = at(o, 2);
        return static_cast<uint16_t>(p[0] | p[1] << 8);
    }
    uint32_t u32(size_t o) const {
        const uint8_t* p = at(o, 4);
        return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
               static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
    }

private:
    std::vector<uint8_t> d_;
};

std::string cstring(const Cpu& cpu, uint32_t address) {
    std::string s;
    for (;; ++address) {
        char c = 0;
        cpu.read(address, &c, 1);
        if (!c || s.size() > 512) break;
        s.push_back(c);
    }
    return s;
}

}  // namespace

namespace {

Reader read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) fail("lecture impossible : " + path.string());
    return Reader(std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {}));
}

}  // namespace

uint32_t pe_image_size(const std::filesystem::path& path) {
    const Reader f = read_file(path);
    if (f.size() < 0x40 || f.u16(0) != 0x5A4D) fail("pas un exécutable MZ");
    const size_t pe = f.u32(0x3C);
    if (f.u32(pe) != 0x00004550) fail("pas un exécutable PE");
    return (f.u32(pe + 24 + 56) + 0xFFF) & ~0xFFFu;
}

PeImage load_pe(const std::filesystem::path& path, Cpu& cpu, uint32_t base) {
    const Reader f = read_file(path);
    if (f.size() < 0x40 || f.u16(0) != 0x5A4D) fail("pas un exécutable MZ");
    const size_t pe = f.u32(0x3C);
    if (f.u32(pe) != 0x00004550) fail("pas un exécutable PE");
    if (f.u16(pe + 4) != 0x014C) fail("exécutable non i386");
    const size_t section_count = f.u16(pe + 6);
    const size_t optional = pe + 24;
    if (f.u16(optional) != 0x010B) fail("exécutable non PE32");

    PeImage image;
    image.dll = (f.u16(pe + 22) & 0x2000) != 0;  // IMAGE_FILE_DLL
    image.entry_point = f.u32(optional + 16);
    image.preferred_base = f.u32(optional + 28);
    image.image_base = base ? base : image.preferred_base;
    image.image_size = f.u32(optional + 56);
    const uint32_t header_size = f.u32(optional + 60);
    const uint32_t export_rva = f.u32(optional + 96);
    const uint32_t import_rva = f.u32(optional + 96 + 8);
    const uint32_t reloc_rva = f.u32(optional + 96 + 5 * 8), reloc_size = f.u32(optional + 96 + 5 * 8 + 4);
    if (image.entry_point) image.entry_point += image.image_base;

    // L'image est réservée par tranches : exécutable pour les sections de code, en simple
    // lecture et écriture pour le reste (en-têtes, données, intervalles entre sections).
    const size_t table = optional + f.u16(pe + 20);
    const uint32_t page = 0x1000;
    const uint32_t image_end = image.image_base + ((image.image_size + page - 1) & ~(page - 1));
    uint32_t mapped = image.image_base;
    for (size_t i = 0; i < section_count; ++i) {
        const size_t s = table + 40 * i;
        if (!(f.u32(s + 36) & 0x20000000)) continue;  // IMAGE_SCN_MEM_EXECUTE
        const uint32_t start = (image.image_base + f.u32(s + 12)) & ~(page - 1);
        const uint32_t end = std::min(image_end, (image.image_base + f.u32(s + 12) + std::max(f.u32(s + 8), f.u32(s + 16)) + page - 1) & ~(page - 1));
        if (start < mapped || end <= start) fail("sections de code désordonnées");
        if (start > mapped) cpu.map(mapped, start - mapped);
        cpu.map(start, end - start, true);
        mapped = end;
    }
    if (image_end > mapped) cpu.map(mapped, image_end - mapped);
    cpu.write(image.image_base, f.at(0, header_size), header_size);
    for (size_t i = 0; i < section_count; ++i) {
        const size_t s = table + 40 * i;
        PeSection section;
        const uint8_t* name = f.at(s, 8);
        for (int k = 0; k < 8 && name[k]; ++k) section.name.push_back(static_cast<char>(name[k]));
        section.virtual_size = f.u32(s + 8);
        section.address = image.image_base + f.u32(s + 12);
        section.raw_size = f.u32(s + 16);
        const uint32_t raw_offset = f.u32(s + 20);
        const uint32_t copied = std::min(section.raw_size, std::max(section.virtual_size, section.raw_size));
        if (copied) cpu.write(section.address, f.at(raw_offset, copied), copied);
        image.sections.push_back(section);
    }

    // Base relocations: blocks of a page address and 16-bit entries (type << 12 | offset);
    // HIGHLOW entries get the distance between the chosen and the preferred base.
    if (image.image_base != image.preferred_base) {
        if (!reloc_rva) fail("image sans relocations, impossible de la déplacer : " + path.string());
        const uint32_t delta = image.image_base - image.preferred_base;
        for (uint32_t b = image.image_base + reloc_rva; b < image.image_base + reloc_rva + reloc_size;) {
            const uint32_t page = cpu.read_u32(b), size = cpu.read_u32(b + 4);
            if (size < 8) break;
            for (uint32_t e = b + 8; e < b + size; e += 2) {
                const uint16_t entry = cpu.read_u16(e);
                const uint32_t at = image.image_base + page + (entry & 0xFFF);
                switch (entry >> 12) {
                    case 0: break;  // IMAGE_REL_BASED_ABSOLUTE: padding
                    case 3: {  // HIGHLOW; through the host's view of the memory: the image is not
                               // running yet, and a write through Unicorn per entry made the 126
                               // mission DLLs take 8 s to place
                        uint32_t value;
                        std::memcpy(&value, cpu.host(at), 4);
                        value += delta;
                        std::memcpy(cpu.host(at), &value, 4);
                        break;
                    }
                    default: fail("relocation de type " + std::to_string(entry >> 12) + " inconnue : " + path.string());
                }
            }
            b += size;
        }
    }

    // Exports: names point into the table of addresses through the table of ordinals.
    if (export_rva) {
        const uint32_t d = image.image_base + export_rva;
        const uint32_t ordinal_base = cpu.read_u32(d + 16), functions = cpu.read_u32(d + 20),
                       names = cpu.read_u32(d + 24);
        const uint32_t addresses = image.image_base + cpu.read_u32(d + 28),
                       name_table = image.image_base + cpu.read_u32(d + 32),
                       ordinals = image.image_base + cpu.read_u32(d + 36);
        for (uint32_t i = 0; i < functions; ++i) {
            const uint32_t rva = cpu.read_u32(addresses + 4 * i);
            if (rva) image.exports_by_ordinal[ordinal_base + i] = image.image_base + rva;
        }
        for (uint32_t i = 0; i < names; ++i) {
            const uint32_t rva = cpu.read_u32(addresses + 4 * cpu.read_u16(ordinals + 2 * i));
            image.exports[cstring(cpu, image.image_base + cpu.read_u32(name_table + 4 * i))] = image.image_base + rva;
        }
    }

    // Répertoire d'import : une entrée de 20 octets par DLL, terminée par une entrée nulle.
    for (uint32_t d = image.image_base + import_rva; import_rva != 0; d += 20) {
        const uint32_t lookup = cpu.read_u32(d), name = cpu.read_u32(d + 12), slots = cpu.read_u32(d + 16);
        if (!name) break;
        std::string dll = cstring(cpu, image.image_base + name);
        std::transform(dll.begin(), dll.end(), dll.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const uint32_t names = image.image_base + (lookup ? lookup : slots);
        for (uint32_t i = 0;; ++i) {
            const uint32_t v = cpu.read_u32(names + 4 * i);
            if (!v) break;
            PeImport imp;
            imp.dll = dll;
            imp.function = (v & 0x80000000) ? "#" + std::to_string(v & 0xFFFF)
                                            : cstring(cpu, image.image_base + v + 2);
            imp.slot = image.image_base + slots + 4 * i;
            image.imports.push_back(std::move(imp));
        }
    }
    return image;
}

}  // namespace zeus::win32
