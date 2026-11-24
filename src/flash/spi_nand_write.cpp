#include "mstar/flash/spi_nand.hpp"

#include <algorithm>
#include <array>
#include <fmt/format.h>

namespace mstar {
namespace {
auto fail(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}
bool giga(NandEccProfile profile) {
    return profile == NandEccProfile::Gd5f2gm7 || profile == NandEccProfile::Gd5f1gm9;
}
std::array<uint8_t, 4> rowCommand(uint8_t opcode, uint32_t page) {
    return {opcode, static_cast<uint8_t>(page >> 16), static_cast<uint8_t>(page >> 8),
            static_cast<uint8_t>(page)};
}
Result<void> withCleanup(Result<void> result, Result<void> cleanup, const char* name) {
    if (!cleanup) {
        if (result) return cleanup;
        result.error().message += std::string("; ") + name + ": " + cleanup.error().message;
    }
    return result;
}
}

Result<void> SpiNand::restoreRawEcc() {
    if (!rawEccPending_) return {};
    auto config = getFeature(0xB0);
    if (!config) return std::unexpected(config.error());
    if (!(*config & 0x10)) {
        if (auto r = setFeature(0xB0, (*config | 0x10) & configWriteMask_); !r) return r;
        if (auto r = waitReady(); !r) return std::unexpected(r.error());
        config = getFeature(0xB0);
        if (!config) return std::unexpected(config.error());
    }
    if (!(*config & 0x10)) return fail(ErrorCode::FlashProtocol, "Failed to restore ECC after raw read");
    rawEccPending_ = false;
    return {};
}

Result<void> SpiNand::readRawSlice(uint32_t page, uint16_t column, std::span<uint8_t> data) {
    if (!readPart_ || page >= uint64_t(readPart_->geometry.pagesPerBlock) * readPart_->geometry.blocksPerLun ||
        uint64_t(column) + data.size() > uint64_t(readPart_->geometry.pageSize) + readPart_->geometry.oobSize)
        return fail(ErrorCode::InvalidArgument, "Invalid raw NAND page/range");
    if (auto r = checkReadConfiguration(); !r) return r;
    auto config = getFeature(0xB0);
    if (!config) return std::unexpected(config.error());
    rawEccPending_ = true; // a failed feature write may still reach the chip
    auto result = [&]() -> Result<void> {
        if (auto r = setFeature(0xB0, (*config & ~0x10) & configWriteMask_); !r) return r;
        if (auto r = waitReady(); !r) return std::unexpected(r.error());
        auto actual = getFeature(0xB0);
        if (!actual) return std::unexpected(actual.error());
        // Preserve the previously verified read mode and all defined settings.
        if ((*actual & configWriteMask_) != ((*config & ~0x10) & configWriteMask_))
            return fail(ErrorCode::FlashProtocol, "Failed to verify raw-read configuration");
        if (auto r = bus_->transaction(rowCommand(0x13, page), {}); !r) return r;
        if (auto r = waitReady(); !r) return std::unexpected(r.error());
        const std::array<uint8_t, 4> command{0x03, static_cast<uint8_t>(column >> 8),
                                           static_cast<uint8_t>(column), 0};
        return bus_->transaction(command, data);
    }();
    return withCleanup(result, restoreRawEcc(), "ECC restoration");
}

Result<void> SpiNand::readRawPage(uint32_t page, std::span<uint8_t> buffer) {
    if (!readPart_ || buffer.size() != uint64_t(readPart_->geometry.pageSize) + readPart_->geometry.oobSize)
        return fail(ErrorCode::InvalidArgument, "Raw backup buffer must include main and physical spare area");
    return readRawSlice(page, 0, buffer);
}

Result<bool> SpiNand::isBadBlock(uint32_t block) {
    if (!readPart_ || block >= readPart_->geometry.blocksPerLun || readPart_->geometry.pagesPerBlock < 2)
        return fail(ErrorCode::InvalidArgument, "Invalid NAND block index");
    const auto firstPage = block * readPart_->geometry.pagesPerBlock;
    const size_t count = readPart_->eccProfile == NandEccProfile::W25n01gv ? 2 : 1;
    // Factory markers are on page 0; also respect page-1 markers used by NAND
    // software. Never interpret main-area byte 0 as a marker on a used device.
    for (uint32_t p = firstPage; p < firstPage + 2; ++p) {
        std::array<uint8_t, 2> marker{};
        auto bytes = std::span(marker).first(count);
        if (auto r = readRawSlice(p, static_cast<uint16_t>(readPart_->geometry.pageSize), bytes); !r)
            return std::unexpected(r.error());
        if (std::any_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value != 0xFF; })) return true;
    }
    return false;
}

