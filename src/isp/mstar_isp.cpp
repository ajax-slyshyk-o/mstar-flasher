#include "mstar/isp/mstar_isp.hpp"

#include <array>
#include <vector>

namespace mstar {

namespace {
constexpr std::array<uint8_t, 5> kActivationString{'M', 'S', 'T', 'A', 'R'};
} // namespace

MstarIsp::MstarIsp(I2cMaster& i2c, uint8_t ispAddress)
    : i2c_(&i2c), ispAddress_(ispAddress) {}

Result<void> MstarIsp::enter() {
    return i2c_->write(ispAddress_, kActivationString);
}

Result<void> MstarIsp::leave() {
    std::array<uint8_t, 1> payload{static_cast<uint8_t>(MstarDdcCommand::Reset)};
    return i2c_->write(ispAddress_, payload);
}

Result<void> MstarIsp::transaction(
    std::span<const uint8_t> tx,
    std::span<uint8_t> rx
) {
    std::vector<uint8_t> writePayload;
    writePayload.reserve(tx.size() + 1);
    writePayload.push_back(static_cast<uint8_t>(MstarDdcCommand::SpiWrite));
    writePayload.insert(writePayload.end(), tx.begin(), tx.end());

    auto result = i2c_->write(ispAddress_, writePayload);

    if (result && !rx.empty()) {
        std::array<uint8_t, 1> readCommand{
            static_cast<uint8_t>(MstarDdcCommand::SpiRead)};
        result = i2c_->writeRead(ispAddress_, readCommand, rx);
    }

    // A failed transfer may still have reached the target and asserted SPI CS.
    // Attempt SPI_END exactly once, without retrying any payload, and keep the
    // original transfer error if releasing CS also fails.
    std::array<uint8_t, 1> endCommand{static_cast<uint8_t>(MstarDdcCommand::SpiEnd)};
    if (auto endResult = i2c_->write(ispAddress_, endCommand); !endResult) {
        if (result) {
            return std::unexpected(Error{
                endResult.error().code, "SPI_END failed: " + endResult.error().message});
        }
        result.error().message += "; SPI_END also failed: " + endResult.error().message;
    }
    return result;
}

} // namespace mstar
