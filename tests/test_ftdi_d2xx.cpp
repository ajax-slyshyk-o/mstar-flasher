#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <set>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <libmpsse_i2c.h>

#include "mstar/transport/ftdi/ftdi_i2c.hpp"
#include "mstar/isp/mstar_isp.hpp"
#include "ftdi_device.hpp"

// Exercise the production transport with an MPSSE interpreter and injected
// driver failures, without vendor DLLs or hardware.
using namespace mstar;
using mstar::ftdi::FtdiI2c;
namespace {
struct Fake {
    DWORD channels = 1;
    DWORD openedChannel = 99;
    FT_STATUS openStatus = FT_OK;
    FT_STATUS initStatus = FT_OK;
    FT_STATUS closeStatus = FT_OK;
    FT_STATUS timeoutStatus = FT_OK;
    FT_STATUS resetStatus = FT_OK;
    int opens = 0;
    int inits = 0;
    int duplicateCloses = 0;
    std::array<int, 8> handles{};
    std::set<FT_HANDLE> live;
    std::vector<FT_HANDLE> closed;
    ChannelConfig config{};
    DWORD readTimeout = 0, writeTimeout = 0;
    int resets = 0;
    std::vector<std::vector<uint8_t>> writes;
    std::vector<DWORD> reads;
    std::deque<uint8_t> replies;
    std::deque<uint8_t> ackScript; // default ACK; bit 0 is the sampled SDA
    std::deque<uint8_t> readData;
    std::vector<uint8_t> transmitted;
    std::vector<bool> masterNacks;
    std::vector<std::vector<uint8_t>> transactions;
    std::vector<char> events;
    unsigned starts = 0, stops = 0;
    unsigned releasedGpioWrites = 0;
    uint8_t pins = 3, directions = 0x10;
    int failWriteAt = -1, shortWriteAt = -1, oversizedWriteAt = -1;
    int failReadAt = -1, shortReadAt = -1, oversizedReadAt = -1;

    void gpio(uint8_t value, uint8_t direction) {
        // Pin states while SDA is released reflect the external pull-up.
        const auto next = static_cast<uint8_t>((value & direction) | (~direction & 3));
        if ((pins & 1) && (next & 1)) {
            if ((pins & 2) && !(next & 2)) {
                ++starts;
                events.push_back('S');
                transactions.emplace_back();
            }
            if (!(pins & 2) && (next & 2)) {
                ++stops;
                events.push_back('P');
            }
        }
        pins = next;
        directions = direction;
        if (!(next & 1) && !(direction & 2)) ++releasedGpioWrites;
        else releasedGpioWrites = 0;
        CHECK((direction & 4) == 0); // AD2 must remain input
        CHECK((direction & 0x10) != 0);
        CHECK((value & 0x10) == 0);
    }

    void interpret(const std::vector<uint8_t>& command) {
        for (std::size_t i = 0; i < command.size();) {
            const auto opcode = command[i++];
            switch (opcode) {
            case 0x80: {
                REQUIRE(i + 2 <= command.size());
                const auto value = command[i++], direction = command[i++];
                gpio(value, direction);
                break;
            }
            case 0x13: {
                REQUIRE(i + 2 <= command.size());
                const auto bits = command[i++] + 1;
                const auto value = command[i++];
                releasedGpioWrites = 0;
                REQUIRE((pins & 1) == 0); // all clock commands start/end low
                if (bits == 8) {
                    REQUIRE((directions & 2) != 0);
                    transmitted.push_back(value);
                    REQUIRE_FALSE(transactions.empty());
                    transactions.back().push_back(value);
                    events.push_back('W');
                    pins = static_cast<uint8_t>((value & 1) ? 2 : 0);
                } else {
                    REQUIRE(bits == 1);
                    const bool nack = (value & 0x80) != 0;
                    masterNacks.push_back(nack);
                    if (nack) CHECK((directions & 2) == 0); // release, never drive NACK high
                    else CHECK((directions & 2) != 0);
                    pins = nack ? 2 : 0;
                }
                break;
            }
            case 0x22: {
                REQUIRE(i < command.size());
                const auto bits = command[i++] + 1;
                REQUIRE((pins & 1) == 0);
                REQUIRE((directions & 2) == 0); // release SDA for ACK and data
                if (bits == 1) {
                    const auto ack = ackScript.empty() ? uint8_t{0} : ackScript.front();
                    if (!ackScript.empty()) ackScript.pop_front();
                    // Model the observed slow SDA release: sampling too soon
                    // masks NACKs as ACKs. This regresses the former bytewise
                    // implementation's false ACK at the unused address 0x7f.
                    replies.push_back(releasedGpioWrites >= 20 ? ack : 0);
                } else {
                    REQUIRE(bits == 8);
                    REQUIRE_FALSE(readData.empty());
                    // Back-to-back bytes need settling after master ACK.
                    replies.push_back(releasedGpioWrites >= 20 ? readData.front() : 0x7F);
                    readData.pop_front();
                    events.push_back('R');
                }
                releasedGpioWrites = 0;
                pins = 2; // SCL low, released SDA
                break;
            }
            case 0x87:
                break;
            default:
                FAIL("Unexpected/unsupported MPSSE opcode " << unsigned(opcode));
            }
        }
    }
} fake;

struct Fixture {
    Fixture() { fake = Fake{}; }
    ~Fixture() {
        CHECK(fake.live.empty());
        CHECK(fake.duplicateCloses == 0);
    }
};
}

