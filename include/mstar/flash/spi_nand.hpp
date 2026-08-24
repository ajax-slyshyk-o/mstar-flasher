#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "mstar/flash/flash_part.hpp"
#include "mstar/flash/flash_table.hpp"
#include "mstar/flash/spi_bus.hpp"
#include "mstar/result.hpp"

namespace mstar {

/// Minimal SPI-NAND command set: identify + raw main-area page reads.
/// Program/erase are deliberately out of scope until reads are trusted
/// (see doc/BLUEPRINT.md Milestone 6). Bad-block/OOB/ECC-detail handling
/// beyond "did this page read cleanly" is Milestone 5's job; this layer
/// only refuses to silently return corrupted data.
class SpiNand {
public:
    explicit SpiNand(SpiBus& bus);

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

    /// Reads exactly geometry.pageSize bytes of the main area (no OOB)
    /// from the given absolute page index via PAGE READ + READ FROM
    /// CACHE. Fails with ErrorCode::EccUncorrectable if the chip reports
    /// the page could not be corrected.
    Result<void> readPage(
        uint32_t pageIndex,
        const NandGeometry& geometry,
        std::span<uint8_t> pageBuffer
    );

private:
    SpiBus* bus_;

    Result<uint8_t> getFeature(uint8_t featureAddress);
    Result<void> setFeature(uint8_t featureAddress, uint8_t value);

    /// Polls the status feature register until the operation-in-progress
    /// bit clears. Returns the final status byte, or ErrorCode::Timeout
    /// if it never clears.
    Result<uint8_t> waitReady();
};

} // namespace mstar
