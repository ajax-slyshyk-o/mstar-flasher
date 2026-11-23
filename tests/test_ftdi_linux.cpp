#include <algorithm>
#include <array>
#include <deque>
#include <string>
#include <vector>
#include <ftdi.h>
#include <libusb.h>
#undef DEPRECATED // libftdi 1.5 and Catch2 both define this macro.
#include <catch2/catch_test_macros.hpp>
#include "mstar/transport/ftdi/ftdi_i2c.hpp"

// Fake C APIs only: this executable does not link libusb or libftdi.
struct libusb_context {};
struct libusb_device {
    uint16_t product = 0x6010;
    uint16_t version = 0x0500;
    std::string serial = "MSTAR";
    bool accessible = true;
};
struct libusb_device_handle { libusb_device* device; };

using namespace mstar;
using mstar::ftdi::FtdiI2c;
namespace {
struct Fake {
    libusb_context usb;
    std::vector<libusb_device> devices{libusb_device{}};
    std::vector<std::vector<uint8_t>> writes;
    std::deque<uint8_t> replies{0xFA, 0xAA, 0xFA, 0xAB};
    std::deque<int> reads;
    std::string failure;
    int openStatus = 0;
    int contexts = 0, handles = 0, lists = 0, closes = 0, opens = 0;
    int shortWriteAt = -1;
    ftdi_interface interface = INTERFACE_ANY;
    libusb_device* openedDevice = nullptr;
} fake;
struct Fixture {
    Fixture() { fake = Fake{}; }
    ~Fixture() {
        CHECK(fake.contexts == 0);
        CHECK(fake.handles == 0);
        CHECK(fake.lists == 0);
    }
};
std::vector<uint8_t> start() {
    std::vector<uint8_t> bytes{0x80, 2, 0x13};
    for (uint8_t value : {uint8_t{3}, uint8_t{1}})
        for (int i = 0; i < (value == 3 ? 10 : 20); ++i) bytes.insert(bytes.end(), {0x80, value, 0x13});
    bytes.insert(bytes.end(), {0x80, 0, 0x13});
    return bytes;
}
std::vector<uint8_t> stop() {
    std::vector<uint8_t> bytes;
    for (uint8_t value : {uint8_t{0}, uint8_t{1}, uint8_t{3}})
        for (int i = 0; i < 10; ++i) bytes.insert(bytes.end(), {0x80, value, 0x13});
    bytes.insert(bytes.end(), {0x80, 3, 0x10});
    return bytes;
}
std::vector<uint8_t> sendByte(uint8_t value) {
    return {0x80, 2, 0x13, 0x13, 7, value, 0x80, 0, 0x11, 0x22, 0, 0x87};
}
}