namespace mstar::ftdi::detail {
Result<std::vector<ProgrammerInfo>> enumerateDevices() { return std::vector<ProgrammerInfo>{}; }
std::string channelFromSerial(std::string_view serial) {
    return serial.empty() ? "" : std::string(1, serial.back());
}
std::string boundedString(const char* data, std::size_t capacity) {
    return std::string(data, std::find(data, data + capacity, '\0'));
}
}

extern "C" {
FT_STATUS I2C_GetNumChannels(DWORD* count) { *count = fake.channels; return FT_OK; }
FT_STATUS I2C_GetChannelInfo(DWORD index, FT_DEVICE_LIST_INFO_NODE* info) {
    *info = {};
    std::snprintf(info->SerialNumber, sizeof(info->SerialNumber), "%s", index == 0 ? "TESTA" : "SECONDA");
    std::snprintf(info->Description, sizeof(info->Description), "Test programmer A");
    return FT_OK;
}
FT_STATUS I2C_OpenChannel(DWORD index, FT_HANDLE* handle) {
    fake.openedChannel = index;
    ++fake.opens;
    *handle = nullptr;
    if (fake.openStatus != FT_OK) return fake.openStatus;
    REQUIRE(fake.opens <= static_cast<int>(fake.handles.size()));
    *handle = &fake.handles[fake.opens - 1];
    fake.live.insert(*handle);
    return FT_OK;
}
FT_STATUS I2C_InitChannel(FT_HANDLE handle, ChannelConfig* config) {
    ++fake.inits;
    REQUIRE(fake.live.contains(handle));
    fake.config = *config;
    return fake.initStatus;
}
FT_STATUS I2C_CloseChannel(FT_HANDLE handle) {
    fake.closed.push_back(handle);
    if (fake.live.erase(handle) != 1) {
        ++fake.duplicateCloses;
        return FT_INVALID_HANDLE;
    }
    return fake.closeStatus;
}
FT_STATUS FT_SetTimeouts(FT_HANDLE handle, DWORD read, DWORD write) {
    REQUIRE(fake.live.contains(handle));
    fake.readTimeout = read;
    fake.writeTimeout = write;
    return fake.timeoutStatus;
}
FT_STATUS FT_SetBitMode(FT_HANDLE handle, UCHAR mask, UCHAR mode) {
    REQUIRE(fake.live.contains(handle));
    CHECK(mask == 0);
    CHECK(mode == 0);
    ++fake.resets;
    fake.replies.clear();
    return fake.resetStatus;
}
FT_STATUS FT_Write(FT_HANDLE handle, void* buffer, DWORD size, LPDWORD transferred) {
    REQUIRE(fake.live.contains(handle));
    REQUIRE(fake.writeTimeout > 0);
    const auto* bytes = static_cast<uint8_t*>(buffer);
    fake.writes.emplace_back(bytes, bytes + size);
    const auto index = static_cast<int>(fake.writes.size()) - 1;
    *transferred = 0;
    if (index == fake.failWriteAt) return FT_IO_ERROR;
    if (index == fake.shortWriteAt) { *transferred = size - 1; return FT_OK; }
    if (index == fake.oversizedWriteAt) { *transferred = size + 1; return FT_OK; }
    fake.interpret(fake.writes.back());
    *transferred = size;
    return FT_OK;
}
FT_STATUS FT_Read(FT_HANDLE handle, void* buffer, DWORD size, LPDWORD transferred) {
    REQUIRE(fake.live.contains(handle));
    REQUIRE(fake.readTimeout > 0);
    fake.reads.push_back(size);
    const auto index = static_cast<int>(fake.reads.size()) - 1;
    *transferred = 0;
    if (index == fake.failReadAt) return FT_IO_ERROR;
    if (index == fake.oversizedReadAt) { *transferred = size + 1; return FT_OK; }
    const auto count = index == fake.shortReadAt ? size - 1 : size;
    REQUIRE(fake.replies.size() >= count);
    for (DWORD i = 0; i < count; ++i) {
        static_cast<uint8_t*>(buffer)[i] = fake.replies.front();
        fake.replies.pop_front();
    }
    *transferred = count;
    fake.releasedGpioWrites = 20; // a synchronous host round trip also allows settling
    return FT_OK;
}
}

