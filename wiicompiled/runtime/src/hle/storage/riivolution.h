// Riivolution overlay loading: host-IO shim around hle/riivolution_contract.h. Discovers DVD
// overlay roots (Config.toml/CLI/recomp mod manifest), parses each root's XML, applies option
// selections, and exposes the resulting disc mappings (dvd.cpp) and savegame redirect (nand_fs.cpp).
//
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace RuntimeRiivolution {

struct Mapping {
    enum class Kind {
        File,         // discPath is a file path
        Folder,       // discPath is a directory prefix
        FolderByName, // no disc path: replace disc files matching by filename
    };

    Kind kind = Kind::File;
    std::string discPath;
    std::filesystem::path hostPath;
    bool recursive = true;
    // Riivolution 'create': when false, only files already present on the disc
    // may be replaced; nothing new is added.
    bool create = false;
};

struct PatchSet {
    // In Dolphin's application order (per active patch: files, then folders);
    // a later mapping for the same disc path wins.
    std::vector<Mapping> mappings;
    uint32_t skippedExternals = 0; // externals that do not exist on the host
    // <memory> patches with an inline value (applied by mod_runtime.cpp); `original`, when given, must match first.
    struct MemoryWrite {
        uint32_t address = 0;
        std::vector<uint8_t> value, original;
    };
    std::vector<MemoryWrite> memory;
};

struct Overlay {
    std::filesystem::path root;
    // nullopt: the root carries no Riivolution XML and is a plain disc-shaped
    // overlay directory.
    std::optional<PatchSet> patches;
};

struct SaveRedirect {
    std::filesystem::path hostDirectory;
    bool clone = false;
};

// Discovered overlay roots in precedence order (highest priority first), each
// with its active Riivolution mappings. Loaded once, thread-safe.
const std::vector<Overlay>& Overlays();

// The active <savegame> redirect from the highest-priority overlay that has
// one, resolved to a host directory (Dolphin semantics: the whole NAND
// /title/<id>/data directory is redirected there).
const std::optional<SaveRedirect>& GetSaveRedirect();

} // namespace RuntimeRiivolution
