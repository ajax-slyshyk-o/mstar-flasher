#pragma once

#include <cstdint>

namespace mstar {

struct NandGeometry {
    uint32_t pageSize;
    uint32_t oobSize;
    uint32_t pagesPerBlock;
    uint32_t blocksPerLun;
    uint32_t luns;
};

} // namespace mstar
