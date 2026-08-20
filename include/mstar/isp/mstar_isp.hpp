#pragma once

#include <cstdint>
#include <span>

#include "mstar/flash/spi_bus.hpp"
#include "mstar/result.hpp"
#include "mstar/transport/i2c_master.hpp"

namespace mstar {

/// MStar/SigmaStar DDC/ISP command bytes, sent as the first byte of the
/// I2C payload following ISP activation.
enum class MstarDdcCommand : uint8_t {
    SpiWrite = 0x10,
    SpiRead  = 0x11,
    SpiEnd   = 0x12,
    Status   = 0x20,
    Reset    = 0x24,
};

/// Exposes the SoC's internal SPI bus through the MStar ISP I2C slave.
class MstarIsp final : public SpiBus {
public:
    static constexpr uint8_t kDefaultIspAddress = 0x49;

    explicit MstarIsp(
        I2cMaster& i2c,
        uint8_t ispAddress = kDefaultIspAddress
    );

    /// Sends the "MSTAR" activation string in a single I2C transaction.
    Result<void> enter();

    /// Sends the reset/exit command.
    Result<void> leave();

    /// Performs a single complete SPI transaction:
    ///   write:  0x10 <tx...>
    ///   if rx is non-empty: writeRead: 0x11 -> rx
    ///   write:  0x12
    /// The SPI chip-select is asserted for the whole transaction and
    /// released only by the trailing 0x12.
    Result<void> transaction(
        std::span<const uint8_t> tx,
        std::span<uint8_t> rx
    ) override;

private:
    I2cMaster* i2c_;
    uint8_t ispAddress_;
};

} // namespace mstar
