#include "write_commands.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "mstar/flash/spi_nand.hpp"
#include "mstar/isp/mstar_isp.hpp"
#include "mstar/transport/tracing_i2c.hpp"
#ifdef MSTAR_ENABLE_FTDI
#include "mstar/transport/ftdi/ftdi_i2c.hpp"
#endif

namespace {
struct Options {
    std::string image;
    std::string backup;
    std::string serial;
    uint64_t offset = 0;
    uint32_t address = 0x49;
    uint32_t clock = 100000;
    bool yes = false;
    bool dryRun = false;
    bool trace = false;
    bool stay = false;
};

int runImage(const Options& options, bool writing) {
#ifdef MSTAR_ENABLE_FTDI
    if (writing && !options.yes && !options.dryRun) {
        fmt::println(stderr, "Writing erases flash blocks. Supply --yes to explicitly request programming.");
        return 1;
    }
    // Snapshot the input before touching hardware; a file change during backup
    // cannot change the bytes that are programmed or verified.
    std::ifstream input(options.image, std::ios::binary | std::ios::ate);
    if (!input) { fmt::println(stderr, "Cannot open image: {}", options.image); return 1; }
    const auto length = input.tellg();
    uint64_t maximum = 0;
    for (const auto& part : mstar::builtinFlashTable()) {
        const auto& g = part.geometry;
        maximum = std::max(maximum, uint64_t(g.pageSize) * g.pagesPerBlock * g.blocksPerLun * g.luns);
    }
    if (length <= 0 || static_cast<uint64_t>(length) > maximum) {
        fmt::println(stderr, "Image must be nonempty and no larger than a supported flash ({} bytes)", maximum);
        return 1;
    }
    std::vector<uint8_t> image(static_cast<size_t>(length));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(image.size()));
    if (!input) { fmt::println(stderr, "Failed to read complete input image"); return 1; }
    input.close();
    if (writing) {
        std::error_code error;
        for (const auto& path : {options.backup, options.backup + ".json"}) {
            if (std::filesystem::exists(path, error) || error) {
                fmt::println(stderr, "Backup path must be new and accessible: {}", path);
                return 1;
            }
        }
    }

