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
    uint8_t featureAddress = 0;
    uint8_t config = 0x10;
    unsigned configWrites = 0;
    uint32_t page = 0;
    bool pageLoaded = false;

    Result<void> reply(std::span<uint8_t> data) {
        std::fill(data.begin(), data.end(), 0);
        if (opcode == 0x9F && data.size() >= 2) {
            data[0] = 0xC8; // Built-in GD5F2GM7, 2048-byte pages.
            data[1] = 0x92;
            if (scenario.starts_with("gv-") || scenario.starts_with("kv-")) {
                data[0] = 0xEF;
                data[1] = 0xAA;
                if (data.size() > 2) data[2] = scenario.starts_with("gv-") ? 0x21 : 0x22;
            } else if (scenario == "gm9-corrected") {
                data[1] = 0x91;
                if (data.size() > 2) data[2] = 1;
            }
        } else if (opcode == 0x0F && !data.empty()) {
            if (featureAddress == 0xB0) {
                data[0] = scenario == "config-lost" && pageLoaded ? 0 : config;
            } else if (featureAddress == 0xC0) {
                const bool ecc = (scenario == "ecc" || scenario == "ecc-then-transport") && page == 1;
                if (pageLoaded && (ecc || scenario == "ecc-all")) data[0] = 0x20;
                if (pageLoaded) {
                    if (scenario == "mixed") data[0] = page == 0 ? 0 : page == 1 ? 0x10 : 0x30;
                    if (scenario == "gv-corrected" || scenario == "kv-corrected" ||
                        scenario == "gm9-corrected" || scenario == "kv-invalid" ||
                        scenario == "extended-error") data[0] = 0x10;
                    if (scenario == "gv-invalid" || scenario == "kv-threshold") data[0] = 0x30;
                }
            } else if (featureAddress == 0xF0 || featureAddress == 0x30) {
                if (scenario == "extended-error")
                    return std::unexpected(Error{ErrorCode::I2cTransfer, "injected ECC detail read error"});
                data[0] = scenario == "kv-invalid" ? 0xF0 : scenario.starts_with("kv-") ? 0x30 :
                          scenario == "gm9-corrected" ? 0x20 : 0;
            }
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
    if (impl->scenario.starts_with("gv-") || impl->scenario.starts_with("kv-") ||
        impl->scenario == "gm9-corrected") impl->config = 0x18;
    if (impl->scenario == "config-disabled" || impl->scenario == "config-ignored" ||
        impl->scenario == "config-write-error" || impl->scenario == "restore-error" ||
        impl->scenario == "restore-mismatch") impl->config = 0x01; // preserve QE
    return FtdiI2c(std::move(impl));
}
Result<void> FtdiI2c::setClock(uint32_t) { return {}; }
Result<void> FtdiI2c::write(uint8_t, std::span<const uint8_t> data) {
    if (data.size() >= 2 && data[0] == 0x10) {
        impl_->opcode = data[1];
        if (impl_->opcode == 0x0F && data.size() == 3) impl_->featureAddress = data[2];
        if (impl_->opcode == 0x1F && data.size() == 4 && data[2] == 0xB0) {
            ++impl_->configWrites;
            if (impl_->configWrites == 2 && impl_->scenario == "restore-error")
                return std::unexpected(Error{ErrorCode::I2cTransfer, "injected restore error"});
            if (impl_->scenario != "config-ignored" &&
                !(impl_->configWrites == 2 && impl_->scenario == "restore-mismatch"))
                impl_->config = data[3];
            if (impl_->configWrites == 1 && impl_->scenario == "config-write-error")
                return std::unexpected(Error{ErrorCode::I2cTransfer, "injected configuration write error"});
        }
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
