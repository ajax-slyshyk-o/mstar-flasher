#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "mstar/flash/nand_geometry.hpp"

namespace mstar {

struct FlashPart {
    std::string_view vendor;
    std::string_view model;

    std::array<uint8_t, 4> id;
    size_t idLength;

    NandGeometry geometry;

    uint32_t eccStepSize;
    uint32_t eccStrength;
};

} // namespace mstar
