#include <array>

#include <catch2/catch_test_macros.hpp>

#include "mstar/isp/mstar_isp.hpp"
#include "mock_i2c.hpp"

using namespace mstar;
using namespace mstar::test;

TEST_CASE("MstarIsp enter sends the MSTAR activation string in one transaction", "[isp]") {
    MockI2cMaster i2c;
    MstarIsp isp(i2c);

    REQUIRE(isp.enter().has_value());

    REQUIRE(i2c.transactions.size() == 1);
    const auto& t = i2c.transactions[0];
    CHECK(t.kind == TransactionKind::Write);
    CHECK(t.address == MstarIsp::kDefaultIspAddress);
    CHECK(t.tx == std::vector<uint8_t>{'M', 'S', 'T', 'A', 'R'});
}

TEST_CASE("MstarIsp::transaction reads the JEDEC ID with the exact expected sequence", "[isp]") {
    MockI2cMaster i2c;
    MstarIsp isp(i2c);

    i2c.queueResponse({0xEF, 0xAA, 0x22});

    std::array<uint8_t, 1> tx{0x9F};
    std::array<uint8_t, 3> rx{};
    REQUIRE(isp.transaction(tx, rx).has_value());

    REQUIRE(i2c.transactions.size() == 3);

    const auto& write1 = i2c.transactions[0];
    CHECK(write1.kind == TransactionKind::Write);
    CHECK(write1.address == MstarIsp::kDefaultIspAddress);
    CHECK(write1.tx == std::vector<uint8_t>{0x10, 0x9F});

    const auto& writeRead = i2c.transactions[1];
    CHECK(writeRead.kind == TransactionKind::WriteRead);
    CHECK(writeRead.address == MstarIsp::kDefaultIspAddress);
    CHECK(writeRead.tx == std::vector<uint8_t>{0x11});
    CHECK(writeRead.rx == std::vector<uint8_t>{0xEF, 0xAA, 0x22});

    const auto& write2 = i2c.transactions[2];
    CHECK(write2.kind == TransactionKind::Write);
    CHECK(write2.tx == std::vector<uint8_t>{0x12});

    CHECK(rx == std::array<uint8_t, 3>{0xEF, 0xAA, 0x22});
}

TEST_CASE("MstarIsp::enter surfaces a NACK during MSTAR activation", "[isp]") {
    MockI2cMaster i2c;
    MstarIsp isp(i2c);
    i2c.failNextWrite = true;

    auto result = isp.enter();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::I2cNack);
}

TEST_CASE("MstarIsp::transaction propagates a failed SPI read", "[isp]") {
    MockI2cMaster i2c;
    MstarIsp isp(i2c);
    i2c.failNextWriteRead = true;

    std::array<uint8_t, 1> tx{0x9F};
    std::array<uint8_t, 3> rx{};
    auto result = isp.transaction(tx, rx);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::I2cTransfer);

    // The 0x10 write happened, but 0x12 must not be sent once the read failed
    // mid-transaction with no explicit recovery path.
    REQUIRE(i2c.transactions.size() == 2);
    CHECK(i2c.transactions[1].kind == TransactionKind::WriteRead);
}

TEST_CASE("MstarIsp::transaction with empty rx skips the SPI read step", "[isp]") {
    MockI2cMaster i2c;
    MstarIsp isp(i2c);

    std::array<uint8_t, 1> tx{0x06}; // WRITE ENABLE, no response expected
    auto result = isp.transaction(tx, {});
    REQUIRE(result.has_value());

    REQUIRE(i2c.transactions.size() == 2);
    CHECK(i2c.transactions[0].kind == TransactionKind::Write);
    CHECK(i2c.transactions[0].tx == std::vector<uint8_t>{0x10, 0x06});
    CHECK(i2c.transactions[1].kind == TransactionKind::Write);
    CHECK(i2c.transactions[1].tx == std::vector<uint8_t>{0x12});
}