extern "C" {
ftdi_context* ftdi_new() {
    if (fake.failure == "new") return nullptr;
    ++fake.contexts;
    auto* ctx = new ftdi_context{};
    ctx->usb_ctx = &fake.usb;
    return ctx;
}
void ftdi_free(ftdi_context* ctx) {
    if (!ctx) return;
    CHECK(ctx->usb_dev == nullptr);
    --fake.contexts;
    delete ctx;
}
const char* ftdi_get_error_string(ftdi_context*) { return "injected error"; }
int ftdi_set_interface(ftdi_context*, ftdi_interface interface) {
    fake.interface = interface;
    return fake.failure == "interface" ? -1 : 0;
}
int ftdi_usb_open_dev(ftdi_context* ctx, libusb_device* device) {
    ++fake.opens;
    fake.openedDevice = device;
    CHECK(ctx->module_detach_mode == AUTO_DETACH_REATACH_SIO_MODULE);
    if (fake.openStatus < 0) return fake.openStatus;
    ctx->usb_dev = new libusb_device_handle{device};
    ++fake.handles;
    switch (device->version) {
        case 0x0700: ctx->type = TYPE_2232H; break;
        case 0x0800: ctx->type = TYPE_4232H; break;
        case 0x0900: ctx->type = TYPE_232H; break;
        default: ctx->type = TYPE_2232C; break;
    }
    return 0;
}
int ftdi_usb_close(ftdi_context* ctx) {
    ++fake.closes;
    delete ctx->usb_dev;
    ctx->usb_dev = nullptr;
    --fake.handles;
    return 0;
}
int ftdi_tcioflush(ftdi_context*) { return fake.failure == "flush" ? -1 : 0; }
int ftdi_set_latency_timer(ftdi_context*, unsigned char) { return 0; }
int ftdi_set_bitmode(ftdi_context*, unsigned char, unsigned char mode) {
    return fake.failure == "mode" && mode == BITMODE_MPSSE ? -1 : 0;
}
int ftdi_write_data(ftdi_context*, const unsigned char* bytes, int size) {
    fake.writes.emplace_back(bytes, bytes + size);
    if (fake.shortWriteAt == static_cast<int>(fake.writes.size()) - 1) return size - 1;
    if (fake.failure == "write") return -1;
    return size;
}
int ftdi_read_data(ftdi_context* ctx, unsigned char* bytes, int size) {
    CHECK(size == 1);
    CHECK(ctx->usb_read_timeout > 0);
    CHECK(ctx->usb_read_timeout <= 1000);
    if (!fake.reads.empty()) {
        const int result = fake.reads.front();
        fake.reads.pop_front();
        if (result != 1) return result;
    }
    if (fake.replies.empty()) return LIBUSB_ERROR_TIMEOUT;
    *bytes = fake.replies.front();
    fake.replies.pop_front();
    return 1;
}
ssize_t libusb_get_device_list(libusb_context*, libusb_device*** list) {
    if (fake.failure == "list") return LIBUSB_ERROR_IO;
    *list = new libusb_device*[fake.devices.size() + 1]{};
    for (std::size_t i = 0; i < fake.devices.size(); ++i) (*list)[i] = &fake.devices[i];
    ++fake.lists;
    return static_cast<ssize_t>(fake.devices.size());
}
void libusb_free_device_list(libusb_device** list, int unref) {
    CHECK(unref == 1);
    delete[] list;
    --fake.lists;
}
int libusb_get_device_descriptor(libusb_device* device, libusb_device_descriptor* desc) {
    desc->idVendor = 0x0403;
    desc->idProduct = device->product;
    desc->bcdDevice = device->version;
    desc->iSerialNumber = 1;
    desc->iProduct = 2;
    return 0;
}
int libusb_open(libusb_device* device, libusb_device_handle** handle) {
    if (fake.failure == "descriptor open" || !device->accessible) return LIBUSB_ERROR_ACCESS;
    *handle = new libusb_device_handle{device};
    ++fake.handles;
    return 0;
}
uint8_t libusb_get_bus_number(libusb_device*) { return 1; }
uint8_t libusb_get_device_address(libusb_device* device) {
    return static_cast<uint8_t>(device - fake.devices.data() + 2);
}
void libusb_close(libusb_device_handle* handle) { delete handle; --fake.handles; }
int libusb_get_string_descriptor_ascii(libusb_device_handle* handle, uint8_t index,
                                       unsigned char* data, int length) {
    if (fake.failure == "string") return LIBUSB_ERROR_IO;
    const std::string value = index == 1 ? handle->device->serial : "MStar programmer";
    REQUIRE(static_cast<int>(value.size()) < length);
    std::copy(value.begin(), value.end(), data);
    return static_cast<int>(value.size());
}
const char* libusb_error_name(int) { return "injected USB error"; }
}

TEST_CASE_METHOD(Fixture, "Linux enumeration exposes only MPSSE channels without claiming USB", "[linux]") {
    fake.devices.push_back({0x6001, 0x0600, "UART"});
    auto devices = FtdiI2c::enumerate();
    REQUIRE(devices);
    REQUIRE(devices->size() == 1);
    CHECK(devices->front().serial == "MSTARA");
    CHECK(devices->front().location == "A");
    CHECK(devices->front().kind == "FT2232C/D");
    CHECK_FALSE(devices->front().comPort);
    CHECK(devices->front().usbLocation == "1:2");
    CHECK(devices->front().accessError.empty());
    CHECK(fake.opens == 0);
    CHECK(fake.writes.empty());
}

