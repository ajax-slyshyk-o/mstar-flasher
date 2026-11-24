// Test-only FtdiI2c replacement for CLI read tests. No USB libraries are linked.
// --serial selects a deterministic scenario; only this test executable uses it.
#include "mstar/transport/ftdi/ftdi_i2c.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace mstar::ftdi {
struct FtdiI2c::Impl {
    std::string scenario;
    uint8_t opcode = 0;
    uint32_t page = 0;
    bool pageLoaded = false;

    Result<void> reply(std::span<uint8_t> data) {
        std::fill(data.begin(), data.end(), 0);
        if (opcode == 0x9F && data.size() >= 2) {
            data[0] = 0xC8; // Built-in GD5F2GM7, 2048-byte pages.
            data[1] = 0x92;
        } else if (opcode == 0x0F && !data.empty()) {
            const bool ecc = (scenario == "ecc" || scenario == "ecc-then-transport") && page == 1;
            if (pageLoaded && (ecc || scenario == "ecc-all")) data[0] = 0x20;
        } else if (opcode == 0x03) {
            for (std::size_t i = 0; i < data.size(); ++i)
                data[i] = static_cast<uint8_t>(page * 17 + i);
        }
        return {};
    }
};

FtdiI2c::FtdiI2c(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FtdiI2c::FtdiI2c(FtdiI2c&&) noexcept = default;
FtdiI2c& FtdiI2c::operator=(FtdiI2c&&) noexcept = default;
FtdiI2c::~FtdiI2c() = default;

Result<std::vector<ProgrammerInfo>> FtdiI2c::enumerate() {
    return std::vector<ProgrammerInfo>{};
}
Result<FtdiI2c> FtdiI2c::open(const ProgrammerSelector& selector) {
    auto impl = std::make_unique<Impl>();
    impl->scenario = selector.serial.value_or("clean");
    return FtdiI2c(std::move(impl));
}
Result<void> FtdiI2c::setClock(uint32_t) { return {}; }
Result<void> FtdiI2c::write(uint8_t, std::span<const uint8_t> data) {
    if (data.size() >= 2 && data[0] == 0x10) {
        impl_->opcode = data[1];
        if (impl_->opcode == 0xFF) impl_->pageLoaded = false;
        if (impl_->opcode == 0x13 && data.size() == 5) {
            impl_->page = (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 8) | data[4];
            impl_->pageLoaded = true;
            if ((impl_->scenario == "transport" && impl_->page == 1) ||
                (impl_->scenario == "ecc-then-transport" && impl_->page == 2)) {
                return std::unexpected(Error{ErrorCode::I2cTransfer, "injected transport error"});
            }
        }
    }
    return {};
}
Result<void> FtdiI2c::read(uint8_t, std::span<uint8_t> data) {
    return impl_->reply(data);
}
Result<void> FtdiI2c::writeRead(uint8_t, std::span<const uint8_t>, std::span<uint8_t> rx) {
    return impl_->reply(rx);
}
} // namespace mstar::ftdi
