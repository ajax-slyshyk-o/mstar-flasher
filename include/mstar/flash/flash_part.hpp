#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "mstar/flash/nand_geometry.hpp"

namespace mstar {

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
};

} // namespace mstar
