#include <algorithm>
#include <array>
#include <cstdio>
#include <set>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <libmpsse_i2c.h>

#include "mstar/transport/ftdi/ftdi_i2c.hpp"
#include "ftdi_device.hpp"

// Compile the real D2XX/LibMPSSE I2C implementation, but never link vendor DLLs
// or USB libraries. The fake preserves closed handles so duplicate closes can
// be detected deterministically rather than crashing the test process.
using namespace mstar;
using mstar::ftdi::FtdiI2c;
namespace {
struct Fake {
    DWORD channels = 1;
    DWORD openedChannel = 99;
    FT_STATUS openStatus = FT_OK;
    FT_STATUS initStatus = FT_OK;
    FT_STATUS closeStatus = FT_OK;
    int opens = 0;
    int inits = 0;
    int duplicateCloses = 0;
    std::array<int, 8> handles{};
    std::set<FT_HANDLE> live;
    std::vector<FT_HANDLE> closed;
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
// Device discovery is outside these ownership tests; supply only the helpers
// needed to link ftdi_i2c.cpp independently of the OS-specific enumerator.
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
FT_STATUS I2C_InitChannel(FT_HANDLE handle, ChannelConfig*) {
    ++fake.inits;
    REQUIRE(fake.live.contains(handle));
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
FT_STATUS I2C_DeviceWrite(FT_HANDLE, UCHAR, DWORD, UCHAR*, LPDWORD transferred, DWORD) {
    *transferred = 0;
    return FT_IO_ERROR;
}
FT_STATUS I2C_DeviceRead(FT_HANDLE, UCHAR, DWORD, UCHAR*, LPDWORD transferred, DWORD) {
    *transferred = 0;
    return FT_IO_ERROR;
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