TEST_CASE_METHOD(Fixture, "D2XX selects the first discovered channel when requested", "[d2xx]") {
    fake.channels = 2;
    ProgrammerSelector selector;
    selector.firstMatch = true;
    SECTION("automatic selection") {
        auto result = FtdiI2c::open(selector);
        REQUIRE(result);
        CHECK(fake.openedChannel == 0);
    }
    SECTION("explicit serial still filters") {
        selector.serial = "SECONDA";
        auto result = FtdiI2c::open(selector);
        REQUIRE(result);
        CHECK(fake.openedChannel == 1);
    }
    SECTION("no programmer") {
        fake.channels = 0;
        auto result = FtdiI2c::open(selector);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::DeviceNotFound);
        CHECK(fake.opens == 0);
    }
}

TEST_CASE_METHOD(Fixture, "D2XX initialization failure closes the acquired handle exactly once", "[d2xx]") {
    fake.initStatus = FT_IO_ERROR;
    auto result = FtdiI2c::open({});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::Ftdi);
    CHECK(result.error().message == "I2C_InitChannel failed with status 4");
    CHECK(fake.opens == 1);
    CHECK(fake.inits == 1);
    CHECK(fake.closed.size() == 1);
    CHECK(fake.live.empty());
}

TEST_CASE_METHOD(Fixture, "D2XX open failure neither initializes nor closes a handle", "[d2xx]") {
    fake.openStatus = FT_IO_ERROR;
    auto result = FtdiI2c::open({});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::DeviceBusy);
    CHECK(fake.opens == 1);
    CHECK(fake.inits == 0);
    CHECK(fake.closed.empty());
}

TEST_CASE_METHOD(Fixture, "D2XX handle ownership survives moves without duplicate closes", "[d2xx]") {
    {
        auto opened = FtdiI2c::open({});
        REQUIRE(opened);
        CHECK(fake.closed.empty());
        SECTION("normal destruction") {}
        SECTION("move construction") {
            auto moved = std::move(*opened);
            CHECK(fake.closed.empty());
        }
        SECTION("move assignment releases the previous handle") {
            auto other = FtdiI2c::open({});
            REQUIRE(other);
            CHECK(fake.live.size() == 2);
            *other = std::move(*opened);
            REQUIRE(fake.closed.size() == 1);
            CHECK(fake.closed.front() == &fake.handles[1]);
        }
    }
    CHECK(fake.closed.size() == static_cast<std::size_t>(fake.opens));
}

TEST_CASE_METHOD(Fixture, "D2XX reinitialization failure keeps ownership until destruction", "[d2xx]") {
    {
        auto opened = FtdiI2c::open({});
        REQUIRE(opened);
        fake.initStatus = FT_IO_ERROR;
        auto result = opened->setClock(400000);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::Ftdi);
        CHECK(fake.closed.empty());
        CHECK(fake.live.size() == 1);
    }
    CHECK(fake.closed.size() == 1);
}

TEST_CASE_METHOD(Fixture, "D2XX cleanup failure does not retry close or hide initialization failure", "[d2xx]") {
    fake.initStatus = FT_IO_ERROR;
    fake.closeStatus = FT_INVALID_HANDLE;
    auto result = FtdiI2c::open({});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::Ftdi);
    CHECK(result.error().message == "I2C_InitChannel failed with status 4");
    CHECK(fake.closed.size() == 1);
}

