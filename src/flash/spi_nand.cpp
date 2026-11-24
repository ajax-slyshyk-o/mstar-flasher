#include "mstar/flash/spi_nand.hpp"

#include <array>

#include <fmt/format.h>

#include "mstar/flash/flash_table.hpp"

namespace mstar {

namespace {

// Standard SPI-NAND command opcodes. This exact set (0xFF/0x9F/0x0F/0x1F/
// 0x13/0x03) is shared across essentially every SPI-NAND vendor (Winbond,
// GigaDevice, Macronix, Toshiba/Kioxia, Micron, ESMT, ...); per-vendor
// differences show up in feature-register bit semantics, not these
// opcodes.
constexpr uint8_t kReset = 0xFF;
constexpr uint8_t kReadJedecId = 0x9F;
constexpr uint8_t kGetFeature = 0x0F;
constexpr uint8_t kSetFeature = 0x1F;
constexpr uint8_t kPageRead = 0x13;
constexpr uint8_t kReadFromCache = 0x03;

constexpr uint8_t kFeatureAddressConfig = 0xB0;
constexpr uint8_t kFeatureAddressStatus = 0xC0;
constexpr uint8_t kOtpEnable = 0x40;

struct ReadConfig {
    uint8_t required;
    uint8_t writable;
    uint32_t strength;
};

std::optional<ReadConfig> readConfig(NandEccProfile profile) {
    switch (profile) {
    // GM7 bit 3 is a protection lock, NOT the GM9 normal-read bit.
    case NandEccProfile::Gd5f2gm7: return ReadConfig{0x10, 0xD9, 8};
    case NandEccProfile::Gd5f1gm9: return ReadConfig{0x18, 0xD9, 8};
    case NandEccProfile::W25n01gv: return ReadConfig{0x18, 0xFF, 1};
    case NandEccProfile::W25n01kv: return ReadConfig{0x18, 0xFF, 4};
    case NandEccProfile::W25n02kv: return ReadConfig{0x18, 0xFF, 8};
    default: return std::nullopt;
    }
}
constexpr uint8_t kStatusOip = 0x01; // Operation In Progress

constexpr int kMaxWaitAttempts = 1000;

} // namespace

SpiNand::SpiNand(SpiBus& bus) : bus_(&bus) {}

Result<uint8_t> SpiNand::getFeature(uint8_t featureAddress) {
    std::array<uint8_t, 2> tx{kGetFeature, featureAddress};
    std::array<uint8_t, 1> rx{};
    if (auto result = bus_->transaction(tx, rx); !result) {
        return std::unexpected(result.error());
    }
    return rx[0];
}

Result<void> SpiNand::setFeature(uint8_t featureAddress, uint8_t value) {
    std::array<uint8_t, 3> tx{kSetFeature, featureAddress, value};
    return bus_->transaction(tx, {});
}

Result<uint8_t> SpiNand::waitReady() {
    for (int attempt = 0; attempt < kMaxWaitAttempts; ++attempt) {
        auto status = getFeature(kFeatureAddressStatus);
        if (!status) {
            return std::unexpected(status.error());
        }
        if ((*status & kStatusOip) == 0) {
            return *status;
        }
    }
    return std::unexpected(Error{
        ErrorCode::Timeout,
        "Timed out waiting for SPI-NAND operation-in-progress to clear"});
}

Result<void> SpiNand::reset() {
    if (auto result = restoreReadConfiguration(); !result) return result;
    std::array<uint8_t, 1> tx{kReset};
    if (auto result = bus_->transaction(tx, {}); !result) {
        return std::unexpected(result.error());
    }
    if (auto result = waitReady(); !result) {
        return std::unexpected(result.error());
    }
    return {};
}

Result<void> SpiNand::readJedecId(std::span<uint8_t> id) {
    // Opcode 0x9F is followed by one dummy/address byte before the ID
    // bytes come out on essentially every SPI-NAND part (GigaDevice,
    // Winbond, Macronix, ...); omitting it shifts every returned byte by
    // one position.
    std::array<uint8_t, 2> tx{kReadJedecId, 0x00};
    return bus_->transaction(tx, id);
}

Result<FlashPart> SpiNand::identify(const std::vector<FlashPart>& table) {
    if (auto result = reset(); !result) {
        return std::unexpected(result.error());
    }

    std::array<uint8_t, 4> id{};
    if (auto result = readJedecId(id); !result) {
        return std::unexpected(result.error());
    }

    auto part = lookupFlashPart(id, table);
    if (!part) {
        return std::unexpected(Error{
            ErrorCode::FlashUnknown,
            fmt::format(
                "Unrecognized flash JEDEC ID: {:02X} {:02X} {:02X} {:02X}", id[0], id[1], id[2],
                id[3])});
    }
    return *part;
}

Result<void> SpiNand::configureRead(const FlashPart& part) {
    if (auto result = restoreReadConfiguration(); !result) return result;
    const auto config = readConfig(part.eccProfile);
    if (!config || part.eccStrength != config->strength) {
        return std::unexpected(Error{ErrorCode::FlashProtocol,
            "No verified ECC profile/strength for " + part.model});
    }
    const auto& geometry = part.geometry;
    const uint64_t pageCount = uint64_t(geometry.pagesPerBlock) * geometry.blocksPerLun;
    if (geometry.luns != 1 || geometry.pageSize == 0 || pageCount == 0 || pageCount > (1u << 24)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument,
            "Unsupported SPI-NAND geometry for page reads"});
    }
    auto value = getFeature(kFeatureAddressConfig);
    if (!value) return std::unexpected(value.error());
    if (*value & kOtpEnable) {
        return std::unexpected(Error{ErrorCode::FlashProtocol,
            "OTP access is enabled; refusing a main-array read"});
    }
    originalConfig_ = *value;
    configWriteMask_ = config->writable;
    changedConfigMask_ = config->required & ~*value;
    if (changedConfigMask_) {
        // Record ownership before writing: even a failed transfer can reach flash.
        const uint8_t updated = (*value | config->required) & config->writable;
        if (auto result = setFeature(kFeatureAddressConfig, updated); !result) return result;
        if (auto ready = waitReady(); !ready) return std::unexpected(ready.error());
        value = getFeature(kFeatureAddressConfig);
        if (!value) return std::unexpected(value.error());
    }
    if ((*value & (config->required | kOtpEnable)) != config->required) {
        return std::unexpected(Error{ErrorCode::FlashProtocol,
            "Failed to enable/verify SPI-NAND ECC and per-page read mode"});
    }
    readPart_ = part;
    return {};
}

