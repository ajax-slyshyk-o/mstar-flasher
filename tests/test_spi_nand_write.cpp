#include <array>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "mstar/flash/spi_nand.hpp"
#include "nand_model.hpp"

using namespace mstar;
using namespace mstar::test;

namespace {
void small(NandModel& bus) {
    bus.part.geometry.pageSize = 16;
    bus.part.geometry.oobSize = 8;
    bus.part.geometry.pagesPerBlock = 4;
    bus.part.geometry.blocksPerLun = 4;
}
unsigned count(const NandModel& bus, uint8_t opcode) {
    return static_cast<unsigned>(std::count_if(bus.commands.begin(), bus.commands.end(),
        [=](const auto& c) { return c[0] == opcode; }));
}
}

TEST_CASE("All profiles erase, program in order and verify real modeled data", "[nand_write]") {
    const auto profile = GENERATE(NandEccProfile::Gd5f2gm7, NandEccProfile::Gd5f1gm9,
        NandEccProfile::W25n01gv, NandEccProfile::W25n01kv, NandEccProfile::W25n02kv);
    NandModel bus;
    bus.select(profile);
    small(bus);
    const auto originalConfig = bus.config;
    const auto originalProtection = bus.protection;
    SpiNand nand(bus);
    REQUIRE(nand.configureRead(bus.part));
    REQUIRE(nand.configureWrite());
    REQUIRE(nand.eraseBlock(1));
    std::array<uint8_t, 16> data{};
    for (uint32_t page = 4; page < 8; ++page) {
        data.fill(static_cast<uint8_t>(page));
        REQUIRE(nand.programPage(page, data));
        REQUIRE(nand.verifyPage(page, data));
        CHECK_FALSE(bus.wel);
    }
    CHECK(bus.erases == 1);
    CHECK(bus.programs == 4);
    CHECK_FALSE(nand.programPage(8, data));
    REQUIRE(nand.restoreReadConfiguration());
    CHECK(bus.config == originalConfig);
    CHECK(bus.protection == originalProtection);
    CHECK(bus.contents(0)[0] == 0); // neighboring block unchanged
    CHECK(bus.contents(8)[0] == 136);
}

TEST_CASE("Marked blocks on either marker page cannot be erased", "[nand_write]") {
    const auto page = GENERATE(0u, 1u);
    const auto marker = GENERATE(uint8_t{0}, uint8_t{0xFE});
    NandModel bus;
    small(bus);
    bus.mark(2, page);
    bus.storage.at(2 * bus.part.geometry.pagesPerBlock + page)[bus.part.geometry.pageSize] = marker;
    SpiNand nand(bus);
    REQUIRE(nand.configureRead(bus.part));
    REQUIRE(nand.configureWrite());
    auto marked = nand.isBadBlock(2);
    REQUIRE(marked);
    CHECK(*marked);
    auto result = nand.eraseBlock(2);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::BadBlock);
    CHECK(count(bus, 0xD8) == 0);
    CHECK(count(bus, 0x06) == 0);
    CHECK(bus.config & 0x10);
    REQUIRE(nand.restoreReadConfiguration());
}

TEST_CASE("W25N01GV second spare marker byte is checked", "[nand_write]") {
    NandModel bus;
    bus.select(NandEccProfile::W25n01gv);
    small(bus);
    bus.mark(1, 0, 1);
    SpiNand nand(bus);
    REQUIRE(nand.configureRead(bus.part));
    auto result = nand.isBadBlock(1);
    REQUIRE(result);
    CHECK(*result);
}

