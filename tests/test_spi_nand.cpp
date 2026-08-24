#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("SpiNand::readPage issues PAGE READ then READ FROM CACHE and returns page data", "[spi_nand]") {
    MockSpiBus bus;
    SpiNand nand(bus);
    bus.queueResponse({0x00}); // waitReady after PAGE READ
    bus.queueResponse({0xDE, 0xAD, 0xBE, 0xEF}); // cache contents

    NandGeometry geometry{.pageSize = 4, .oobSize = 0, .pagesPerBlock = 1, .blocksPerLun = 1, .luns = 1};
    std::array<uint8_t, 4> buffer{};
    REQUIRE(nand.readPage(5, geometry, buffer).has_value());
    CHECK(buffer == std::array<uint8_t, 4>{0xDE, 0xAD, 0xBE, 0xEF});

    REQUIRE(bus.transactions.size() == 3);
    CHECK(bus.transactions[0].tx == std::vector<uint8_t>{0x13, 0x00, 0x00, 0x05});
    CHECK(bus.transactions[1].tx == std::vector<uint8_t>{0x0F, 0xC0});
    CHECK(bus.transactions[2].tx == std::vector<uint8_t>{0x03, 0x00, 0x00, 0x00});
}

TEST_CASE("SpiNand::readPage fails with EccUncorrectable when the status register reports an uncorrectable error", "[spi_nand]") {
    MockSpiBus bus;
    SpiNand nand(bus);
    bus.queueResponse({0x20}); // ECC status bits (ECCS1:ECCS0) = 10 -> uncorrectable

    NandGeometry geometry{.pageSize = 4, .oobSize = 0, .pagesPerBlock = 1, .blocksPerLun = 1, .luns = 1};
    std::array<uint8_t, 4> buffer{};
    auto result = nand.readPage(0, geometry, buffer);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::EccUncorrectable);
}

TEST_CASE("SpiNand::readPage succeeds when the status register reports ECC corrected at max strength", "[spi_nand]") {
    MockSpiBus bus;
    SpiNand nand(bus);
    bus.queueResponse({0x30}); // ECC status bits (ECCS1:ECCS0) = 11 -> corrected, still good data
    bus.queueResponse({0xDE, 0xAD, 0xBE, 0xEF});

    NandGeometry geometry{.pageSize = 4, .oobSize = 0, .pagesPerBlock = 1, .blocksPerLun = 1, .luns = 1};
    std::array<uint8_t, 4> buffer{};
    auto result = nand.readPage(0, geometry, buffer);

    REQUIRE(result.has_value());
    CHECK(buffer == std::array<uint8_t, 4>{0xDE, 0xAD, 0xBE, 0xEF});
}

TEST_CASE("SpiNand::readPage rejects a buffer size that does not match geometry.pageSize", "[spi_nand]") {
    MockSpiBus bus;
    SpiNand nand(bus);

    NandGeometry geometry{.pageSize = 4, .oobSize = 0, .pagesPerBlock = 1, .blocksPerLun = 1, .luns = 1};
    std::array<uint8_t, 2> buffer{};
    auto result = nand.readPage(0, geometry, buffer);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::InvalidArgument);
    CHECK(bus.transactions.empty());
}
