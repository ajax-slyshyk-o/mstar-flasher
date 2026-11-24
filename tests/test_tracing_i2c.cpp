#include <array>
#include <sstream>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "mstar/transport/tracing_i2c.hpp"
#include "mock_i2c.hpp"

using namespace mstar;
using namespace mstar::test;

TEST_CASE("I2C tracing preserves transactions and displays returned bytes", "[trace]") {
    MockI2cMaster inner;
    std::ostringstream output;
    TracingI2cMaster bus(inner, &output);
    REQUIRE(bus.setClock(100000));
    std::array<uint8_t, 5> activation{'M', 'S', 'T', 'A', 'R'};
    REQUIRE(bus.write(0x49, activation));
    inner.queueResponse({0xC8, 0x92});
    std::array<uint8_t, 1> command{0x11};
    std::array<uint8_t, 2> reply{};
    REQUIRE(bus.writeRead(0x49, command, reply));
    CHECK(reply == std::array<uint8_t, 2>{0xC8, 0x92});
    CHECK(inner.lastClockHz == 100000);
    REQUIRE(inner.transactions.size() == 2);
    CHECK(inner.transactions[0].tx == std::vector<uint8_t>{'M', 'S', 'T', 'A', 'R'});
    CHECK(inner.transactions[1].kind == TransactionKind::WriteRead);
    CHECK(output.str() ==
        "I2C CLOCK 100000 Hz -> OK\n"
        "I2C W 0x49: 4D 53 54 41 52 -> OK\n"
        "I2C WR 0x49: 11 -> C8 92\n");
}

TEST_CASE("I2C tracing reports failures without printing stale input bytes", "[trace]") {
    MockI2cMaster inner;
    std::ostringstream output;
    TracingI2cMaster bus(inner, &output);
    std::array<uint8_t, 1> command{0x11};
    std::array<uint8_t, 2> reply{0xDE, 0xAD};
    Result<void> result;
    SECTION("read") { inner.failNextRead = true; result = bus.read(0x49, reply); }
    SECTION("writeRead") { inner.failNextWriteRead = true; result = bus.writeRead(0x49, command, reply); }
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::I2cTransfer);
    CHECK(output.str().find("ERROR: " + result.error().message) != std::string::npos);
    CHECK(output.str().find("DE AD") == std::string::npos);
}

TEST_CASE("I2C tracing summarizes large buffers and preserves NACKs", "[trace]") {
    MockI2cMaster inner;
    std::ostringstream output;
    TracingI2cMaster bus(inner, &output);
    std::vector<uint8_t> data(2048, 0xA5);
    REQUIRE(bus.write(0x49, data));
    inner.queueResponse(data);
    REQUIRE(bus.read(0x49, data));
    inner.failNextWrite = true;
    auto result = bus.write(0x49, {});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::I2cNack);
    CHECK(output.str() ==
        "I2C W 0x49: 2048 bytes -> OK\n"
        "I2C R 0x49 (2048 bytes): 2048 bytes\n"
        "I2C W 0x49: 0 bytes -> ERROR: mock write NACK\n");
}

TEST_CASE("I2C tracing can be disabled without changing transfers", "[trace]") {
    MockI2cMaster inner;
    TracingI2cMaster bus(inner);
    inner.queueResponse({0x42});
    std::array<uint8_t, 1> reply{};
    REQUIRE(bus.read(0x49, reply));
    CHECK(reply[0] == 0x42);
    REQUIRE(inner.transactions.size() == 1);
    CHECK(inner.transactions[0].kind == TransactionKind::Read);
}