Result<void> SpiNand::restoreReadConfiguration() {
    readPart_.reset();
    if (!changedConfigMask_) return {};
    auto value = getFeature(kFeatureAddressConfig);
    if (!value) return std::unexpected(value.error());
    const uint8_t restored = ((*value & ~changedConfigMask_) |
                             (originalConfig_ & changedConfigMask_)) & configWriteMask_;
    if ((*value & changedConfigMask_) != (originalConfig_ & changedConfigMask_)) {
        if (auto result = setFeature(kFeatureAddressConfig, restored); !result) return result;
        if (auto ready = waitReady(); !ready) return std::unexpected(ready.error());
        value = getFeature(kFeatureAddressConfig);
        if (!value) return std::unexpected(value.error());
    }
    if ((*value & changedConfigMask_) != (originalConfig_ & changedConfigMask_)) {
        return std::unexpected(Error{ErrorCode::FlashProtocol,
            "Failed to restore SPI-NAND read configuration"});
    }
    changedConfigMask_ = 0;
    return {};
}

Result<EccReport> SpiNand::decodeEcc(uint8_t status, uint32_t pageIndex) {
    const auto profile = readPart_->eccProfile;
    const uint8_t ecc = (status >> 4) & 3;
    if (ecc == 0) return EccReport{};
    if (ecc == 2) {
        return std::unexpected(Error{ErrorCode::EccUncorrectable,
            fmt::format("Uncorrectable ECC error reading page {} ({} status {:#04x})",
                        pageIndex, readPart_->model, status)});
    }
    if (profile == NandEccProfile::W25n01gv) {
        // 11 describes failed continuous reads and cannot be valid in BUF=1.
        if (ecc == 3) {
            return std::unexpected(Error{ErrorCode::FlashProtocol,
                fmt::format("Invalid W25N01GV ECC status {:#04x} in buffer read mode at page {}",
                            status, pageIndex)});
        }
        return EccReport{EccState::Threshold, 1, 1}; // at its 1-bit correction limit
    }
    if (profile == NandEccProfile::Gd5f2gm7 || profile == NandEccProfile::Gd5f1gm9) {
        if (ecc == 3) return EccReport{EccState::Threshold, 8, 8};
        auto extended = getFeature(0xF0);
        if (!extended) return std::unexpected(extended.error());
        const uint8_t detail = (*extended >> 4) & 3;
        // Both manufacturer tables encode 1..4, 5, 6, 7; not an exact
        // count for the first bucket. Do not infer a bit count from 0x7C.
        if (detail == 0) return EccReport{EccState::Corrected, 1, 4};
        const auto bits = static_cast<uint8_t>(4 + detail);
        return EccReport{EccState::Corrected, bits, bits};
    }
    if (profile != NandEccProfile::W25n01kv && profile != NandEccProfile::W25n02kv) {
        return std::unexpected(Error{ErrorCode::FlashProtocol, "Unsupported ECC decoder"});
    }
    // Winbond KV exposes the maximum corrected count in feature 30h.
    auto extended = getFeature(0x30);
    if (!extended) return std::unexpected(extended.error());
    const uint8_t mask = profile == NandEccProfile::W25n01kv ? 7 : 15;
    const uint8_t bits = (*extended >> 4) & mask;
    if (bits == 0 || bits > readPart_->eccStrength) {
        return std::unexpected(Error{ErrorCode::FlashProtocol,
            fmt::format("Inconsistent {} ECC status at page {}: C0={:#04x}, 30={:#04x}",
                        readPart_->model, pageIndex, status, *extended)});
    }
    return EccReport{ecc == 3 ? EccState::Threshold : EccState::Corrected, bits, bits};
}

