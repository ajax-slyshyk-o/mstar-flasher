#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "mstar/result.hpp"

namespace mstar {

/// UART console transport. Kept separate from I2cMaster.
/// Not implemented until the original programmer's channel-B routing
/// (74HC08 gating) is understood; see docs/HARDWARE.md.
class Console {
public:
    virtual ~Console() = default;

    virtual Result<void> setBaudRate(uint32_t baud) = 0;
    virtual Result<size_t> read(std::span<uint8_t> data) = 0;
    virtual Result<size_t> write(std::span<const uint8_t> data) = 0;
};

} // namespace mstar
