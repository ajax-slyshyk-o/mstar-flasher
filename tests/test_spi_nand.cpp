#include <algorithm>
#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "mstar/flash/spi_nand.hpp"
#include "mock_spi_bus.hpp"

using namespace mstar;
using namespace mstar::test;

namespace {

FlashPart makeTestPart() {
    return FlashPart{
        .vendor = "TestVendor",
        .model = "TV128M",
        .id = {0xEF, 0xAA, 0x21, 0x00},
        .idLength = 3,
        .geometry = {.pageSize = 2048, .oobSize = 64, .pagesPerBlock = 64, .blocksPerLun = 1024, .luns = 1},
        .eccStepSize = 512,
        .eccStrength = 1,
    };
}

} // namespace

TEST_CASE("SpiNand::reset sends the RESET opcode and waits for ready", "[spi_nand]") {
    MockSpiBus bus;
    SpiNand nand(bus);
    bus.queueResponse({0x00});

    REQUIRE(nand.reset().has_value());

    REQUIRE(bus.transactions.size() == 2);
    CHECK(bus.transactions[0].tx == std::vector<uint8_t>{0xFF});
    CHECK(bus.transactions[0].rx.empty());
    CHECK(bus.transactions[1].tx == std::vector<uint8_t>{0x0F, 0xC0});
    CHECK(bus.transactions[1].rx == std::vector<uint8_t>{0x00});
}

TEST_CASE("SpiNand::readJedecId issues the READ JEDEC ID opcode and returns the requested bytes", "[spi_nand]") {
    MockSpiBus bus;
    SpiNand nand(bus);
    bus.queueResponse({0xEF, 0xAA, 0x21, 0x00});

    std::array<uint8_t, 4> id{};
    REQUIRE(nand.readJedecId(id).has_value());
    CHECK(id == std::array<uint8_t, 4>{0xEF, 0xAA, 0x21, 0x00});

    REQUIRE(bus.transactions.size() == 1);
    CHECK(bus.transactions[0].tx == std::vector<uint8_t>{0x9F, 0x00});
}

TEST_CASE("SpiNand::identify resets, reads the JEDEC ID, and matches a supplied table", "[spi_nand]") {
    MockSpiBus bus;
    SpiNand nand(bus);
    bus.queueResponse({0x00}); // waitReady during reset()
    bus.queueResponse({0xEF, 0xAA, 0x21, 0x00}); // JEDEC ID

    auto result = nand.identify({makeTestPart()});
    REQUIRE(result.has_value());
    CHECK(result->vendor == "TestVendor");
    CHECK(result->model == "TV128M");
}

TEST_CASE("SpiNand::identify fails with FlashUnknown when the JEDEC ID matches no table entry", "[spi_nand]") {
    MockSpiBus bus;
    SpiNand nand(bus);
    bus.queueResponse({0x00});
    bus.queueResponse({0x11, 0x22, 0x33, 0x44});

    auto result = nand.identify({makeTestPart()});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::FlashUnknown);
}

TEST_CASE("SpiNand::identify propagates a transaction failure during reset", "[spi_nand]") {
    MockSpiBus bus;
    SpiNand nand(bus);
    bus.failNextTransaction = true;

    auto result = nand.identify({makeTestPart()});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::FlashProtocol);
}

TEST_CASE("SpiNand::waitReady times out if the operation-in-progress bit never clears", "[spi_nand]") {
    MockSpiBus bus;
    SpiNand nand(bus);
    bus.queueResponse({0x01}); // OIP set forever (sticky last response)

    auto result = nand.reset();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::Timeout);
}

namespace {
FlashPart smallPart(NandEccProfile profile = NandEccProfile::Gd5f2gm7) {
    const auto& table = builtinFlashTable();
    auto it = std::find_if(table.begin(), table.end(),
                          [profile](const auto& part) { return part.eccProfile == profile; });
    REQUIRE(it != table.end());
    auto part = *it;
    part.geometry = {.pageSize = 4, .oobSize = 0, .pagesPerBlock = 16, .blocksPerLun = 1, .luns = 1};
    return part;
}
uint8_t enabledConfig(const FlashPart& part) {
    return part.eccProfile == NandEccProfile::Gd5f2gm7 ? 0x10 : 0x18;
}
void configure(MockSpiBus& bus, SpiNand& nand, const FlashPart& part) {
    bus.queueResponse({enabledConfig(part)});
    REQUIRE(nand.configureRead(part));
    bus.clearResponses();
    bus.transactions.clear();
}
bool sent(const MockSpiBus& bus, std::vector<uint8_t> bytes) {
    return std::any_of(bus.transactions.begin(), bus.transactions.end(),
                       [&](const auto& t) { return t.tx == bytes; });
}
}

