#pragma once

#include <cstdint>
#include <span>

#include "mstar/result.hpp"

namespace mstar {

/// Generic I2C master. Implementations must not leak vendor-specific
/// handles or status types (FT_HANDLE, FT_STATUS, libusb types, ...).
class I2cMaster {
public:
    virtual ~I2cMaster() = default;

    virtual Result<void> setClock(uint32_t hz) = 0;

    virtual Result<void> write(
        uint8_t address,
        std::span<const uint8_t> data
    ) = 0;

    virtual Result<void> read(
        uint8_t address,
        std::span<uint8_t> data
    ) = 0;

    /// START, address+W, tx..., REPEATED START, address+R, rx..., STOP.
    virtual Result<void> writeRead(
        uint8_t address,
        std::span<const uint8_t> tx,
        std::span<uint8_t> rx
    ) = 0;
};

} // namespace mstar