TEST_CASE_METHOD(Fixture, "D2XX initializes finite timeouts and two-phase clocking", "[d2xx]") {
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    CHECK(fake.config.ClockRate == 100000);
    CHECK(fake.config.Options == I2C_DISABLE_3PHASE_CLOCKING);
    CHECK(fake.config.LatencyTimer == 16);
    CHECK(fake.readTimeout == 1000);
    CHECK(fake.writeTimeout == 1000);
    REQUIRE(opened->setClock(10000));
    CHECK(fake.config.ClockRate == 10000);
    CHECK(fake.readTimeout == 1000);
}

TEST_CASE_METHOD(Fixture, "D2XX timeout setup failure closes the handle", "[d2xx]") {
    fake.timeoutStatus = FT_IO_ERROR;
    auto opened = FtdiI2c::open({});
    REQUIRE_FALSE(opened);
    CHECK(opened.error().message.find("FT_SetTimeouts") != std::string::npos);
    CHECK(fake.closed.size() == 1);
}

TEST_CASE_METHOD(Fixture, "D2XX page writes batch payload but sample every ACK", "[d2xx]") {
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    std::vector<uint8_t> payload(2052);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i);
    payload[0] = 0x10; payload[1] = 0x02; payload[2] = payload[3] = 0;
    REQUIRE(opened->write(0x49, payload));
    auto expected = payload;
    expected.insert(expected.begin(), 0x92);
    CHECK(fake.transmitted == expected);
    CHECK(fake.starts == 1);
    CHECK(fake.stops == 1);
    // Address, four checked prefix bytes, two data batches, STOP.
    CHECK(fake.writes.size() == 8);
    CHECK(fake.reads == std::vector<DWORD>{1, 1, 1, 1, 1, 1024, 1024});
    CHECK(fake.replies.empty());
    CHECK(fake.resets == 0);
}

TEST_CASE_METHOD(Fixture, "D2XX reads preserve bytes and ACK across batch boundaries", "[d2xx]") {
    const auto size = GENERATE(1u, 1024u, 1025u, 2176u);
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    std::vector<uint8_t> expected(size), actual(size);
    for (unsigned i = 0; i < size; ++i) expected[i] = static_cast<uint8_t>(i * 37);
    fake.readData.assign(expected.begin(), expected.end());
    REQUIRE(opened->read(0x49, actual));
    CHECK(actual == expected);
    CHECK(fake.transmitted == std::vector<uint8_t>{0x93});
    CHECK(fake.starts == 1);
    CHECK(fake.stops == 1);
    REQUIRE(fake.masterNacks.size() == size);
    CHECK(std::count(fake.masterNacks.begin(), fake.masterNacks.end(), true) == 1);
    CHECK(fake.masterNacks.back());
    CHECK(fake.writes.size() == 2 + (size + 1023) / 1024);
    CHECK(fake.replies.empty());
}

TEST_CASE_METHOD(Fixture, "D2XX combined reads use repeated START without an intervening STOP", "[d2xx]") {
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    fake.readData = {0xA5, 0x5A};
    std::array<uint8_t, 1> tx{0x11};
    std::array<uint8_t, 2> rx{};
    REQUIRE(opened->writeRead(0x49, tx, rx));
    CHECK(rx == std::array<uint8_t, 2>{0xA5, 0x5A});
    CHECK(fake.events == std::vector<char>{'S', 'W', 'W', 'S', 'W', 'R', 'R', 'P'});
    CHECK(fake.transmitted == std::vector<uint8_t>{0x92, 0x11, 0x93});
    CHECK(fake.masterNacks == std::vector<bool>{false, true});
}

TEST_CASE_METHOD(Fixture, "D2XX control and prefix NACKs stop before later bytes", "[d2xx]") {
    const auto size = GENERATE(5u, 2052u);
    const auto nackAt = GENERATE(0u, 1u, 4u);
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    fake.ackScript.assign(nackAt + 1, 0);
    fake.ackScript.back() = 1;
    std::vector<uint8_t> data(size, 0xA5);
    auto result = opened->write(0x49, data);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::I2cNack);
    CHECK(fake.transmitted.size() == nackAt + 1);
    CHECK(fake.stops == 1);
    CHECK(fake.resets == 0);
    CHECK(fake.replies.empty());
    // A normal NACK leaves the transport usable for SPI_END/WRDI cleanup.
    REQUIRE(opened->write(0x49, std::array<uint8_t, 1>{0x12}));
    CHECK(fake.stops == 2);
}