TEST_CASE("Every built-in part has a verified ECC profile and strength", "[spi_nand][ecc]") {
    for (const auto& part : builtinFlashTable()) {
        DYNAMIC_SECTION(part.model) {
            MockSpiBus bus;
            SpiNand nand(bus);
            configure(bus, nand, part);
        }
    }
}

TEST_CASE("ECC configuration preserves unrelated bits and restores only owned bits", "[spi_nand][ecc]") {
    auto profile = GENERATE(NandEccProfile::Gd5f2gm7, NandEccProfile::Gd5f1gm9,
                           NandEccProfile::W25n01gv, NandEccProfile::W25n01kv,
                           NandEccProfile::W25n02kv);
    MockSpiBus bus;
    SpiNand nand(bus);
    auto part = smallPart(profile);
    const uint8_t initial = 0x81; // OTP protection and QE/drive setting preserved
    const uint8_t enabled = initial | enabledConfig(part);
    bus.queueResponse({initial});
    bus.queueResponse({0x01}); // busy after feature write
    bus.queueResponse({0x00});
    bus.queueResponse({enabled});
    REQUIRE(nand.configureRead(part));
    CHECK(sent(bus, {0x1F, 0xB0, enabled}));
    if (profile == NandEccProfile::Gd5f2gm7) CHECK((enabled & 8) == 0); // don't set BPL!
    bus.clearResponses();
    // An unrelated bit changed after setup: restore must preserve it.
    bus.queueResponse({static_cast<uint8_t>(enabled & ~1)});
    bus.queueResponse({0x00});
    bus.queueResponse({0x80});
    REQUIRE(nand.restoreReadConfiguration());
    CHECK(sent(bus, {0x1F, 0xB0, 0x80}));
    const auto count = bus.transactions.size();
    REQUIRE(nand.restoreReadConfiguration());
    CHECK(bus.transactions.size() == count);
    std::array<uint8_t, 4> buffer{};
    CHECK_FALSE(nand.readPage(0, buffer));
}

TEST_CASE("Already enabled ECC requires no configuration writes", "[spi_nand][ecc]") {
    MockSpiBus bus;
    SpiNand nand(bus);
    auto part = smallPart();
    bus.queueResponse({0x19}); // GM7 BPL/QE must remain set
    REQUIRE(nand.configureRead(part));
    REQUIRE(nand.restoreReadConfiguration());
    REQUIRE(bus.transactions.size() == 1);
    CHECK(bus.transactions[0].tx == std::vector<uint8_t>{0x0F, 0xB0});
}

TEST_CASE("Unknown ECC profiles and active OTP cannot authorize a read", "[spi_nand][ecc]") {
    MockSpiBus bus;
    SpiNand nand(bus);
    auto part = smallPart();
    SECTION("unknown profile") { part.eccProfile = NandEccProfile::Unknown; }
    SECTION("inconsistent strength") { part.eccStrength = 0; }
    SECTION("OTP access") { bus.queueResponse({0x50}); }
    auto result = nand.configureRead(part);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::FlashProtocol);
    CHECK(bus.transactions.size() <= 1);
    std::array<uint8_t, 4> buffer{};
    CHECK_FALSE(nand.readPage(0, buffer));
}

TEST_CASE("Configuration errors propagate and partial writes remain restorable", "[spi_nand][ecc]") {
    const auto failingTransaction = GENERATE(size_t{0}, size_t{1}, size_t{2}, size_t{3});
    MockSpiBus bus;
    SpiNand nand(bus);
    bus.queueResponse({0});
    bus.queueResponse({0});
    bus.queueResponse({0x10});
    bus.failTransactionAt = failingTransaction;
    auto result = nand.configureRead(smallPart());
    REQUIRE_FALSE(result);
    CHECK(result.error().message == "mock SPI transaction failure");
    bus.failTransactionAt.reset();
    bus.clearResponses();
    bus.queueResponse({0x10});
    bus.queueResponse({0});
    bus.queueResponse({0});
    REQUIRE(nand.restoreReadConfiguration());
    CHECK(sent(bus, {0x1F, 0xB0, 0}) == (failingTransaction != 0));
}

