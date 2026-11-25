#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "mstar/flash/flash_part.hpp"
#include "mstar/flash/flash_table.hpp"
#include "mstar/flash/spi_bus.hpp"
#include "mstar/result.hpp"

namespace mstar {

enum class EccState { Clean, Corrected, Threshold };

struct EccReport {
    EccState state = EccState::Clean;
    // Maximum corrected bit count in an ECC step, not a sum over the page.
    // Some chips expose a range rather than an exact count.
    uint8_t correctedBitsMin = 0;
    uint8_t correctedBitsMax = 0;
};

/// SPI-NAND reads and block/page programming. No bad-block remapping or OTP
/// programming. See doc/ECC.md and doc/WRITING.md for supported profiles.
class SpiNand {
public:
    explicit SpiNand(SpiBus& bus);
    SpiNand(const SpiNand&) = delete;
    SpiNand& operator=(const SpiNand&) = delete;

    /// Standard SPI-NAND opcode 0xFF, then waits for the device to
    /// report ready.
    Result<void> reset();

    /// Standard SPI-NAND opcode 0x9F followed by one dummy/address byte.
    /// Returns however many ID bytes were requested (id.size()); callers
    /// typically read 4 to have enough for any builtinFlashTable()
    /// entry's idLength.
    Result<void> readJedecId(std::span<uint8_t> id);

    /// reset() + readJedecId() + a builtinFlashTable() lookup (or
    /// `table`, if supplied — mainly for testing). Fails with
    /// ErrorCode::FlashUnknown if the ID doesn't match any entry.
    Result<FlashPart> identify(const std::vector<FlashPart>& table = builtinFlashTable());

    /// Enable internal ECC and the part's per-page read mode, with readback.
    /// Reject unknown profiles and active OTP access. Call restoreReadConfiguration
    /// on every exit path, including failure: a failed write may have reached flash.
    Result<void> configureRead(const FlashPart& part);

    /// Restore only configuration bits changed by configureRead; report errors.
    /// Does not reset/program/erase the flash. Invalidates readPage readiness.
    Result<void> restoreReadConfiguration();

    /// Requires configureRead. Rechecks configuration, loads a page, decodes
    /// chip-specific ECC and returns data only for a valid corrected/clean page.
    /// Uncorrectable data returns EccUncorrectable; inconsistent status returns
    /// FlashProtocol. Neither permits reading the cache as trusted data.
    Result<EccReport> readPage(
        uint32_t pageIndex,
        std::span<uint8_t> pageBuffer
    );

    /// ECC-off main+physical-spare data for backups. Restores ECC even on error.
    Result<void> readRawPage(uint32_t pageIndex, std::span<uint8_t> buffer);
    /// Inspect spare markers on the first two pages; never mark or remap blocks.
    Result<bool> isBadBlock(uint32_t blockIndex);

    /// Requires configureRead; temporarily removes volatile block protection.
    /// restoreReadConfiguration also restores protection and disables writes.
    Result<void> configureWrite();
    Result<void> eraseBlock(uint32_t blockIndex);
    /// Full pages, ascending order, in the most recently erased block only.
    Result<void> programPage(uint32_t pageIndex, std::span<const uint8_t> data);
    Result<EccReport> verifyPage(uint32_t pageIndex, std::span<const uint8_t> expected);

private:
    SpiBus* bus_;
    std::optional<FlashPart> readPart_;
    uint8_t originalConfig_ = 0;
    uint8_t changedConfigMask_ = 0;
    uint8_t configWriteMask_ = 0xFF;
    bool rawEccPending_ = false;
    uint8_t rawConfigBefore_ = 0;
    bool writeConfigured_ = false;
    bool writeTouched_ = false;
    uint8_t originalProtection_ = 0;
    uint8_t changedProtectionMask_ = 0;
    uint8_t protectionMask_ = 0;
    uint8_t protectionWriteMask_ = 0xFF;
    std::optional<uint32_t> nextProgramPage_;
    uint32_t programEndPage_ = 0;

    Result<uint8_t> getFeature(uint8_t featureAddress);
    Result<void> setFeature(uint8_t featureAddress, uint8_t value);
    Result<EccReport> decodeEcc(uint8_t status, uint32_t pageIndex);
    Result<void> checkReadConfiguration();
    Result<void> restoreEccConfiguration();
    Result<void> restoreRawEcc();
    Result<void> restoreWriteConfiguration();
    Result<void> readRawSlice(uint32_t pageIndex, uint16_t column, std::span<uint8_t> data);
    Result<void> checkWriteConfiguration();
    Result<void> writeEnable();
    Result<void> writeDisable();
    Result<void> finishMutation(Result<void> result);

    /// Polls the status feature register until the operation-in-progress
    /// bit clears. Returns the final status byte, or ErrorCode::Timeout
    /// if it never clears.
    Result<uint8_t> waitReady();
};

} // namespace mstar