TEST_CASE_METHOD(Fixture, "D2XX payload NACKs are checked at every position and block later batches", "[d2xx]") {
    const auto nackOffset = GENERATE(4u, 515u, 1027u, 1028u, 2051u);
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    fake.ackScript.assign(nackOffset + 2, 0);
    fake.ackScript.back() = 1;
    std::vector<uint8_t> data(3076, 0xA5);
    auto result = opened->write(0x49, data);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::I2cNack);
    CHECK(result.error().message.find("byte " + std::to_string(nackOffset) + ";") != std::string::npos);
    const auto completedBatches = 1 + (nackOffset - 4) / 1024;
    CHECK(fake.transmitted.size() == 5 + completedBatches * 1024);
    CHECK(fake.reads.size() == 5 + completedBatches);
    CHECK(fake.stops == 1);
    CHECK(fake.replies.empty()); // all replies drained, including after the NACK
    REQUIRE(opened->write(0x49, std::array<uint8_t, 1>{0x12}));
}

TEST_CASE_METHOD(Fixture, "D2XX failed combined phases never start reading data", "[d2xx]") {
    const auto nackAt = GENERATE(0u, 1u, 2u); // write address, command, read address
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    fake.ackScript.assign(nackAt + 1, 0);
    fake.ackScript.back() = 1;
    std::array<uint8_t, 4> rx{};
    auto result = opened->writeRead(0x49, std::array<uint8_t, 1>{0x11}, rx);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::I2cNack);
    CHECK(fake.transmitted.size() == nackAt + 1);
    CHECK(fake.masterNacks.empty());
    CHECK(fake.stops == 1);
}

TEST_CASE_METHOD(Fixture, "D2XX USB write faults invalidate the stream without retrying or appending STOP", "[d2xx]") {
    const auto call = GENERATE(0, 3, 5, 7); // address, header, bulk payload, STOP
    const auto kind = GENERATE(0, 1, 2); // failure, short, impossible count
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    if (kind == 0) fake.failWriteAt = call;
    if (kind == 1) fake.shortWriteAt = call;
    if (kind == 2) fake.oversizedWriteAt = call;
    auto result = opened->write(0x49, std::vector<uint8_t>(2052, 0xA5));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::I2cTransfer);
    CHECK(fake.writes.size() == static_cast<std::size_t>(call + 1));
    CHECK(fake.stops == 0);
    CHECK(fake.resets == 1);
    CHECK_FALSE(opened->write(0x49, std::array<uint8_t, 1>{0x12}));
    CHECK_FALSE(opened->setClock(100000));
    std::array<uint8_t, 1> rx{};
    CHECK_FALSE(opened->read(0x49, rx));
    CHECK_FALSE(opened->writeRead(0x49, {}, rx));
    CHECK(fake.writes.size() == static_cast<std::size_t>(call + 1));
    CHECK(fake.inits == 1);
    CHECK(fake.resets == 1);
}

TEST_CASE_METHOD(Fixture, "D2XX failed or incomplete responses cannot become later ACKs", "[d2xx]") {
    const auto reading = GENERATE(false, true);
    const auto stage = GENERATE(0, 1, 2);
    const auto call = reading || stage == 0 ? stage : 4 + stage;
    const auto kind = GENERATE(0, 1, 2);
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    if (kind == 0) fake.failReadAt = call;
    if (kind == 1) fake.shortReadAt = call;
    if (kind == 2) fake.oversizedReadAt = call;
    std::array<uint8_t, 2048> data{};
    fake.readData.assign(data.size(), 0xA5);
    auto result = reading ? opened->read(0x49, data) : opened->write(0x49, data);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == (kind == 1 ? ErrorCode::Timeout : ErrorCode::I2cTransfer));
    CHECK(fake.reads.size() == static_cast<std::size_t>(call + 1));
    CHECK(fake.resets == 1);
    const auto writes = fake.writes.size();
    CHECK_FALSE(opened->write(0x49, std::array<uint8_t, 1>{0x12}));
    CHECK(fake.writes.size() == writes);
    CHECK(fake.replies.empty());
}

TEST_CASE_METHOD(Fixture, "D2XX STOP and reset errors preserve the primary failure", "[d2xx]") {
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    fake.ackScript = {1}; // address NACK
    fake.failWriteAt = 1; // STOP
    fake.resetStatus = FT_IO_ERROR;
    auto result = opened->write(0x49, {});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::I2cNack);
    CHECK(result.error().message.find("address 0x49 was NACKed") != std::string::npos);
    CHECK(result.error().message.find("STOP also failed") != std::string::npos);
    CHECK(result.error().message.find("MPSSE reset also failed") != std::string::npos);
    CHECK(fake.resets == 1);
}