TEST_CASE("Raw backups include spare bytes and restore ECC after transport failure", "[nand_write]") {
    NandModel bus;
    small(bus);
    bus.mark(0);
    SpiNand nand(bus);
    REQUIRE(nand.configureRead(bus.part));
    std::array<uint8_t, 24> raw{};
    SECTION("complete backup") {
        REQUIRE(nand.readRawPage(0, raw));
        CHECK(raw[1] == 1);
        CHECK(raw[16] == 0);
        CHECK(raw[17] == 0xFF);
    }
    SECTION("page read fails") { bus.failOpcode = 0x13; CHECK_FALSE(nand.readRawPage(0, raw)); }
    SECTION("cache read fails") { bus.failOpcode = 0x03; CHECK_FALSE(nand.readRawPage(0, raw)); }
    CHECK(bus.config == 0x10);
    REQUIRE(nand.restoreReadConfiguration());
}

TEST_CASE("Failed raw ECC restoration blocks further reads and writes", "[nand_write]") {
    NandModel bus;
    small(bus);
    SpiNand nand(bus);
    REQUIRE(nand.configureRead(bus.part));
    bus.failRawRestore = true;
    auto marker = nand.isBadBlock(0);
    REQUIRE_FALSE(marker);
    CHECK_FALSE(nand.configureWrite());
    CHECK_FALSE(nand.restoreReadConfiguration());
    bus.failRawRestore = false;
    REQUIRE(nand.restoreReadConfiguration());
    CHECK(bus.config == 0x10);
}

TEST_CASE("Write protection and WEL failures prevent erase", "[nand_write]") {
    NandModel bus;
    small(bus);
    SpiNand nand(bus);
    REQUIRE(nand.configureRead(bus.part));
    SECTION("unlock ignored") {
        bus.ignoreUnlock = true;
        CHECK_FALSE(nand.configureWrite());
        CHECK_FALSE(nand.eraseBlock(0));
    }
    SECTION("WREN ignored") {
        REQUIRE(nand.configureWrite());
        bus.ignoreWren = true;
        auto result = nand.eraseBlock(0);
        REQUIRE_FALSE(result);
        CHECK(result.error().message.find("WEL") != std::string::npos);
    }
    CHECK(bus.erases == 0);
    CHECK_FALSE(bus.wel);
    REQUIRE(nand.restoreReadConfiguration());
    CHECK(bus.protection == 0x3E);
}

TEST_CASE("E-FAIL, P-FAIL, timeout and mismatch are failures", "[nand_write]") {
    NandModel bus;
    small(bus);
    SpiNand nand(bus);
    REQUIRE(nand.configureRead(bus.part));
    REQUIRE(nand.configureWrite());
    SECTION("erase failure") {
        bus.failErase = true;
        auto r = nand.eraseBlock(0);
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find("E-FAIL") != std::string::npos);
    }
    SECTION("busy timeout") {
        bus.stuckBusy = true;
        auto r = nand.eraseBlock(0);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::Timeout);
        CHECK(bus.erases == 0);
        bus.stuckBusy = false;
    }
    SECTION("program failure") {
        REQUIRE(nand.eraseBlock(0));
        bus.failProgram = true;
        std::array<uint8_t, 16> data{};
        auto r = nand.programPage(0, data);
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find("P-FAIL") != std::string::npos);
    }
    SECTION("readback mismatch") {
        REQUIRE(nand.eraseBlock(0));
        std::array<uint8_t, 16> data{};
        REQUIRE(nand.programPage(0, data));
        bus.corruptVerify = true;
        auto r = nand.verifyPage(0, data);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::VerifyMismatch);
    }
    CHECK_FALSE(bus.wel);
    REQUIRE(nand.restoreReadConfiguration());
    CHECK(bus.protection == 0x3E);
}

TEST_CASE("No execute follows a failed load and ambiguous mutations are not retried", "[nand_write]") {
    const auto opcode = GENERATE(uint8_t{0x06}, uint8_t{0x02}, uint8_t{0x10});
    NandModel bus;
    small(bus);
    SpiNand nand(bus);
    REQUIRE(nand.configureRead(bus.part));
    REQUIRE(nand.configureWrite());
    REQUIRE(nand.eraseBlock(0));
    bus.commands.clear();
    bus.failOpcode = opcode;
    bus.failureAfterEffect = true;
    std::array<uint8_t, 16> data{};
    auto result = nand.programPage(0, data);
    REQUIRE_FALSE(result);
    CHECK(count(bus, opcode) == 1);
    CHECK(count(bus, 0x10) == (opcode == 0x10 ? 1 : 0));
    CHECK_FALSE(nand.programPage(0, data));
    CHECK_FALSE(bus.wel);
    REQUIRE(nand.restoreReadConfiguration());
}

