#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <span>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include "mstar/flash/flash_table.hpp"
#include "mstar/flash/spi_nand.hpp"
#include "mstar/isp/mstar_isp.hpp"

#ifdef MSTAR_ENABLE_FTDI
#include "mstar/transport/ftdi/ftdi_i2c.hpp"
#endif

namespace {

std::string hexBytes(std::span<const uint8_t> bytes) {
    std::string result;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) result += ' ';
        result += fmt::format("{:02X}", bytes[i]);
    }
    return result;
}

uint64_t totalPages(const mstar::NandGeometry& geometry) {
    return uint64_t(geometry.pagesPerBlock) * geometry.blocksPerLun * geometry.luns;
}

int runList() {
#ifdef MSTAR_ENABLE_FTDI
    auto devices = mstar::ftdi::FtdiI2c::enumerate();
    if (!devices) {
        fmt::println(stderr, "Failed to enumerate FTDI devices: {}", devices.error().message);
        return 1;
    }
    if (devices->empty()) {
        fmt::println("No FTDI programmer devices found.");
        return 0;
    }
    for (const auto& dev : *devices) {
        fmt::println("Programmer:");
        fmt::println("  FTDI device: {}", dev.kind);
        fmt::println("  Description: {}", dev.description);
        fmt::println("  Serial: {}", dev.serial);
        fmt::println("  Channel: {}", dev.location.empty() ? "-" : dev.location);
        if (dev.comPort) {
            fmt::println("  COM port: COM{}", *dev.comPort);
        }
        if (dev.inUse) {
            fmt::println(
                "  Note: held open by another process/driver (fields above may be incomplete)");
        }
        fmt::println("");
    }
    return 0;
#else
    fmt::println("No programmer backends are enabled in this build.");
    return 0;
#endif
}

int runProbe(const std::string& serial, uint8_t ispAddress, uint32_t i2cClockHz) {
#ifdef MSTAR_ENABLE_FTDI
    mstar::ProgrammerSelector selector;
    if (!serial.empty()) {
        selector.serial = serial;
    }

    auto i2c = mstar::ftdi::FtdiI2c::open(selector);
    if (!i2c) {
        fmt::println(stderr, "Failed to open FTDI I2C channel: {}", i2c.error().message);
        return 1;
    }

    if (auto result = i2c->setClock(i2cClockHz); !result) {
        fmt::println(stderr, "Failed to set I2C clock: {}", result.error().message);
        return 1;
    }

    fmt::println("Target:");
    fmt::println("  MStar ISP address: {:#x}", ispAddress);

    mstar::MstarIsp isp(*i2c, ispAddress);

    if (auto result = isp.enter(); !result) {
        fmt::println("  ISP activation: FAILED ({})", result.error().message);
        return 1;
    }
    fmt::println("  ISP activation: OK");
    fmt::println("");

    mstar::SpiNand nand(isp);

    auto resetResult = nand.reset();
    std::array<uint8_t, 4> jedecId{};
    auto readResult = resetResult ? nand.readJedecId(jedecId) : resetResult;

    // Best-effort: release the SoC from ISP mode whether or not the JEDEC
    // ID read below succeeded. probe must never leave the target stuck in
    // a debug state.
    if (auto leaveResult = isp.leave(); !leaveResult) {
        fmt::println(stderr, "Warning: failed to leave MStar ISP mode: {}",
                      leaveResult.error().message);
    }

    if (!readResult) {
        fmt::println(stderr, "Failed to read flash JEDEC ID: {}", readResult.error().message);
        return 1;
    }

    fmt::println("Flash:");

    auto part = mstar::lookupFlashPart(jedecId);
    if (part) {
        // Only idLength bytes are the chip's actual ID; the rest of the
        // buffer is the ID stream repeating (harmless, but not part of
        // the ID itself), so trim it once we know how many bytes matter.
        fmt::println("  JEDEC ID: {}", hexBytes(std::span(jedecId).first(part->idLength)));
        fmt::println("  Type: SPI-NAND");
        fmt::println("  Model: {} {}", part->vendor, part->model);
        uint64_t capacityBytes = totalPages(part->geometry) * part->geometry.pageSize;
        fmt::println("  Capacity: {} MiB", capacityBytes / (1024 * 1024));
    } else {
        // Unknown part: show every byte read, since idLength isn't known
        // and the extra bytes may help identify it against a datasheet.
        fmt::println("  JEDEC ID: {}", hexBytes(jedecId));
        fmt::println("  Type: Unknown (no matching entry in the flash table)");
    }
    return 0;
#else
    (void)serial;
    (void)ispAddress;
    (void)i2cClockHz;
    fmt::println("No programmer backends are enabled in this build.");
    return 1;
#endif
}