Result<void> SpiNand::configureWrite() {
    if (auto r = checkReadConfiguration(); !r) return r;
    if (auto r = restoreWriteConfiguration(); !r) return r;
    // Clear volatile BP/INV/CMP (GigaDevice), or BP/TB (Winbond). Preserve
    // register locks, hardware WP control, and all unrelated defined bits.
    protectionMask_ = giga(readPart_->eccProfile) ? 0x3E : 0x7C;
    protectionWriteMask_ = giga(readPart_->eccProfile) ? 0xBE : 0xFF;
    auto protection = getFeature(0xA0);
    if (!protection) return std::unexpected(protection.error());
    originalProtection_ = *protection;
    changedProtectionMask_ = *protection & protectionMask_;
    if (changedProtectionMask_) {
        if (auto r = setFeature(0xA0, (*protection & ~protectionMask_) & protectionWriteMask_); !r) return r;
        if (auto r = waitReady(); !r) return std::unexpected(r.error());
        protection = getFeature(0xA0);
        if (!protection) return std::unexpected(protection.error());
    }
    if (*protection & protectionMask_)
        return fail(ErrorCode::FlashProtocol, "Flash remains write-protected; unlock readback failed");
    writeConfigured_ = true;
    return {};
}

Result<void> SpiNand::writeDisable() {
    const std::array<uint8_t, 1> command{0x04};
    if (auto r = bus_->transaction(command, {}); !r) return r;
    auto status = getFeature(0xC0);
    if (!status) return std::unexpected(status.error());
    if (*status & 3) return fail(ErrorCode::FlashProtocol, "Flash is still busy or write-enabled after WRDI");
    writeTouched_ = false;
    return {};
}

Result<void> SpiNand::restoreWriteConfiguration() {
    writeConfigured_ = false;
    nextProgramPage_.reset();
    Result<void> result;
    if (writeTouched_) result = writeDisable();
    auto restoreProtection = [&]() -> Result<void> {
        if (!changedProtectionMask_) return {};
        auto protection = getFeature(0xA0);
        if (!protection) return std::unexpected(protection.error());
        if ((*protection & changedProtectionMask_) != (originalProtection_ & changedProtectionMask_)) {
            const uint8_t updated = ((*protection & ~changedProtectionMask_) |
                                    (originalProtection_ & changedProtectionMask_)) & protectionWriteMask_;
            if (auto r = setFeature(0xA0, updated); !r) return r;
            if (auto r = waitReady(); !r) return std::unexpected(r.error());
            protection = getFeature(0xA0);
            if (!protection) return std::unexpected(protection.error());
        }
        if ((*protection & changedProtectionMask_) != (originalProtection_ & changedProtectionMask_))
            return fail(ErrorCode::FlashProtocol, "Failed to restore flash block protection");
        changedProtectionMask_ = 0;
        return {};
    };
    return withCleanup(result, restoreProtection(), "block protection restoration");
}

Result<void> SpiNand::checkWriteConfiguration() {
    if (!writeConfigured_) return fail(ErrorCode::FlashProtocol, "configureWrite must succeed before erase/program");
    if (auto r = checkReadConfiguration(); !r) return r;
    auto protection = getFeature(0xA0);
    if (!protection) return std::unexpected(protection.error());
    if (*protection & protectionMask_) return fail(ErrorCode::FlashProtocol, "Flash block protection changed");
    auto ready = waitReady();
    if (!ready) return std::unexpected(ready.error());
    return {};
}

