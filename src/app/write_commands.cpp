#include "write_commands.hpp"
#include "write_plan.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <string_view>
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
// Report completed work on stderr, including when output is redirected.
// Updates follow completed pages/blocks; a blocked driver call cannot advance them.
class Progress {
public:
    Progress(std::string_view stage, uint64_t total, std::string_view unit,
             std::string_view item)
        : stage_(stage), total_(total), unit_(unit), item_(item) {
        fmt::println(stderr, "{}: 0/{} {} (0.0%)", stage_, total_, unit_);
        std::fflush(stderr);
    }

    void advance(uint32_t deviceIndex) {
        ++completed_;
        const auto now = Clock::now();
        if (completed_ != 1 && completed_ != total_ &&
            now - lastReport_ < std::chrono::seconds(1)) return;
        const double seconds = std::chrono::duration<double>(now - started_).count();
        const double rate = seconds > 0 ? completed_ / seconds : 0;
        const double remaining = rate > 0 ? (total_ - completed_) / rate : 0;
        fmt::println(stderr,
            "{}: {}/{} {} ({:.1f}%), last {} {}, elapsed {:.1f}s, ETA {:.0f}s",
            stage_, completed_, total_, unit_, 100.0 * completed_ / total_,
            item_, deviceIndex, seconds, remaining);
        std::fflush(stderr);
        lastReport_ = now;
    }

private:
    using Clock = std::chrono::steady_clock;
    std::string_view stage_;
    uint64_t total_;
    std::string_view unit_;
    std::string_view item_;
    uint64_t completed_ = 0;
    Clock::time_point started_ = Clock::now();
    Clock::time_point lastReport_ = started_;
};

struct Options {
    std::string image;
    std::string config;
    std::string backup;
    std::string serial;
    uint64_t offset = 0;
    uint32_t address = 0x49;
    uint32_t clock = 100000;
    bool dryRun = false;
    bool preserveGaps = false;
    bool trace = false;
    bool stay = false;
};