int runRead(
    const std::string& serial,
    uint8_t ispAddress,
    uint32_t i2cClockHz,
    const std::string& outputPath,
    uint64_t offset,
    uint64_t size
) {
#ifdef MSTAR_ENABLE_FTDI
    mstar::ProgrammerSelector selector;
    if (!serial.empty()) {
        selector.serial = serial;
    }

    auto i2c = mstar::ftdi::FtdiI2c::open(selector);
    if (!i2c) {
        fmt::println(stderr, "Failed to open FTDI I2C channel: {}", i2c.error().message);
        return 1;
    }

    if (auto result = i2c->setClock(i2cClockHz); !result) {
        fmt::println(stderr, "Failed to set I2C clock: {}", result.error().message);
        return 1;
    }

    mstar::MstarIsp isp(*i2c, ispAddress);
    if (auto result = isp.enter(); !result) {
        fmt::println(stderr, "Failed to enter MStar ISP mode: {}", result.error().message);
        return 1;
    }

    mstar::SpiNand nand(isp);
    auto part = nand.identify();
    if (!part) {
        fmt::println(stderr, "Failed to identify flash: {}", part.error().message);
        if (auto leaveResult = isp.leave(); !leaveResult) {
            fmt::println(stderr, "Warning: failed to leave MStar ISP mode: {}",
                          leaveResult.error().message);
        }
        return 1;
    }

    fmt::println("Flash: {} {} ({} bytes/page)", part->vendor, part->model,
                  part->geometry.pageSize);

    uint32_t pageSize = part->geometry.pageSize;
    uint64_t flashBytes = totalPages(part->geometry) * pageSize;

    if (offset > flashBytes) {
        fmt::println(stderr, "Offset {:#x} is beyond the end of the flash ({} bytes)", offset,
                      flashBytes);
        if (auto leaveResult = isp.leave(); !leaveResult) {
            fmt::println(stderr, "Warning: failed to leave MStar ISP mode: {}",
                          leaveResult.error().message);
        }
        return 1;
    }

    uint64_t readSize = (size == 0) ? (flashBytes - offset) : size;
    if (offset + readSize > flashBytes) {
        fmt::println(
            stderr, "Requested range [{:#x}, {:#x}) exceeds the flash size ({} bytes)", offset,
            offset + readSize, flashBytes);
        if (auto leaveResult = isp.leave(); !leaveResult) {
            fmt::println(stderr, "Warning: failed to leave MStar ISP mode: {}",
                          leaveResult.error().message);
        }
        return 1;
    }

    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        fmt::println(stderr, "Failed to open output file: {}", outputPath);
        if (auto leaveResult = isp.leave(); !leaveResult) {
            fmt::println(stderr, "Warning: failed to leave MStar ISP mode: {}",
                          leaveResult.error().message);
        }
        return 1;
    }

    // readPage only ever returns a whole page, so a sub-page offset/size
    // still requires reading the whole first/last page and slicing out
    // just the requested bytes.
    uint64_t startPage = readSize == 0 ? 0 : offset / pageSize;
    uint64_t endPage = readSize == 0 ? 0 : (offset + readSize - 1) / pageSize;
    uint64_t pagesRead = readSize == 0 ? 0 : (endPage - startPage + 1);

    std::vector<uint8_t> pageBuffer(pageSize);
    uint64_t badPages = 0;
    bool fatalError = false;

    for (uint64_t page = startPage; readSize > 0 && page <= endPage && !fatalError; ++page) {
        auto readResult = nand.readPage(static_cast<uint32_t>(page), part->geometry, pageBuffer);
        if (!readResult) {
            if (readResult.error().code == mstar::ErrorCode::EccUncorrectable) {
                ++badPages;
                std::fill(pageBuffer.begin(), pageBuffer.end(), 0xFF);
            } else {
                fmt::println(stderr, "Read failed at page {}: {}", page,
                              readResult.error().message);
                fatalError = true;
                break;
            }
        }

        uint64_t pageStartByte = page * pageSize;
        uint64_t sliceStart = std::max(offset, pageStartByte) - pageStartByte;
        uint64_t sliceEnd = std::min(offset + readSize, pageStartByte + pageSize) - pageStartByte;
        out.write(reinterpret_cast<const char*>(pageBuffer.data() + sliceStart),
                   static_cast<std::streamsize>(sliceEnd - sliceStart));
    }

    out.close();

    if (auto leaveResult = isp.leave(); !leaveResult) {
        fmt::println(stderr, "Warning: failed to leave MStar ISP mode: {}",
                      leaveResult.error().message);
    }

    if (fatalError) {
        return 1;
    }

    fmt::println("Read {} bytes at offset {:#x} ({} pages) to {}", readSize, offset, pagesRead,
                  outputPath);
    if (badPages > 0) {
        fmt::println(
            "Warning: {} page(s) had uncorrectable ECC errors and were filled with 0xFF",
            badPages);
    }
    return 0;
