#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "mstar/result.hpp"
#include "mstar/transport/i2c_master.hpp"
#include "mstar/transport/programmer.hpp"

namespace mstar::ftdi {

/// FT2232D-based I2C master backed by D2XX + LibMPSSE-I2C.
///
/// No FT_HANDLE, FT_STATUS or LibMPSSE type ever appears in this header -
/// they are confined to the Impl defined in ftdi_i2c.cpp.
class FtdiI2c final : public I2cMaster {
public:
    static Result<std::vector<ProgrammerInfo>> enumerate();
    static Result<FtdiI2c> open(const ProgrammerSelector& selector);

    FtdiI2c(const FtdiI2c&) = delete;
    FtdiI2c& operator=(const FtdiI2c&) = delete;
    FtdiI2c(FtdiI2c&&) noexcept;
    FtdiI2c& operator=(FtdiI2c&&) noexcept;
    ~FtdiI2c() override;

    Result<void> setClock(uint32_t hz) override;
    Result<void> write(uint8_t address, std::span<const uint8_t> data) override;
    Result<void> read(uint8_t address, std::span<uint8_t> data) override;
    Result<void> writeRead(
        uint8_t address,
        std::span<const uint8_t> tx,
        std::span<uint8_t> rx
    ) override;

private:
    struct Impl;
    explicit FtdiI2c(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace mstar::ftdi