TEST_CASE_METHOD(Fixture, "Linux lists inaccessible flashers alongside accessible ones", "[linux]") {
    fake.devices[0].accessible = false;
    fake.devices.push_back({0x6014, 0x0900, "SECOND"});
    auto devices = FtdiI2c::enumerate();
    REQUIRE(devices);
    REQUIRE(devices->size() == 2);
    CHECK(devices->at(0).kind == "FT2232C/D");
    CHECK(devices->at(0).location == "A");
    CHECK(devices->at(0).usbLocation == "1:2");
    CHECK(devices->at(0).serial.empty());
    CHECK(devices->at(0).description.empty());
    CHECK(devices->at(0).accessError.find("permissions/udev") != std::string::npos);
    CHECK(devices->at(1).kind == "FT232H");
    CHECK(devices->at(1).serial == "SECOND");
    CHECK(devices->at(1).usbLocation == "1:3");
    CHECK(devices->at(1).accessError.empty());
    CHECK(fake.opens == 0);
    CHECK(fake.writes.empty());
}

TEST_CASE_METHOD(Fixture, "Linux listing handles empty inventories and unavailable strings", "[linux]") {
    SECTION("no devices") {
        fake.devices.clear();
        auto devices = FtdiI2c::enumerate();
        REQUIRE(devices);
        CHECK(devices->empty());
    }
    SECTION("strings unreadable") {
        fake.failure = "string";
        auto devices = FtdiI2c::enumerate();
        REQUIRE(devices);
        REQUIRE(devices->size() == 1);
        CHECK(devices->front().serial.empty());
        CHECK_FALSE(devices->front().accessError.empty());
    }
    SECTION("enumeration failure") {
        fake.failure = "list";
        CHECK_FALSE(FtdiI2c::enumerate());
    }
    CHECK(fake.opens == 0);
    CHECK(fake.writes.empty());
}

TEST_CASE_METHOD(Fixture, "Linux selects the retained device and rejects ambiguity", "[linux]") {
    fake.devices.push_back({0x6010, 0x0500, "SECOND"});
    SECTION("ambiguous") {
        auto result = FtdiI2c::open({});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::InvalidArgument);
        CHECK(fake.opens == 0);
    }
    SECTION("selector") {
        ProgrammerSelector selector;
        selector.serial = "SECONDA";
        selector.location = "A";
        auto result = FtdiI2c::open(selector);
        REQUIRE(result);
        CHECK(fake.openedDevice == &fake.devices[1]);
        CHECK(fake.interface == INTERFACE_A);
    }
    SECTION("no match") {
        ProgrammerSelector selector;
        selector.location = "B"; // FT2232D channel B is UART only.
        auto result = FtdiI2c::open(selector);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::DeviceNotFound);
        CHECK(fake.opens == 0);
    }
}

TEST_CASE_METHOD(Fixture, "Linux FT2232D setup uses no H-series MPSSE commands", "[linux]") {
    auto device = FtdiI2c::open({});
    REQUIRE(device);
    CHECK(fake.writes == std::vector<std::vector<uint8_t>>{
        {0xAA, 0x87}, {0xAB, 0x87}, {0x85, 0x80, 3, 0x10}, {0x86, 59, 0}});
    REQUIRE(device->setClock(400000));
    CHECK(fake.writes.back() == std::vector<uint8_t>{0x86, 14, 0});
    const auto count = fake.writes.size();
    CHECK_FALSE(device->setClock(0));
    CHECK_FALSE(device->setClock(91));
    CHECK_FALSE(device->setClock(6000001));
    CHECK(fake.writes.size() == count);
}

TEST_CASE_METHOD(Fixture, "Linux H-series channel selection and clock base", "[linux]") {
    fake.devices = {{0x6010, 0x0700, "HIGH"}};
    ProgrammerSelector selector;
    selector.serial = "HIGHB";
    auto device = FtdiI2c::open(selector);
    REQUIRE(device);
    CHECK(fake.interface == INTERFACE_B);
    CHECK(fake.writes[2] == std::vector<uint8_t>{0x8B, 0x8D, 0x97});
}