#else
    (void)serial;
    (void)ispAddress;
    (void)i2cClockHz;
    (void)outputPath;
    (void)offset;
    (void)size;
    fmt::println("No programmer backends are enabled in this build.");
    return 1;
#endif
}

// Milestone 2 hardware validation (see docs/BLUEPRINT.md, "Test B - I2C
// electrical activity"): a single-byte write with nothing above I2cMaster
// involved, so START/address/ACK/STOP can be inspected on a logic analyzer.
int runI2cTest(const std::string& serial, uint8_t i2cAddress, uint32_t i2cClockHz) {
#ifdef MSTAR_ENABLE_FTDI
    mstar::ProgrammerSelector selector;
    if (!serial.empty()) {
        selector.serial = serial;
    }

    auto i2c = mstar::ftdi::FtdiI2c::open(selector);
    if (!i2c) {
        fmt::println(stderr, "Failed to open FTDI I2C channel: {}", i2c.error().message);
        return 1;
    }

    if (auto result = i2c->setClock(i2cClockHz); !result) {
        fmt::println(stderr, "Failed to set I2C clock: {}", result.error().message);
        return 1;
    }

    fmt::println("Probing I2C address {:#x} at {} Hz...", i2cAddress, i2cClockHz);
    fmt::println("Expect on a logic analyzer: START, address+W, ACK/NACK, STOP.");

    std::array<uint8_t, 1> probeByte{0x00};
    auto result = i2c->write(i2cAddress, probeByte);
    if (!result) {
        fmt::println("No ACK ({})", result.error().message);
        return 1;
    }

    fmt::println("ACK received.");
    return 0;
#else
    (void)serial;
    (void)i2cAddress;
    (void)i2cClockHz;
    fmt::println("No programmer backends are enabled in this build.");
    return 1;
#endif
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"mstar-flasher: in-system programmer for MStar/SigmaStar SPI-NAND/NOR"};
    app.require_subcommand(1);

    app.add_subcommand("list", "List detected programmer devices")
        ->callback([] { std::exit(runList()); });

    auto* probe = app.add_subcommand("probe", "Identify the attached flash without modifying it");
    static std::string serial;
    static uint32_t ispAddress = 0x49;
    static uint32_t i2cClockHz = 100000;
    probe->add_option("--serial", serial, "Select programmer by serial number");
    probe->add_option("--i2c-address", ispAddress, "MStar ISP I2C address")->default_val(0x49);
    probe->add_option("--i2c-clock", i2cClockHz, "I2C clock in Hz")->default_val(100000);
    probe->callback([] {
        std::exit(runProbe(serial, static_cast<uint8_t>(ispAddress), i2cClockHz));
    });

    auto* read = app.add_subcommand("read", "Read part or all of the flash's main area to a file");
    static std::string readSerial;
    static uint32_t readIspAddress = 0x49;
    static uint32_t readI2cClockHz = 100000;
    static std::string readOutputPath;
    static uint64_t readOffset = 0;
    static uint64_t readSize = 0;
    read->add_option("output", readOutputPath, "Output file path")->required();
    read->add_option("--serial", readSerial, "Select programmer by serial number");
    read->add_option("--i2c-address", readIspAddress, "MStar ISP I2C address")->default_val(0x49);
    read->add_option("--i2c-clock", readI2cClockHz, "I2C clock in Hz")->default_val(100000);
    read->add_option("--offset", readOffset, "Byte offset to start reading from")
        ->default_val(0);
    read->add_option("--size", readSize, "Number of bytes to read (default: to end of flash)")
        ->default_val(0);
    read->callback([] {
        std::exit(runRead(
            readSerial, static_cast<uint8_t>(readIspAddress), readI2cClockHz, readOutputPath,
            readOffset, readSize));
    });

    auto* i2cTest = app.add_subcommand(
        "i2c-test", "Send a single I2C write to verify electrical activity on a logic analyzer");
    static std::string i2cTestSerial;
    static uint32_t i2cTestAddress = 0x49;
    static uint32_t i2cTestClockHz = 100000;
    i2cTest->add_option("--serial", i2cTestSerial, "Select programmer by serial number");
    i2cTest->add_option("--i2c-address", i2cTestAddress, "I2C address to probe")
        ->default_val(0x49);
    i2cTest->add_option("--i2c-clock", i2cTestClockHz, "I2C clock in Hz")->default_val(100000);
    i2cTest->callback([] {
        std::exit(runI2cTest(i2cTestSerial, static_cast<uint8_t>(i2cTestAddress), i2cTestClockHz));
    });

    CLI11_PARSE(app, argc, argv);
    return 0;
}