int runImage(const Options& options, bool writing) {
#ifdef MSTAR_ENABLE_FTDI
    const bool backingUp = writing && !options.backup.empty();
    // Snapshot the input before touching hardware; a file change during backup
    // cannot change the bytes that are programmed or verified.
    uint64_t maximum = 0;
    for (const auto& part : mstar::builtinFlashTable()) {
        const auto& g = part.geometry;
        maximum = std::max(maximum, uint64_t(g.pageSize) * g.pagesPerBlock * g.blocksPerLun * g.luns);
    }
    mstar::Result<std::vector<mstar::app::WriteImage>> loaded;
    try {
        loaded = mstar::app::loadWriteImages(options.image, options.config, options.offset, maximum);
    } catch (const std::exception& error) {
        fmt::println(stderr, "Cannot load images: {}", error.what());
        return 1;
    }
    if (!loaded) { fmt::println(stderr, "{}", loaded.error().message); return 1; }
    auto images = std::move(*loaded);
    if (backingUp) {
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
    fmt::println(stderr, "Opening programmer...");
    std::fflush(stderr);
    auto transport = mstar::ftdi::FtdiI2c::open(selector);
    if (!transport) { fmt::println(stderr, "Failed to open programmer: {}", transport.error().message); return 1; }
    mstar::TracingI2cMaster bus(*transport, options.trace ? &std::cerr : nullptr);
    if (auto r = bus.setClock(options.clock); !r) { fmt::println(stderr, "{}", r.error().message); return 1; }
    mstar::MstarIsp isp(bus, static_cast<uint8_t>(options.address));
    fmt::println(stderr, "Entering ISP...");
    std::fflush(stderr);
    if (auto r = isp.enter(); !r) { fmt::println(stderr, "ISP activation failed: {}", r.error().message); return 1; }
    mstar::SpiNand nand(isp);
    bool destructiveStarted = false;
    auto perform = [&]() -> mstar::Result<void> {
        auto error = [](mstar::ErrorCode code, std::string message) { return std::unexpected(mstar::Error{code, std::move(message)}); };
        fmt::println(stderr, "Identifying flash...");
        std::fflush(stderr);
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
        if (auto r = mstar::app::prepareWriteImages(images, capacity,
                writing ? blockBytes : g.pageSize, !options.config.empty(), writing); !r) return r;
        fmt::println(stderr, "Configuring ECC reads...");
        std::fflush(stderr);
        if (auto r = nand.configureRead(*part); !r) return r;
        uint64_t totalBytes = 0;
        fmt::println("Flash: {} {}", part->vendor, part->model);
        if (!options.config.empty())
            fmt::println("Config tail policy: {}; whole blocks between images are unchanged",
                         options.preserveGaps ? "preserve existing main bytes" : "fill with FF");
        for (const auto& image : images) {
            fmt::println("Image: {}; {} bytes at offset {:#x}; {} bytes including padding",
                         image.path.string(), image.sourceSize, image.offset, image.bytes.size());
            totalBytes += image.bytes.size();
        }
        // One page list drives backup, programming and verification so sparse
        // config ranges never erase or copy the whole span between images.
        struct Page {
            uint32_t number;
            std::span<uint8_t> bytes;
            size_t sourceBytes;
        };
        std::vector<Page> pages;
        std::vector<uint32_t> blocks;
        for (auto& image : images) {
            const auto firstPage = static_cast<uint32_t>(image.offset / g.pageSize);
            const auto pageCount = static_cast<uint32_t>(image.bytes.size() / g.pageSize);
            for (uint32_t p = 0; p < pageCount; ++p) {
                const size_t byteOffset = size_t(p) * g.pageSize;
                const auto sourceBytes = byteOffset < image.sourceSize ?
                    std::min<uint64_t>(g.pageSize, image.sourceSize - byteOffset) : 0;
                pages.push_back({firstPage + p, std::span(image.bytes).subspan(byteOffset, g.pageSize),
                                 static_cast<size_t>(sourceBytes)});
                if (writing && p % g.pagesPerBlock == 0) blocks.push_back((firstPage + p) / g.pagesPerBlock);
            }
        }
        std::fflush(stdout);
        if (writing) {
            fmt::println(stderr, "Write plan: {} blocks, {} pages; backup {}",
                         blocks.size(), pages.size(), backingUp ? "enabled" : "disabled");
            if (backingUp)
                fmt::println(stderr, "Raw backup: {} bytes; saved and compared before erase",
                             uint64_t(pages.size()) * (uint64_t(g.pageSize) + g.oobSize));
            Progress markers("Checking bad-block markers", blocks.size(), "blocks", "block");
            for (const auto block : blocks) {
                auto bad = nand.isBadBlock(block);
                if (!bad) return std::unexpected(bad.error());
                if (*bad) return error(mstar::ErrorCode::BadBlock,
                    fmt::format("Marked block {} in requested range; no blocks erased", block));
                markers.advance(block);
            }
            if (backingUp) {
                // The backup is headerless, interleaved main+physical OOB, with a
                // separate manifest. noreplace also protects against path races.
                std::ofstream backup(options.backup, std::ios::binary | std::ios::out | std::ios::noreplace);
                if (!backup) return error(mstar::ErrorCode::Io, "Cannot create new raw backup file");
                std::ofstream manifest(options.backup + ".json", std::ios::out | std::ios::noreplace);
                if (!manifest) return error(mstar::ErrorCode::Io, "Cannot create new backup manifest");
                nlohmann::json metadata{
                    {"format", "mstar-raw-pages-v1"}, {"vendor", part->vendor}, {"model", part->model},
                    {"jedec_id", std::vector<uint8_t>(part->id.begin(), part->id.begin() + part->idLength)},
                    {"page_size", g.pageSize}, {"physical_oob_size", g.oobSize},
                    {"pages_per_block", g.pagesPerBlock}, {"ecc_enabled_during_backup", false}
                };
                if (options.config.empty()) {
                    metadata["main_area_offset"] = images.front().offset;
                    metadata["main_area_size"] = images.front().bytes.size();
                } else {
                    metadata["format"] = "mstar-raw-ranges-v2";
                    metadata["config"] = options.config;
                    metadata["tail_policy"] = options.preserveGaps ? "preserve" : "fill-ff";
                    metadata["ranges"] = nlohmann::json::array();
                    uint64_t backupOffset = 0;
                    for (const auto& image : images) {
                        const uint64_t rawSize = (image.bytes.size() / g.pageSize) * (uint64_t(g.pageSize) + g.oobSize);
                        metadata["ranges"].push_back({
                            {"image", image.path.string()}, {"image_size", image.sourceSize},
                            {"main_area_offset", image.offset}, {"main_area_size", image.bytes.size()},
                            {"backup_offset", backupOffset}, {"backup_size", rawSize}
                        });
                        backupOffset += rawSize;
                    }
                }
                manifest << metadata.dump(2) << '\n';
                manifest.flush();
                manifest.close();
                if (!manifest) return error(mstar::ErrorCode::Io, "Failed to write backup manifest");
                std::vector<uint8_t> page(g.pageSize), raw(size_t(g.pageSize) + g.oobSize);
                Progress backupProgress("Backing up flash", pages.size(), "pages", "page");
                for (const auto& planned : pages) {
                    const auto p = planned.number;
                    // Refuse unreadable source data rather than erasing the only copy.
                    if (auto r = nand.readPage(p, page); !r) return std::unexpected(r.error());
                    if (options.preserveGaps) {
                        // Preserve ECC-corrected main bytes, not potentially faulty
                        // raw bytes; parity is regenerated when the block is written.
                        std::copy(page.begin() + planned.sourceBytes, page.end(),
                                  planned.bytes.begin() + planned.sourceBytes);
                    }
                    if (auto r = nand.readRawPage(p, raw); !r) return r;
                    backup.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
                    if (!backup) return error(mstar::ErrorCode::Io, "Failed to write raw backup; no blocks erased");
                    backupProgress.advance(p);
                }
                backup.flush();
                backup.close();
                if (!backup) return error(mstar::ErrorCode::Io, "Failed to finish raw backup; no blocks erased");
                // Compare the saved backup against a second raw read before any erase.
                std::ifstream saved(options.backup, std::ios::binary);
                if (!saved) return error(mstar::ErrorCode::Io, "Cannot reopen raw backup for verification");
                std::vector<uint8_t> savedPage(raw.size());
                Progress backupCompare("Comparing backup with flash", pages.size(), "pages", "page");
                for (const auto& planned : pages) {
                    const auto p = planned.number;
                    saved.read(reinterpret_cast<char*>(savedPage.data()), static_cast<std::streamsize>(savedPage.size()));
                    if (!saved) return error(mstar::ErrorCode::Io, "Incomplete raw backup; no blocks erased");
                    if (auto r = nand.readRawPage(p, raw); !r) return r;
                    if (raw != savedPage) return error(mstar::ErrorCode::VerifyMismatch,
                        fmt::format("Backup differs from second raw read at page {}; no blocks erased", p));
                    backupCompare.advance(p);
                }
                if (saved.peek() != std::char_traits<char>::eof() || saved.bad())
                    return error(mstar::ErrorCode::Io, "Unexpected raw backup length/read error");
                fmt::println("Raw backup verified: {} (+ .json); {} block(s)", options.backup, blocks.size());
                std::fflush(stdout);
            } else if (options.preserveGaps) {
                const auto tailPages = std::count_if(pages.begin(), pages.end(),
                    [](const Page& p) { return p.sourceBytes < p.bytes.size(); });
                if (tailPages != 0) {
                    Progress preserving("Reading preserved tails", tailPages, "pages", "page");
                    std::vector<uint8_t> page(g.pageSize);
                    // Read every preserved tail before the first erase, even
                    // when no backup was requested. Fully replaced pages need no read.
                    for (const auto& planned : pages) {
                        if (planned.sourceBytes == planned.bytes.size()) continue;
                        if (auto r = nand.readPage(planned.number, page); !r)
                            return std::unexpected(r.error());
                        std::copy(page.begin() + planned.sourceBytes, page.end(),
                                  planned.bytes.begin() + planned.sourceBytes);
                        preserving.advance(planned.number);
                    }
                }
            }
            if (options.dryRun) return {};
            fmt::println(stderr, "Configuring flash for programming...");
            std::fflush(stderr);
            if (auto r = nand.configureWrite(); !r) return r;
            Progress programming("Programming flash", pages.size(), "pages", "page");
            for (const auto& planned : pages) {
                if (planned.number % g.pagesPerBlock == 0) {
                    fmt::println(stderr, "Erasing block {}...", planned.number / g.pagesPerBlock);
                    std::fflush(stderr);
                    destructiveStarted = true; // even an unsuccessful erase may reach flash
                    if (auto r = nand.eraseBlock(planned.number / g.pagesPerBlock); !r) return r;
                }
                if (auto r = nand.programPage(planned.number, planned.bytes); !r) return r;
                programming.advance(planned.number);
                if ((planned.number + 1) % g.pagesPerBlock == 0)
                    fmt::println("Programmed block {}", planned.number / g.pagesPerBlock);
            }
        }
        std::fflush(stdout);
        Progress verification("Verifying image", pages.size(), "pages", "page");
        uint32_t corrected = 0;
        for (const auto& planned : pages) {
            auto result = nand.verifyPage(planned.number, planned.bytes);
            if (!result) return std::unexpected(result.error());
            if (result->state != mstar::EccState::Clean) ++corrected;
            verification.advance(planned.number);
        }
        fmt::println("Compared {} bytes; {} page(s) required ECC correction", totalBytes, corrected);
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
    std::fflush(stdout);
    fmt::println(stderr, "Restoring flash settings...");
    std::fflush(stderr);
    auto cleanup = nand.restoreReadConfiguration();
    if (!cleanup) {
        if (operation) operation = cleanup;
        else operation.error().message += "; cleanup: " + cleanup.error().message;
    }
    if (!options.stay) {
        fmt::println(stderr, "Leaving ISP...");
        std::fflush(stderr);
        if (auto r = isp.leave(); !r) {
            if (operation) operation = r;
            else operation.error().message += "; ISP exit: " + r.error().message;
        }
    }
    if (!operation) {
        fmt::println(stderr, "{} failed: {}", writing ? "Write" : "Verify", operation.error().message);
        if (destructiveStarted) {
            fmt::println(stderr, "Flash may be partially programmed.");
            if (backingUp)
                fmt::println(stderr, "Retain raw backup {} and its manifest.", options.backup);
        } else if (writing) {
            fmt::println(stderr, "No erase/program commands were issued.");
            if (backingUp)
                fmt::println(stderr, "Any backup files may be incomplete or unverified.");
        }
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
            "Erase, program and verify a NAND image or vendor config; optionally back up first" :
            "Compare a page-aligned main-area image with flash using ECC-checked reads");
        auto* image = command->add_option("image", options->image, "Main-area image (no interleaved OOB)");
        auto* offset = command->add_option("--offset", options->offset, "Main-area byte offset (decimal or 0x hex)")->default_val(0);
        if (writing) {
            auto* config = command->add_option("--config", options->config,
                "Vendor filename : offset config; relative image paths use the config directory; pad tails with FF");
            config->excludes(image);
            config->excludes(offset);
            command->add_flag("--preserve-gaps", options->preserveGaps,
                "With --config, preserve main bytes after each image in its final erase block instead of filling FF")->needs(config);
        } else image->required();
        command->add_option("--serial", options->serial, writing ?
            "Programmer serial shown by list (default: first detected programmer)" :
            "Programmer serial shown by list");
        if (writing) {
            command->add_option("--backup", options->backup,
                "Optional new raw main+OOB backup path; saves and verifies PATH and creates PATH.json");
            command->add_flag("--dry-run", options->dryRun,
                "Check the write plan and markers, read preserved tails and any requested backup, without erasing/programming");
        }
        command->add_option("--i2c-address", options->address, "ISP I2C address")->default_val(0x49)->check(CLI::Range(0, 0x7F));
        command->add_option("--i2c-clock", options->clock, "I2C clock in Hz")->default_val(100000);
        command->add_flag("--trace-i2c", options->trace, "Trace I2C transactions to stderr");
        command->add_flag("--stay-in-isp", options->stay, "Omit ISP exit after restoring flash settings");
        command->callback([options, writing] {
            if (writing && options->image.empty() && options->config.empty()) {
                fmt::println(stderr, "Write requires an image or --config");
                std::exit(1);
            }
            std::exit(runImage(*options, writing));
        });
    }
}
