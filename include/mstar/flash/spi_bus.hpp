#pragma once

#include <cstdint>
#include <span>

#include "mstar/result.hpp"

namespace mstar {

/// Generic SPI bus. Flash code must not know whether SPI is transported
/// through MStar ISP, a direct SPI controller, or anything else.
class SpiBus {
public:
    virtual ~SpiBus() = default;

    virtual Result<void> transaction(
        std::span<const uint8_t> tx,
        std::span<uint8_t> rx
    ) = 0;
};

} // namespace mstar