TEST_CASE_METHOD(Fixture, "D2XX small control commands retain immediate NACK handling", "[d2xx]") {
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    std::array<uint8_t, 16> control{};
    fake.ackScript.assign(11, 0);
    fake.ackScript.back() = 1;
    auto result = opened->write(0x49, control);
    REQUIRE_FALSE(result);
    CHECK(fake.transmitted.size() == 11);
    CHECK(std::all_of(fake.reads.begin(), fake.reads.end(), [](auto n) { return n == 1; }));
}

TEST_CASE_METHOD(Fixture, "D2XX slow clocks bound the wire time of each batch", "[d2xx]") {
    const auto hz = GENERATE(92u, 1000u, 10000u);
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    REQUIRE(opened->setClock(hz));
    std::array<uint8_t, 40> data{};
    fake.readData.assign(data.size(), 0xA5);
    REQUIRE(opened->read(0x49, data));
    for (auto n : fake.reads) CHECK(n * 9.0 / hz < 0.25);
    CHECK(fake.stops == 1);
    CHECK(std::count(fake.masterNacks.begin(), fake.masterNacks.end(), true) == 1);
}

TEST_CASE_METHOD(Fixture, "D2XX invalid arguments never touch the bus", "[d2xx]") {
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    std::array<uint8_t, 1> data{};
    CHECK_FALSE(opened->write(0x80, data));
    CHECK_FALSE(opened->read(0x80, data));
    CHECK_FALSE(opened->writeRead(0x80, data, data));
    CHECK_FALSE(opened->read(0x49, {}));
    CHECK_FALSE(opened->writeRead(0x49, data, {}));
    CHECK_FALSE(opened->setClock(0));
    CHECK_FALSE(opened->setClock(6000001));
    CHECK(fake.writes.empty());
    CHECK(fake.inits == 1);
    // Empty writes remain address-only probes.
    REQUIRE(opened->write(0x49, {}));
    CHECK(fake.transmitted == std::vector<uint8_t>{0x92});
    CHECK(fake.stops == 1);
}

TEST_CASE_METHOD(Fixture, "MStar ISP retains failed-load errors and sends SPI_END once after a batched NACK", "[d2xx][isp]") {
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    MstarIsp isp(*opened, 0x49);
    std::vector<uint8_t> load(2051, 0xA5);
    load[0] = 0x02; load[1] = load[2] = 0;
    fake.ackScript.assign(11, 0);
    fake.ackScript.back() = 1;
    auto result = isp.transaction(load, {});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::I2cNack);
    REQUIRE(fake.transactions.size() == 2);
    CHECK(fake.transactions[0].size() == 1029); // address + header + first batch
    CHECK(fake.transactions[1] == std::vector<uint8_t>{0x92, 0x12});
    CHECK(fake.stops == 2);
    CHECK(fake.resets == 0);
}

TEST_CASE_METHOD(Fixture, "D2XX checks only the current ACK bit, not stale upper shift-register bits", "[d2xx]") {
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    fake.ackScript.assign(2053, 0xFE); // bit 0 ACK, upper bits are previous samples
    REQUIRE(opened->write(0x49, std::vector<uint8_t>(2052, 0xA5)));
    CHECK(fake.replies.empty());
}

TEST_CASE_METHOD(Fixture, "D2XX ISP reconnection requires an acknowledged idle status after activation NACK", "[d2xx][isp]") {
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    MstarIsp isp(*opened, 0x49);
    fake.ackScript = {0, 1}; // already active: address ACK, MSTAR data NACK
    fake.readData = {0xC0};
    REQUIRE(isp.enter());
    CHECK(fake.transactions == std::vector<std::vector<uint8_t>>{
        {0x92, 'M'}, {0x92, 0x20}, {0x93}});
    CHECK(fake.stops == 2);
    CHECK(fake.resets == 0);
}

TEST_CASE_METHOD(Fixture, "D2XX ISP activation never masks a USB failure with a status probe", "[d2xx][isp]") {
    auto opened = FtdiI2c::open({});
    REQUIRE(opened);
    MstarIsp isp(*opened, 0x49);
    fake.shortWriteAt = 1;
    auto result = isp.enter();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::I2cTransfer);
    CHECK(fake.writes.size() == 2);
    CHECK(fake.resets == 1);
}
