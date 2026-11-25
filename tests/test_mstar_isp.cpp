#include <array>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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

TEST_CASE("MstarIsp::transaction ends SPI after a failed read and preserves the error", "[isp]") {
    MockI2cMaster i2c;
    MstarIsp isp(i2c);
    i2c.failNextWriteRead = true;

    std::array<uint8_t, 1> tx{0x9F};
    std::array<uint8_t, 3> rx{};
    auto result = isp.transaction(tx, rx);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::I2cTransfer);
    CHECK(result.error().message == "mock writeRead failure");

    REQUIRE(i2c.transactions.size() == 3);
    CHECK(i2c.transactions[1].kind == TransactionKind::WriteRead);
    CHECK(i2c.transactions[2].kind == TransactionKind::Write);
    CHECK(i2c.transactions[2].tx == std::vector<uint8_t>{0x12});
}

TEST_CASE("MstarIsp::transaction ends SPI after a failed write without reading or retrying", "[isp]") {
    MockI2cMaster i2c;
    MstarIsp isp(i2c, 0x4A);
    i2c.failNextWrite = true;
    std::array<uint8_t, 1> tx{0x9F};
    std::array<uint8_t, 3> rx{};

    auto result = isp.transaction(tx, rx);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::I2cNack);
    CHECK(result.error().message == "mock write NACK");
    REQUIRE(i2c.transactions.size() == 2);
    CHECK(i2c.transactions[0].tx == std::vector<uint8_t>{0x10, 0x9F});
    CHECK(i2c.transactions[1].tx == std::vector<uint8_t>{0x12});
    CHECK(i2c.transactions[1].kind == TransactionKind::Write);
    CHECK(i2c.transactions[0].address == 0x4A);
    CHECK(i2c.transactions[1].address == 0x4A);
}

TEST_CASE("MstarIsp::transaction reports SPI_END failure after a successful transfer", "[isp]") {
    MockI2cMaster i2c;
    MstarIsp isp(i2c);
    i2c.failWriteAt = 2;
    std::array<uint8_t, 1> tx{0x9F};
    std::array<uint8_t, 3> rx{};
    Result<void> result;
    SECTION("write only") { result = isp.transaction(tx, {}); }
    SECTION("write and read") {
        i2c.queueResponse({0xEF, 0xAA, 0x22});
        result = isp.transaction(tx, rx);
        CHECK(rx == std::array<uint8_t, 3>{0xEF, 0xAA, 0x22});
    }
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::I2cNack);
    CHECK(result.error().message == "SPI_END failed: mock write NACK");
    CHECK(i2c.writeCalls == 2);
    CHECK(i2c.transactions.back().tx == std::vector<uint8_t>{0x12});
}

TEST_CASE("MstarIsp::transaction retains the transfer error when SPI_END also fails", "[isp]") {
    MockI2cMaster i2c;
    MstarIsp isp(i2c);
    i2c.failWriteAt = 2;
    ErrorCode expected = ErrorCode::I2cTransfer;
    std::string message;
    SECTION("read failure") {
        i2c.failNextWriteRead = true;
        message = "mock writeRead failure";
    }
    SECTION("write failure") {
        i2c.failNextWrite = true;
        expected = ErrorCode::I2cNack;
        message = "mock write NACK";
    }
    std::array<uint8_t, 1> tx{0x9F};
    std::array<uint8_t, 3> rx{};
    auto result = isp.transaction(tx, rx);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == expected);
    CHECK(result.error().message == message + "; SPI_END also failed: mock write NACK");
    CHECK(i2c.writeCalls == 2);
    CHECK(i2c.transactions.back().tx == std::vector<uint8_t>{0x12});
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

TEST_CASE("MstarIsp accepts an activation NACK only after confirming idle ISP status", "[isp]") {
    const auto status = GENERATE(uint8_t{0xC0}, uint8_t{0x80}, uint8_t{0}, uint8_t{0xFF});
    MockI2cMaster i2c;
    MstarIsp isp(i2c, 0x4A);
    i2c.failNextWrite = true;
    i2c.queueResponse({status});
    auto result = isp.enter();
    CHECK(result.has_value() == (status == 0xC0));
    if (!result) CHECK(result.error().code == ErrorCode::I2cNack);
    REQUIRE(i2c.transactions.size() == 2);
    CHECK(i2c.transactions[1].kind == TransactionKind::WriteRead);
    CHECK(i2c.transactions[1].tx == std::vector<uint8_t>{0x20});
    CHECK(i2c.transactions[1].address == 0x4A);
    CHECK(i2c.writeCalls == 1); // no activation retry
}

TEST_CASE("MstarIsp preserves activation and status-transfer failures", "[isp]") {
    MockI2cMaster i2c;
    MstarIsp isp(i2c);
    i2c.failNextWrite = true;
    i2c.failNextWriteRead = true;
    auto result = isp.enter();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::I2cNack);
    CHECK(result.error().message == "mock write NACK; ISP status check failed: mock writeRead failure");
    CHECK(i2c.transactions.size() == 2);
}
