// Chargement d'un exécutable Windows 32 bits (PE) dans la mémoire du processeur émulé.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "win32/cpu.h"

namespace zeus::win32 {

struct PeSection {
    std::string name;
    uint32_t address = 0, virtual_size = 0, raw_size = 0;
};

// Une fonction importée : la case `slot` de la table d'import doit recevoir son adresse.
struct PeImport {
    std::string dll;       // en minuscules
    std::string function;  // nom, ou « #n » pour un import par numéro
    uint32_t slot = 0;
};

struct PeImage {
    uint32_t image_base = 0, entry_point = 0, image_size = 0;
    uint32_t preferred_base = 0;  // base written in the file; image_base differs when relocated
    bool dll = false;
    std::vector<PeSection> sections;
    std::vector<PeImport> imports;
    std::map<std::string, uint32_t> exports;            // name -> address
    std::map<uint32_t, uint32_t> exports_by_ordinal;    // ordinal -> address
};

// Lit le fichier, copie ses sections dans la mémoire émulée et relève ses imports et exports.
// `base`: where to place the image, 0 for its preferred base; elsewhere its base relocations are
// applied (an image without them cannot move). N'exécute rien. Lève std::runtime_error si le
// fichier n'est pas un PE 32 bits i386.
PeImage load_pe(const std::filesystem::path& path, Cpu& cpu, uint32_t base = 0);
// Size of the image in memory (SizeOfImage, rounded to the page), read from the file.
uint32_t pe_image_size(const std::filesystem::path& path);

}  // namespace zeus::win32