    mstar::ProgrammerSelector selector;
    if (!options.serial.empty()) selector.serial = options.serial;
    else if (writing) selector.firstMatch = true;
    auto transport = mstar::ftdi::FtdiI2c::open(selector);
    if (!transport) { fmt::println(stderr, "Failed to open programmer: {}", transport.error().message); return 1; }
    mstar::TracingI2cMaster bus(*transport, options.trace ? &std::cerr : nullptr);
    if (auto r = bus.setClock(options.clock); !r) { fmt::println(stderr, "{}", r.error().message); return 1; }
    mstar::MstarIsp isp(bus, static_cast<uint8_t>(options.address));
    if (auto r = isp.enter(); !r) { fmt::println(stderr, "ISP activation failed: {}", r.error().message); return 1; }
    mstar::SpiNand nand(isp);
    bool destructiveStarted = false;
    auto perform = [&]() -> mstar::Result<void> {
        auto error = [](mstar::ErrorCode code, std::string message) { return std::unexpected(mstar::Error{code, std::move(message)}); };
        auto part = nand.identify();
        if (!part) return std::unexpected(part.error());
        // A first-connection issue may require another invocation, but never
        // retry erase/program automatically after a possibly completed transfer.
        std::array<uint8_t, 8> id{};
        if (auto r = nand.readJedecId(id); !r) return r;
        if (!std::equal(part->id.begin(), part->id.begin() + part->idLength, id.begin()))
            return error(mstar::ErrorCode::FlashProtocol, "JEDEC ID changed between consecutive reads");
        const auto& g = part->geometry;
        const uint64_t blockBytes = uint64_t(g.pageSize) * g.pagesPerBlock;
        const uint64_t capacity = blockBytes * g.blocksPerLun * g.luns;
        if (options.offset > capacity || image.size() > capacity - options.offset)
            return error(mstar::ErrorCode::InvalidArgument, "Image range exceeds flash capacity");
        const uint64_t alignment = writing ? blockBytes : g.pageSize;
        if (options.offset % alignment || image.size() % alignment)
            return error(mstar::ErrorCode::InvalidArgument,
                         fmt::format("{} requires offset and image size aligned to {} bytes",
                                     writing ? "Write" : "Verify", alignment));
        if (auto r = nand.configureRead(*part); !r) return r;
        const auto firstPage = static_cast<uint32_t>(options.offset / g.pageSize);
        const auto pageCount = static_cast<uint32_t>(image.size() / g.pageSize);
        fmt::println("Flash: {} {}; {} bytes at offset {:#x}", part->vendor, part->model, image.size(), options.offset);
        if (writing) {
            const auto firstBlock = firstPage / g.pagesPerBlock;
            const auto blockCount = pageCount / g.pagesPerBlock;
            for (uint32_t block = firstBlock; block < firstBlock + blockCount; ++block) {
                auto bad = nand.isBadBlock(block);
                if (!bad) return std::unexpected(bad.error());
                if (*bad) return error(mstar::ErrorCode::BadBlock,
                    fmt::format("Marked block {} in requested range; no blocks erased", block));
            }
            // The backup is headerless, interleaved main+physical OOB, with a
            // separate manifest. noreplace also protects against path races.
            std::ofstream backup(options.backup, std::ios::binary | std::ios::out | std::ios::noreplace);
            if (!backup) return error(mstar::ErrorCode::Io, "Cannot create new raw backup file");
            std::ofstream manifest(options.backup + ".json", std::ios::out | std::ios::noreplace);
            if (!manifest) return error(mstar::ErrorCode::Io, "Cannot create new backup manifest");
            nlohmann::json metadata{
                {"format", "mstar-raw-pages-v1"}, {"vendor", part->vendor}, {"model", part->model},
                {"jedec_id", std::vector<uint8_t>(part->id.begin(), part->id.begin() + part->idLength)},
                {"main_area_offset", options.offset}, {"main_area_size", image.size()},
                {"page_size", g.pageSize}, {"physical_oob_size", g.oobSize},
                {"pages_per_block", g.pagesPerBlock}, {"ecc_enabled_during_backup", false}
            };
            manifest << metadata.dump(2) << '\n';
            manifest.flush();
            manifest.close();
            if (!manifest) return error(mstar::ErrorCode::Io, "Failed to write backup manifest");
            std::vector<uint8_t> page(g.pageSize), raw(size_t(g.pageSize) + g.oobSize);
            for (uint32_t p = firstPage; p < firstPage + pageCount; ++p) {
                // Refuse unreadable source data rather than erasing the only copy.
                if (auto r = nand.readPage(p, page); !r) return std::unexpected(r.error());
                if (auto r = nand.readRawPage(p, raw); !r) return r;
                backup.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
                if (!backup) return error(mstar::ErrorCode::Io, "Failed to write raw backup; no blocks erased");
            }
            backup.flush();
            backup.close();
            if (!backup) return error(mstar::ErrorCode::Io, "Failed to finish raw backup; no blocks erased");
            // Compare the saved backup against a second raw read before any erase.
            std::ifstream saved(options.backup, std::ios::binary);
            if (!saved) return error(mstar::ErrorCode::Io, "Cannot reopen raw backup for verification");
            std::vector<uint8_t> savedPage(raw.size());
            for (uint32_t p = firstPage; p < firstPage + pageCount; ++p) {
                saved.read(reinterpret_cast<char*>(savedPage.data()), static_cast<std::streamsize>(savedPage.size()));
                if (!saved) return error(mstar::ErrorCode::Io, "Incomplete raw backup; no blocks erased");
                if (auto r = nand.readRawPage(p, raw); !r) return r;
                if (raw != savedPage) return error(mstar::ErrorCode::VerifyMismatch,
                    fmt::format("Backup differs from second raw read at page {}; no blocks erased", p));
            }
            if (saved.peek() != std::char_traits<char>::eof() || saved.bad())
                return error(mstar::ErrorCode::Io, "Unexpected raw backup length/read error");
            fmt::println("Raw backup verified: {} (+ .json); {} block(s)", options.backup, blockCount);
            if (options.dryRun) return {};
            if (auto r = nand.configureWrite(); !r) return r;
            for (uint32_t block = firstBlock; block < firstBlock + blockCount; ++block) {
                destructiveStarted = true; // even an unsuccessful erase may reach flash
                if (auto r = nand.eraseBlock(block); !r) return r;
                for (uint32_t p = block * g.pagesPerBlock; p < (block + 1) * g.pagesPerBlock; ++p) {
                    auto bytes = std::span<const uint8_t>(image).subspan(size_t(p - firstPage) * g.pageSize, g.pageSize);
                    if (auto r = nand.programPage(p, bytes); !r) return r;
                }
                fmt::println("Programmed block {}", block);
            }
        }
        uint32_t corrected = 0;
        for (uint32_t p = firstPage; p < firstPage + pageCount; ++p) {
            auto bytes = std::span<const uint8_t>(image).subspan(size_t(p - firstPage) * g.pageSize, g.pageSize);
            auto result = nand.verifyPage(p, bytes);
            if (!result) return std::unexpected(result.error());
            if (result->state != mstar::EccState::Clean) ++corrected;
        }
        fmt::println("Compared {} bytes; {} page(s) required ECC correction", image.size(), corrected);
        return {};
    };
    mstar::Result<void> operation;
    try {
        operation = perform();
    } catch (const std::exception& error) {
        // In particular, allocation/file-system exceptions after setup must
        // still reach flash-settings and ISP cleanup.
        operation = std::unexpected(mstar::Error{mstar::ErrorCode::Io, error.what()});
    }
    auto cleanup = nand.restoreReadConfiguration();
    if (!cleanup) {
        if (operation) operation = cleanup;
        else operation.error().message += "; cleanup: " + cleanup.error().message;
    }
    if (!options.stay) {
        if (auto r = isp.leave(); !r) {
            if (operation) operation = r;
            else operation.error().message += "; ISP exit: " + r.error().message;
        }
    }
    if (!operation) {
        fmt::println(stderr, "{} failed: {}", writing ? "Write" : "Verify", operation.error().message);
        if (destructiveStarted)
            fmt::println(stderr, "Flash may be partially programmed. Retain raw backup {} and its manifest.", options.backup);
        return 1;
    }
    fmt::println("{} successful{}", writing ? (options.dryRun ? "Write dry run (no erase/program)" : "Write and verification") : "Verification",
                 options.stay ? "; ISP exit omitted" : "");
    return 0;
#else
    (void)options;
    (void)writing;
    fmt::println(stderr, "No programmer backends are enabled in this build.");
    return 1;
#endif
}
}