Result<EccReport> SpiNand::readPage(uint32_t pageIndex, std::span<uint8_t> pageBuffer) {
    if (!readPart_) {
        return std::unexpected(Error{ErrorCode::FlashProtocol,
            "configureRead must succeed before reading SPI-NAND pages"});
    }
    const auto& geometry = readPart_->geometry;
    const uint64_t pageCount = uint64_t(geometry.pagesPerBlock) * geometry.blocksPerLun * geometry.luns;
    if (pageBuffer.size() != geometry.pageSize || pageIndex >= pageCount) {
        return std::unexpected(Error{ErrorCode::InvalidArgument,
            "readPage buffer size or page index does not match flash geometry"});
    }
    auto config = getFeature(kFeatureAddressConfig);
    if (!config) return std::unexpected(config.error());
    const uint8_t required = readConfig(readPart_->eccProfile)->required;
    if ((*config & (required | kOtpEnable)) != required) {
        return std::unexpected(Error{ErrorCode::FlashProtocol,
            "SPI-NAND ECC/read configuration changed; refusing unchecked page data"});
    }

    std::array<uint8_t, 4> pageReadTx{
        kPageRead,
        static_cast<uint8_t>((pageIndex >> 16) & 0xFF),
        static_cast<uint8_t>((pageIndex >> 8) & 0xFF),
        static_cast<uint8_t>(pageIndex & 0xFF),
    };
    if (auto result = bus_->transaction(pageReadTx, {}); !result) {
        return std::unexpected(result.error());
    }
    auto status = waitReady();
    if (!status) return std::unexpected(status.error());
    auto ecc = decodeEcc(*status, pageIndex);
    if (!ecc) return ecc;

    // All supported profiles use two column bytes and one dummy byte for 03h.
    std::array<uint8_t, 4> readCacheTx{kReadFromCache, 0x00, 0x00, 0x00};
    if (auto result = bus_->transaction(readCacheTx, pageBuffer); !result) {
        return std::unexpected(result.error());
    }
    return ecc;
}

} // namespace mstar
