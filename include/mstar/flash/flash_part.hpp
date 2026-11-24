#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "mstar/flash/nand_geometry.hpp"

namespace mstar {

// Explicit profiles: never infer ECC semantics from vendor ID or geometry.
enum class NandEccProfile {
    Unknown,
    Gd5f2gm7,
    Gd5f1gm9,
    W25n01gv,
    W25n01kv,
    W25n02kv,
};

struct FlashPart {
    // Owned strings, not string_view: entries may eventually come from a
    // loaded file (see flash_table.hpp) rather than only from static
    // string literals in the built-in table.
    std::string vendor;
    std::string model;

    std::array<uint8_t, 8> id;
    size_t idLength;

    NandGeometry geometry;

    uint32_t eccStepSize;
    uint32_t eccStrength;
    NandEccProfile eccProfile = NandEccProfile::Unknown;
};

} // namespace mstar