TEST_CASE("Out-of-order, partial and unprepared programming is rejected", "[nand_write]") {
    NandModel bus;
    small(bus);
    SpiNand nand(bus);
    std::array<uint8_t, 16> data{};
    CHECK_FALSE(nand.programPage(0, data));
    REQUIRE(nand.configureRead(bus.part));
    REQUIRE(nand.configureWrite());
    CHECK_FALSE(nand.programPage(0, data));
    REQUIRE(nand.eraseBlock(0));
    CHECK_FALSE(nand.programPage(1, data));
    CHECK_FALSE(nand.programPage(0, std::span(data).first(8)));
    REQUIRE(nand.programPage(0, data));
    CHECK_FALSE(nand.programPage(0, data));
    CHECK(bus.programs == 1);
    REQUIRE(nand.restoreReadConfiguration());
}

TEST_CASE("Protection restoration errors are visible", "[nand_write]") {
    NandModel bus;
    small(bus);
    SpiNand nand(bus);
    REQUIRE(nand.configureRead(bus.part));
    REQUIRE(nand.configureWrite());
    bus.ignoreProtectionRestore = true;
    auto result = nand.restoreReadConfiguration();
    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("protection") != std::string::npos);
    bus.ignoreProtectionRestore = false;
    REQUIRE(nand.restoreReadConfiguration());
    CHECK(bus.protection == 0x3E);
}

TEST_CASE("Busy timeout after erase or program does not replay the operation", "[nand_write]") {
    NandModel bus;
    small(bus);
    SpiNand nand(bus);
    REQUIRE(nand.configureRead(bus.part));
    REQUIRE(nand.configureWrite());
    Result<void> result;
    SECTION("erase busy") {
        bus.busyAfterErase = true;
        result = nand.eraseBlock(0);
        CHECK(bus.erases == 1);
        CHECK(bus.programs == 0);
    }
    SECTION("program busy") {
        REQUIRE(nand.eraseBlock(0));
        bus.busyAfterProgram = true;
        std::array<uint8_t, 16> data{};
        result = nand.programPage(0, data);
        CHECK(bus.programs == 1);
    }
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::Timeout);
    CHECK(result.error().message.find("WRDI") != std::string::npos);
    bus.stuckBusy = false;
    REQUIRE(nand.restoreReadConfiguration());
    CHECK(bus.protection == 0x3E);
    CHECK_FALSE(bus.wel);
}

TEST_CASE("A failed erase acknowledgement is never retried", "[nand_write]") {
    NandModel bus;
    small(bus);
    SpiNand nand(bus);
    REQUIRE(nand.configureRead(bus.part));
    REQUIRE(nand.configureWrite());
    bus.failOpcode = 0xD8;
    bus.failureAfterEffect = true;
    auto result = nand.eraseBlock(0);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::I2cTransfer);
    CHECK(bus.erases == 1);
    CHECK(count(bus, 0xD8) == 1);
    std::array<uint8_t, 16> data{};
    CHECK_FALSE(nand.programPage(0, data));
    REQUIRE(nand.restoreReadConfiguration());
}

TEST_CASE("Raw-read recovery retains the original disabled ECC setting", "[nand_write]") {
    NandModel bus;
    small(bus);
    bus.config = 0;
    SpiNand nand(bus);
    REQUIRE(nand.configureRead(bus.part));
    bus.failRawRestore = true;
    CHECK_FALSE(nand.isBadBlock(0));
    CHECK_FALSE(nand.restoreReadConfiguration());
    bus.failRawRestore = false;
    REQUIRE(nand.restoreReadConfiguration());
    CHECK(bus.config == 0);
}