TEST_CASE("Configuration readback mismatch and timeout fail closed", "[spi_nand][ecc]") {
    MockSpiBus bus;
    SpiNand nand(bus);
    bus.queueResponse({0});
    SECTION("write ignored") {
        bus.queueResponse({0});
        auto result = nand.configureRead(smallPart());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::FlashProtocol);
    }
    SECTION("busy never clears") {
        bus.queueResponse({1});
        auto result = nand.configureRead(smallPart());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::Timeout);
    }
}

TEST_CASE("Failed restore is reported and can be retried", "[spi_nand][ecc]") {
    MockSpiBus bus;
    SpiNand nand(bus);
    bus.queueResponse({0});
    bus.queueResponse({0});
    bus.queueResponse({0x10});
    REQUIRE(nand.configureRead(smallPart()));
    bus.clearResponses();
    SECTION("transport error") { bus.failNextTransaction = true; }
    SECTION("readback mismatch") { bus.queueResponse({0x10}); }
    auto result = nand.restoreReadConfiguration();
    REQUIRE_FALSE(result);
    bus.clearResponses();
    bus.queueResponse({0x10});
    bus.queueResponse({0});
    bus.queueResponse({0});
    REQUIRE(nand.restoreReadConfiguration());
}

TEST_CASE("Page reads use each part's ECC status instead of a shared interpretation", "[spi_nand][ecc]") {
    const auto status = GENERATE(uint8_t{0x00}, uint8_t{0x10}, uint8_t{0x20}, uint8_t{0x30});
    for (auto profile : {NandEccProfile::Gd5f2gm7, NandEccProfile::Gd5f1gm9,
                         NandEccProfile::W25n01gv, NandEccProfile::W25n01kv,
                         NandEccProfile::W25n02kv}) {
        DYNAMIC_SECTION(static_cast<int>(profile) << ": status " << int(status)) {
            MockSpiBus bus;
            SpiNand nand(bus);
            auto part = smallPart(profile);
            configure(bus, nand, part);
            bus.queueResponse({enabledConfig(part)});
            bus.queueResponse({static_cast<uint8_t>(status | 1)}); // busy status ignored
            bus.queueResponse({status});
            const bool giga = profile == NandEccProfile::Gd5f2gm7 || profile == NandEccProfile::Gd5f1gm9;
            const bool gv = profile == NandEccProfile::W25n01gv;
            if (status == 0x10 && giga) bus.queueResponse({0}); // corrected 1..4
            if ((status == 0x10 || status == 0x30) && !giga && !gv) bus.queueResponse({0x30});
            bus.queueResponse({0xDE, 0xAD, 0xBE, 0xEF});
            std::array<uint8_t, 4> buffer{0xAA, 0xAA, 0xAA, 0xAA};
            auto result = nand.readPage(5, buffer);
            CHECK(sent(bus, {0x13, 0, 0, 5}));
            if (status == 0x20 || (gv && status == 0x30)) {
                REQUIRE_FALSE(result);
                CHECK(result.error().code == (status == 0x20 ? ErrorCode::EccUncorrectable : ErrorCode::FlashProtocol));
                CHECK_FALSE(sent(bus, {0x03, 0, 0, 0}));
                CHECK(buffer == std::array<uint8_t, 4>{0xAA, 0xAA, 0xAA, 0xAA});
            } else {
                REQUIRE(result);
                CHECK(sent(bus, {0x03, 0, 0, 0}));
                CHECK(buffer == std::array<uint8_t, 4>{0xDE, 0xAD, 0xBE, 0xEF});
                CHECK(result->state == (status == 0 ? EccState::Clean :
                    status == 0x30 || gv ? EccState::Threshold : EccState::Corrected));
                if (status == 0) CHECK(result->correctedBitsMax == 0);
                else if (gv) CHECK(result->correctedBitsMax == 1);
                else if (giga) CHECK(result->correctedBitsMax == (status == 0x30 ? 8 : 4));
                else CHECK(result->correctedBitsMax == 3);
            }
        }
    }
}

