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

constexpr uint8_t kFeatureAddressStatus = 0xC0;
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

Result<void> SpiNand::readPage(
    uint32_t pageIndex,
    const NandGeometry& geometry,
    std::span<uint8_t> pageBuffer
) {
    if (pageBuffer.size() != geometry.pageSize) {
        return std::unexpected(Error{
            ErrorCode::InvalidArgument, "readPage buffer size does not match geometry.pageSize"});
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
    if (!status) {
        return std::unexpected(status.error());
    }

    // Bits [5:4] of the status register (ECCS1:ECCS0), per the GD5F2GM7
    // datasheet's ECC Error Bits table: 00 = no errors, 01 = errors
    // corrected, 11 = corrected at the ECC engine's max strength (8
    // bits for this chip) -- still good data, 10 = errors exceeded ECC
    // capability and were NOT corrected. Only 10 means bad data. This
    // is inverted from the generic guess used before the datasheet was
    // available; other vendors may differ (Milestone 5's job to widen).
    uint8_t eccBits = (*status >> 4) & 0x03;
    if (eccBits == 0x02) {
        return std::unexpected(Error{
            ErrorCode::EccUncorrectable,
            fmt::format("Uncorrectable ECC error reading page {}", pageIndex)});
    }

    // Per the GD5F2GM7 datasheet's Read From Cache timing diagram (Figure
    // 8-2): opcode, then 2 column-address bytes, then one full dummy byte
    // -- data only starts after that. Omitting the dummy byte shifts every
    // returned byte forward by one position.
    std::array<uint8_t, 4> readCacheTx{kReadFromCache, 0x00, 0x00, 0x00};
    return bus_->transaction(readCacheTx, pageBuffer);
}

} // namespace mstar