TEST_CASE_METHOD(Fixture, "Linux repeated START reads ACK all but the final byte", "[linux]") {
    auto device = FtdiI2c::open({});
    REQUIRE(device);
    fake.writes.clear();
    fake.replies = {0xFE, 0, 0, 0xEF, 0xAA}; // upper ACK bits are irrelevant
    fake.reads = {0, 1}; // libftdi may first return only modem-status data
    const std::array<uint8_t, 1> tx{0x11};
    std::array<uint8_t, 2> rx{};
    REQUIRE(device->writeRead(0x49, tx, rx));
    CHECK(rx == std::array<uint8_t, 2>{0xEF, 0xAA});
    CHECK(fake.writes == std::vector<std::vector<uint8_t>>{
        start(), sendByte(0x92), sendByte(0x11), start(), sendByte(0x93),
        {0x80, 0, 0x11, 0x22, 7, 0x80, 0, 0x13, 0x13, 0, 0, 0x80, 0, 0x11, 0x87},
        {0x80, 0, 0x11, 0x22, 7, 0x80, 0, 0x11, 0x13, 0, 0xFF, 0x80, 0, 0x11, 0x87}, stop()});
}

TEST_CASE_METHOD(Fixture, "Linux transfer errors stop the bus without retrying commands", "[linux]") {
    auto device = FtdiI2c::open({});
    REQUIRE(device);
    fake.writes.clear();
    ErrorCode expected = ErrorCode::I2cTransfer;
    SECTION("address NACK") { fake.replies = {1}; expected = ErrorCode::I2cNack; }
    SECTION("data NACK") { fake.replies = {0, 1}; expected = ErrorCode::I2cNack; }
    SECTION("timeout") { fake.replies.clear(); expected = ErrorCode::Timeout; }
    SECTION("short write") { fake.shortWriteAt = 1; }
    SECTION("driver failure") { fake.failure = "write"; expected = ErrorCode::Ftdi; }
    const std::array<uint8_t, 1> tx{0x11};
    std::array<uint8_t, 1> rx{};
    auto result = device->writeRead(0x49, tx, rx);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == expected);
    CHECK(fake.writes.back() == stop());
    CHECK(std::count(fake.writes.begin(), fake.writes.end(), start()) == 1);
    CHECK(std::count(fake.writes.begin(), fake.writes.end(), sendByte(0x92)) <= 1);
    if (fake.failure == "write") CHECK(result.error().message.find("STOP also failed") != std::string::npos);
}

TEST_CASE_METHOD(Fixture, "Linux standalone reads and address-only writes", "[linux]") {
    auto device = FtdiI2c::open({});
    REQUIRE(device);
    fake.writes.clear();
    fake.replies = {0};
    REQUIRE(device->write(0x49, {}));
    CHECK(fake.writes == std::vector<std::vector<uint8_t>>{start(), sendByte(0x92), stop()});
    fake.replies = {0, 0xA5};
    std::array<uint8_t, 1> rx{};
    REQUIRE(device->read(0x49, rx));
    CHECK(rx[0] == 0xA5);
    CHECK(fake.writes.back() == stop());
    const auto count = fake.writes.size();
    CHECK_FALSE(device->read(0x49, {}));
    CHECK_FALSE(device->write(0x80, {}));
    CHECK(fake.writes.size() == count);
}

TEST_CASE_METHOD(Fixture, "Linux initialization failures release all resources exactly once", "[linux]") {
    int closes = 0;
    SECTION("allocation") { fake.failure = "new"; }
    SECTION("USB list") { fake.failure = "list"; }
    SECTION("permissions") { fake.failure = "descriptor open"; }
    SECTION("descriptor string") { fake.failure = "string"; }
    SECTION("interface") { fake.failure = "interface"; }
    SECTION("busy") { fake.openStatus = -5; }
    SECTION("flush") { fake.failure = "flush"; closes = 1; }
    SECTION("MPSSE mode") { fake.failure = "mode"; closes = 1; }
    SECTION("synchronization") { fake.replies = {0, 0}; closes = 1; }
    SECTION("synchronization timeout") { fake.replies.clear(); closes = 1; }
    auto result = FtdiI2c::open({});
    REQUIRE_FALSE(result);
    if (fake.openStatus == -5) CHECK(result.error().code == ErrorCode::DeviceBusy);
    CHECK(fake.closes == closes);
}
