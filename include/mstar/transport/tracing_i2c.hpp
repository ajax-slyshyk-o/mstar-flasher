#pragma once

#include <iosfwd>
#include "mstar/transport/i2c_master.hpp"

namespace mstar {

/// Optional transaction tracing without changing transport behavior. Large
/// buffers are summarized; failed reads never print potentially stale bytes.
class TracingI2cMaster final : public I2cMaster {
public:
    TracingI2cMaster(I2cMaster& inner, std::ostream* trace = nullptr)
        : inner_(inner), trace_(trace) {}

    Result<void> setClock(uint32_t hz) override;
    Result<void> write(uint8_t address, std::span<const uint8_t> data) override;
    Result<void> read(uint8_t address, std::span<uint8_t> data) override;
    Result<void> writeRead(uint8_t address, std::span<const uint8_t> tx,
                           std::span<uint8_t> rx) override;

private:
    I2cMaster& inner_;
    std::ostream* trace_;
};

} // namespace mstar