void addWriteCommands(CLI::App& app) {
    for (bool writing : {true, false}) {
        auto options = std::make_shared<Options>();
        auto* command = app.add_subcommand(writing ? "write" : "verify", writing ?
            "Back up, erase, program and verify a block-aligned main-area NAND image" :
            "Compare a page-aligned main-area image with flash using ECC-checked reads");
        command->add_option("image", options->image, "Main-area image (no interleaved OOB)")->required();
        command->add_option("--serial", options->serial, writing ?
            "Programmer serial shown by list (default: first detected programmer)" :
            "Programmer serial shown by list");
        if (writing) {
            command->add_option("--backup", options->backup, "New raw main+OOB backup path; also creates PATH.json")->required();
            command->add_flag("--yes", options->yes, "Confirm erasing/programming the image range");
            command->add_flag("--dry-run", options->dryRun, "Check markers and save/verify the backup without erasing/programming");
        }
        command->add_option("--offset", options->offset, "Main-area byte offset")->default_val(0);
        command->add_option("--i2c-address", options->address, "ISP I2C address")->default_val(0x49)->check(CLI::Range(0, 0x7F));
        command->add_option("--i2c-clock", options->clock, "I2C clock in Hz")->default_val(100000);
        command->add_flag("--trace-i2c", options->trace, "Trace I2C transactions to stderr");
        command->add_flag("--stay-in-isp", options->stay, "Omit ISP exit after restoring flash settings");
        command->callback([options, writing] { std::exit(runImage(*options, writing)); });
    }
}