Result<void> SpiNand::writeEnable() {
    writeTouched_ = true;
    const std::array<uint8_t, 1> command{0x06};
    if (auto r = bus_->transaction(command, {}); !r) return r;
    auto status = getFeature(0xC0);
    if (!status) return std::unexpected(status.error());
    if ((*status & 3) != 2) return fail(ErrorCode::FlashProtocol, "Flash did not set WEL before erase/program");
    return {};
}

Result<void> SpiNand::finishMutation(Result<void> result) {
    result = withCleanup(result, writeDisable(), "WRDI");
    if (!result) {
        writeConfigured_ = false;
        nextProgramPage_.reset();
    }
    return result;
}

Result<void> SpiNand::eraseBlock(uint32_t block) {
    if (!readPart_ || block >= readPart_->geometry.blocksPerLun)
        return fail(ErrorCode::InvalidArgument, "Invalid NAND erase block");
    if (auto r = checkWriteConfiguration(); !r) return r;
    auto bad = isBadBlock(block);
    if (!bad) return std::unexpected(bad.error());
    if (*bad) return fail(ErrorCode::BadBlock, fmt::format("Refusing to erase marked block {}", block));
    nextProgramPage_.reset();
    const auto page = block * readPart_->geometry.pagesPerBlock;
    auto result = [&]() -> Result<void> {
        if (auto r = writeEnable(); !r) return r;
        if (auto r = bus_->transaction(rowCommand(0xD8, page), {}); !r) return r;
        auto status = waitReady();
        if (!status) return std::unexpected(status.error());
        if (*status & 4) return fail(ErrorCode::FlashProtocol, fmt::format("E-FAIL erasing block {}", block));
        return {};
    }();
    result = finishMutation(result);
    if (result) {
        nextProgramPage_ = page;
        programEndPage_ = page + readPart_->geometry.pagesPerBlock;
    }
    return result;
}

Result<void> SpiNand::programPage(uint32_t page, std::span<const uint8_t> data) {
    if (!readPart_ || data.size() != readPart_->geometry.pageSize ||
        !nextProgramPage_ || page != *nextProgramPage_ || page >= programEndPage_)
        return fail(ErrorCode::InvalidArgument, "Program full pages in ascending order in a freshly erased block");
    if (auto r = checkWriteConfiguration(); !r) return r;
    std::vector<uint8_t> load{0x02, 0, 0};
    load.insert(load.end(), data.begin(), data.end());
    // 02h replaces the cache and initializes unloaded spare bytes to FF.
    // Follow manufacturer ordering: GigaDevice load then WREN; Winbond WREN then load.
    writeTouched_ = true;
    auto result = [&]() -> Result<void> {
        const bool loadFirst = giga(readPart_->eccProfile);
        if (!loadFirst) { if (auto r = writeEnable(); !r) return r; }
        if (auto r = bus_->transaction(load, {}); !r) return r;
        if (loadFirst) { if (auto r = writeEnable(); !r) return r; }
        if (auto r = bus_->transaction(rowCommand(0x10, page), {}); !r) return r;
        auto status = waitReady();
        if (!status) return std::unexpected(status.error());
        if (*status & 8) return fail(ErrorCode::FlashProtocol, fmt::format("P-FAIL programming page {}", page));
        return {};
    }();
    result = finishMutation(result);
    if (result) ++*nextProgramPage_;
    return result;
}

Result<EccReport> SpiNand::verifyPage(uint32_t page, std::span<const uint8_t> expected) {
    if (!readPart_ || expected.size() != readPart_->geometry.pageSize)
        return fail(ErrorCode::InvalidArgument, "Verify requires a full main-area page");
    std::vector<uint8_t> actual(expected.size());
    auto result = readPage(page, actual);
    if (!result) return result;
    const auto mismatch = std::mismatch(actual.begin(), actual.end(), expected.begin());
    if (mismatch.first != actual.end())
        return fail(ErrorCode::VerifyMismatch,
                    fmt::format("Verify mismatch at page {}, byte {}", page, mismatch.first - actual.begin()));
    return result;
}
} // namespace mstar
