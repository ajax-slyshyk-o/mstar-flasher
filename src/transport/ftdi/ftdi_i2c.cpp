#include "mstar/transport/ftdi/ftdi_i2c.hpp"

// Milestone 0 stub: no D2XX/LibMPSSE calls yet (see Milestone 2).

namespace mstar::ftdi {

Result<std::vector<ProgrammerInfo>> FtdiI2c::enumerate() {
    return std::unexpected(Error{ErrorCode::Ftdi, "FTDI backend not yet implemented"});
}

Result<FtdiI2c> FtdiI2c::open(const ProgrammerSelector&) {
    return std::unexpected(Error{ErrorCode::Ftdi, "FTDI backend not yet implemented"});
}

Result<void> FtdiI2c::setClock(uint32_t) {
    return std::unexpected(Error{ErrorCode::Ftdi, "FTDI backend not yet implemented"});
}

Result<void> FtdiI2c::write(uint8_t, std::span<const uint8_t>) {
    return std::unexpected(Error{ErrorCode::Ftdi, "FTDI backend not yet implemented"});
}

Result<void> FtdiI2c::read(uint8_t, std::span<uint8_t>) {
    return std::unexpected(Error{ErrorCode::Ftdi, "FTDI backend not yet implemented"});
}

Result<void> FtdiI2c::writeRead(uint8_t, std::span<const uint8_t>, std::span<uint8_t>) {
    return std::unexpected(Error{ErrorCode::Ftdi, "FTDI backend not yet implemented"});
}

} // namespace mstar::ftdi
