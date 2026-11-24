#include "mstar/transport/tracing_i2c.hpp"

#include <ostream>
#include <string>
#include <fmt/format.h>

namespace mstar {
namespace {
std::string bytes(std::span<const uint8_t> data) {
    if (data.empty()) return "0 bytes";
    if (data.size() > 32) return fmt::format("{} bytes", data.size());
    std::string text;
    for (auto byte : data) {
        if (!text.empty()) text += ' ';
        text += fmt::format("{:02X}", byte);
    }
    return text;
}
std::string status(const Result<void>& result) {
    return result ? "OK" : "ERROR: " + result.error().message;
}
}

Result<void> TracingI2cMaster::setClock(uint32_t hz) {
    auto result = inner_.setClock(hz);
    if (trace_) *trace_ << fmt::format("I2C CLOCK {} Hz -> {}\n", hz, status(result));
    return result;
}
Result<void> TracingI2cMaster::write(uint8_t address, std::span<const uint8_t> data) {
    auto result = inner_.write(address, data);
    if (trace_) *trace_ << fmt::format("I2C W {:#04x}: {} -> {}\n", address, bytes(data), status(result));
    return result;
}
Result<void> TracingI2cMaster::read(uint8_t address, std::span<uint8_t> data) {
    auto result = inner_.read(address, data);
    if (trace_) *trace_ << fmt::format("I2C R {:#04x} ({} bytes): {}\n", address, data.size(),
                                     result ? bytes(data) : status(result));
    return result;
}
Result<void> TracingI2cMaster::writeRead(uint8_t address, std::span<const uint8_t> tx,
                                       std::span<uint8_t> rx) {
    auto result = inner_.writeRead(address, tx, rx);
    if (trace_) *trace_ << fmt::format("I2C WR {:#04x}: {} -> {}\n", address, bytes(tx),
                                     result ? bytes(rx) : status(result));
    return result;
}
} // namespace mstar
