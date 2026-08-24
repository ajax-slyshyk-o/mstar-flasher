#include "mstar/flash/flash_table.hpp"

#include <algorithm>

namespace mstar {

namespace {

std::vector<FlashPart> makeBuiltinFlashTable() {
    // TODO: add more vendors/densities (Winbond W25N, Macronix MX35LF,
    // etc.), each cross-checked against a real datasheet rather than
    // guessed.
    // Verified against DS-GD5F2GM7xExxG-Rev1.5 (GigaDevice, July 2024):
    // Table 4 (Array Organization), Table 8-1 (Read ID), section 8.11
    // (Parameter Page), and the Features section (Internal ECC).
    return {
        FlashPart{
            .vendor = "GigaDevice",
            .model = "GD5F2GM7UExxG",
            // Read ID (0x9F + dummy byte) returns manufacturer 0xC8,
            // device 0x92, then repeats -- only the first idLength bytes
            // are significant. 3.3V variant (Table 8-1); the 1.8V
            // GD5F2GM7RExxG variant reports device ID 0x82 instead.
            .id = {0xC8, 0x92, 0x00, 0x00},
            .idLength = 2,
            .geometry =
                {
                    .pageSize = 2048,
                    // Physical spare area with internal ECC off (128
                    // bytes total; 64 with internal ECC on, the factory
                    // default). Not read by SpiNand::readPage (main area
                    // only).
                    .oobSize = 128,
                    // 2Gbit = 64 pages/block x 2048 blocks/die x
                    // 2048B/page = 268,435,456 bytes (256 MiB). Confirmed
                    // by both Table 4 and the Parameter Page's
                    // pages-per-block/blocks-per-LUN fields.
                    .pagesPerBlock = 64,
                    .blocksPerLun = 2048,
                    .luns = 1,
                },
            // "Internal ECC: 8 bits / 528 bytes" (Features section): one
            // ECC codeword covers a 512-byte main chunk plus its 16-byte
            // spare slice, correcting up to 8 bits. Matches the status
            // register's ECC encoding used in SpiNand::readPage.
            .eccStepSize = 528,
            .eccStrength = 8,
        },
        FlashPart{
            .vendor = "GigaDevice",
            .model = "GD5F2GM7RExxG",
            // 1.8V variant of the same die; same geometry/ECC, device ID
            // 0x82 instead of 0x92 (Table 8-1).
            .id = {0xC8, 0x82, 0x00, 0x00},
            .idLength = 2,
            .geometry =
                {
                    .pageSize = 2048,
                    .oobSize = 128,
                    .pagesPerBlock = 64,
                    .blocksPerLun = 2048,
                    .luns = 1,
                },
            .eccStepSize = 528,
            .eccStrength = 8,
        },
    };
}

} // namespace

const std::vector<FlashPart>& builtinFlashTable() {
    static const std::vector<FlashPart> table = makeBuiltinFlashTable();
    return table;
}

std::optional<FlashPart> lookupFlashPart(
    std::span<const uint8_t> id,
    const std::vector<FlashPart>& table
) {
    for (const auto& part : table) {
        if (id.size() < part.idLength) {
            continue;
        }
        if (std::equal(part.id.begin(), part.id.begin() + part.idLength, id.begin())) {
            return part;
        }
    }
    return std::nullopt;
}

} // namespace mstar
