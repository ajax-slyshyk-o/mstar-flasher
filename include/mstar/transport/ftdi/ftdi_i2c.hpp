#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "mstar/result.hpp"
#include "mstar/transport/i2c_master.hpp"
#include "mstar/transport/programmer.hpp"

namespace mstar::ftdi {

/// FT2232D-based I2C master backed by D2XX + LibMPSSE-I2C.
/// Stub for Milestone 0: does not yet call D2XX or LibMPSSE.
class FtdiI2c final : public I2cMaster {
public:
    static Result<std::vector<ProgrammerInfo>> enumerate();
    static Result<FtdiI2c> open(const ProgrammerSelector& selector);

    Result<void> setClock(uint32_t hz) override;
    Result<void> write(uint8_t address, std::span<const uint8_t> data) override;
    Result<void> read(uint8_t address, std::span<uint8_t> data) override;
    Result<void> writeRead(
        uint8_t address,
        std::span<const uint8_t> tx,
        std::span<uint8_t> rx
    ) override;
};

} // namespace mstar::ftdi
