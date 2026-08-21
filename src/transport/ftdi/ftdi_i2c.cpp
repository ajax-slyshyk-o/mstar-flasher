#include "mstar/transport/ftdi/ftdi_i2c.hpp"

#include "ftdi_device.hpp"

// Milestone 1: device enumeration is implemented via D2XX (ftdi_device.cpp).
// Actual I2C transactions are implemented in Milestone 2 via LibMPSSE.

namespace mstar::ftdi {

Result<std::vector<ProgrammerInfo>> FtdiI2c::enumerate() {
    return detail::enumerateDevices();
}

Result<FtdiI2c> FtdiI2c::open(const ProgrammerSelector&) {
    return std::unexpected(Error{ErrorCode::Ftdi, "FTDI I2C backend not yet implemented"});
}

Result<void> FtdiI2c::setClock(uint32_t) {
    return std::unexpected(Error{ErrorCode::Ftdi, "FTDI I2C backend not yet implemented"});
}

Result<void> FtdiI2c::write(uint8_t, std::span<const uint8_t>) {
    return std::unexpected(Error{ErrorCode::Ftdi, "FTDI I2C backend not yet implemented"});
}

Result<void> FtdiI2c::read(uint8_t, std::span<uint8_t>) {
    return std::unexpected(Error{ErrorCode::Ftdi, "FTDI I2C backend not yet implemented"});
}

Result<void> FtdiI2c::writeRead(uint8_t, std::span<const uint8_t>, std::span<uint8_t>) {
    return std::unexpected(Error{ErrorCode::Ftdi, "FTDI I2C backend not yet implemented"});
}

} // namespace mstar::ftdi
