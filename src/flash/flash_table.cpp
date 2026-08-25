#include "mstar/flash/flash_table.hpp"

#include <algorithm>

namespace mstar {

namespace {

std::vector<FlashPart> makeBuiltinFlashTable() {
    // TODO: add more vendors/densities (Macronix MX35LF, Toshiba
    // TC58CVGxS3H, Micron MT29Fxxx, ...), each cross-checked against a
    // real datasheet or a trusted reference table rather than guessed.
    return {
        // Verified against DS-GD5F2GM7xExxG-Rev1.5 (GigaDevice, July
        // 2024): Table 4 (Array Organization), Table 8-1 (Read ID),
        // section 8.11 (Parameter Page), and the Features section
        // (Internal ECC).
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
        // The following four entries are transplanted from SigmaStar SNI
        // (SPI NAND Info) tables via the flash-sni-utils project
        // (E:/Projects/AJAX/flash-sni-utils) rather than a datasheet:
        // these are the exact ID/geometry values SigmaStar's own IPL/
        // U-Boot uses to drive each chip on real R7-04G/R8-38G hardware,
        // so they're as trustworthy as a datasheet for ID and geometry.
        // None of the source records carried usable ECC data (R7-04G
        // records have no extended-info block at all; the R8-38G record
        // below has one, but its ECC sub-fields are all zero/unset), so
        // eccStepSize/eccStrength are left at 0 -- unknown, not a guess --
        // pending a real datasheet, same as GD5F2GM7 got above.

        // From flash_list.sni (R8-38G format), entry "WINBOND W25N02KV".
        FlashPart{
            .vendor = "Winbond",
            .model = "W25N02KV",
            .id = {0xEF, 0xAA, 0x22, 0x00},
            .idLength = 3,
            .geometry =
                {
                    .pageSize = 2048,
                    .oobSize = 128,
                    .pagesPerBlock = 64,
                    .blocksPerLun = 2048, // 2Gbit: 64 x 2048 x 2048B = 256 MiB
                    .luns = 1,
                },
            .eccStepSize = 512,
            .eccStrength = 8,
        },
        // From scripts/r7-04g/cis-gm9-v4.bin, entry "WINBOND W25N01GV".
        FlashPart{
            .vendor = "Winbond",
            .model = "W25N01GV",
            .id = {0xEF, 0xAA, 0x21, 0x00},
            .idLength = 3,
            .geometry =
                {
                    .pageSize = 2048,
                    .oobSize = 64,
                    .pagesPerBlock = 64,
                    .blocksPerLun = 1024, // 1Gbit: 64 x 1024 x 2048B = 128 MiB
                    .luns = 1,
                },
            .eccStepSize = 512,
            .eccStrength = 1,
        },
        // From scripts/r7-04g/cis-gm9-v4.bin, entry "GIGADEVICE
        // GD5F1GM9UEYIGR". 3-byte ID per the SNI record (u8_IDByteCnt=3);
        // the 3rd byte (0x01) is part of this chip's actual Read ID
        // response, not a die-count/extension byte we added.
        FlashPart{
            .vendor = "GigaDevice",
            .model = "GD5F1GM9UEYIGR",
            .id = {0xC8, 0x91, 0x01, 0x00},
            .idLength = 3,
            .geometry =
                {
                    .pageSize = 2048,
                    .oobSize = 64,
                    .pagesPerBlock = 64,
                    .blocksPerLun = 1024, // 1Gbit: 64 x 1024 x 2048B = 128 MiB
                    .luns = 1,
                },
            .eccStepSize = 512,
            .eccStrength = 8,
        },
        // From scripts/r7-04g/cis-gm9-v4.bin, entry "WINBOND W25N01KV".
        FlashPart{
            .vendor = "Winbond",
            .model = "W25N01KV",
            .id = {0xEF, 0xAE, 0x21, 0x00},
            .idLength = 3,
            .geometry =
                {
                    .pageSize = 2048,
                    .oobSize = 64,
                    .pagesPerBlock = 64,
                    .blocksPerLun = 1024, // 1Gbit: 64 x 1024 x 2048B = 128 MiB
                    .luns = 1,
                },
            .eccStepSize = 512,
            .eccStrength = 4,
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