TEST_CASE("GigaDevice extended ECC reports ranges and exact counts from F0", "[spi_nand][ecc]") {
    const auto profile = GENERATE(NandEccProfile::Gd5f2gm7, NandEccProfile::Gd5f1gm9);
    const auto detail = GENERATE(0, 1, 2, 3);
    MockSpiBus bus;
    SpiNand nand(bus);
    auto part = smallPart(profile);
    configure(bus, nand, part);
    bus.queueResponse({enabledConfig(part)});
    bus.queueResponse({0x10});
    bus.queueResponse({static_cast<uint8_t>((detail << 4) | 0xCF)}); // ignore unrelated bits
    bus.queueResponse({1, 2, 3, 4});
    std::array<uint8_t, 4> buffer{};
    auto result = nand.readPage(0, buffer);
    REQUIRE(result);
    CHECK(result->correctedBitsMin == (detail ? 4 + detail : 1));
    CHECK(result->correctedBitsMax == 4 + detail);
    CHECK(sent(bus, {0x0F, 0xF0}));
}

TEST_CASE("Winbond KV validates extended counts and masks reserved bits", "[spi_nand][ecc]") {
    const auto profile = GENERATE(NandEccProfile::W25n01kv, NandEccProfile::W25n02kv);
    const auto bits = GENERATE(0, 1, 4, 5, 8, 15);
    MockSpiBus bus;
    SpiNand nand(bus);
    auto part = smallPart(profile);
    configure(bus, nand, part);
    bus.queueResponse({enabledConfig(part)});
    bus.queueResponse({0x10});
    const auto raw = static_cast<uint8_t>((bits << 4) | 0x0F |
        (profile == NandEccProfile::W25n01kv ? 0x80 : 0));
    bus.queueResponse({raw});
    bus.queueResponse({1, 2, 3, 4});
    std::array<uint8_t, 4> buffer{};
    auto result = nand.readPage(0, buffer);
    if (bits == 0 || bits > int(part.eccStrength)) {
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::FlashProtocol);
        CHECK_FALSE(sent(bus, {0x03, 0, 0, 0}));
    } else {
        REQUIRE(result);
        CHECK(result->correctedBitsMin == bits);
        CHECK(result->correctedBitsMax == bits);
    }
    CHECK(sent(bus, {0x0F, 0x30}));
}

TEST_CASE("A changed read configuration or invalid range prevents PAGE READ", "[spi_nand][ecc]") {
    MockSpiBus bus;
    SpiNand nand(bus);
    configure(bus, nand, smallPart(NandEccProfile::W25n01kv));
    std::array<uint8_t, 4> buffer{};
    Result<EccReport> result;
    SECTION("ECC disabled") { bus.queueResponse({8}); result = nand.readPage(0, buffer); }
    SECTION("buffer mode disabled") { bus.queueResponse({0x10}); result = nand.readPage(0, buffer); }
    SECTION("OTP activated") { bus.queueResponse({0x58}); result = nand.readPage(0, buffer); }
    SECTION("invalid page") { result = nand.readPage(16, buffer); }
    SECTION("invalid buffer") { result = nand.readPage(0, std::span(buffer).first(2)); }
    REQUIRE_FALSE(result);
    CHECK(bus.transactions.size() <= 1);
}

TEST_CASE("Read errors at every stage propagate without returning clean ECC", "[spi_nand][ecc]") {
    const auto index = GENERATE(size_t{0}, size_t{1}, size_t{2}, size_t{3}, size_t{4});
    MockSpiBus bus;
    SpiNand nand(bus);
    configure(bus, nand, smallPart());
    bus.queueResponse({0x10}); // config
    bus.queueResponse({0x10}); // status
    bus.queueResponse({0}); // extended ECC
    bus.queueResponse({1, 2, 3, 4});
    bus.failTransactionAt = index;
    std::array<uint8_t, 4> buffer{};
    auto result = nand.readPage(0, buffer);
    REQUIRE_FALSE(result);
    CHECK(result.error().message == "mock SPI transaction failure");
}

TEST_CASE("Reset invalidates prepared page reads", "[spi_nand][ecc]") {
    MockSpiBus bus;
    SpiNand nand(bus);
    configure(bus, nand, smallPart());
    bus.queueResponse({0});
    REQUIRE(nand.reset());
    std::array<uint8_t, 4> buffer{};
    REQUIRE_FALSE(nand.readPage(0, buffer));
}
