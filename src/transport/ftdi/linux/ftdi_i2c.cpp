#include "mstar/transport/ftdi/ftdi_i2c.hpp"

#include <array>
#include <chrono>
#include <limits>
#include <string>
#include <utility>

#include <ftdi.h>
#include <libusb.h>
#include <fmt/format.h>

namespace mstar::ftdi {
namespace {
auto fail(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

auto usbError(int status, const char* operation) {
    return fail(ErrorCode::Usb, fmt::format("{}: {}; check USB permissions/udev and connection",
                                          operation, libusb_error_name(status)));
}

// Retain the inventory until open completes: a second enumeration could reorder devices.
struct Inventory {
    libusb_device** devices = nullptr;
    struct Channel {
        ProgrammerInfo info;
        libusb_device* device;
        ftdi_interface interface;
        ftdi_chip_type type;
    };
    std::vector<Channel> channels;
    ~Inventory() { if (devices) libusb_free_device_list(devices, 1); }

    Result<void> scan(ftdi_context* ctx, bool allowUnavailable = false) {
        if (!ctx) return fail(ErrorCode::Usb, "ftdi_new failed");
        const auto count = libusb_get_device_list(ctx->usb_ctx, &devices);
        if (count < 0) return usbError(static_cast<int>(count), "USB enumeration failed");
        for (std::ptrdiff_t i = 0; i < count; ++i) {
            libusb_device_descriptor desc{};
            if (const int r = libusb_get_device_descriptor(devices[i], &desc); r < 0)
                return usbError(r, "USB descriptor failed");
            if (desc.idVendor != 0x0403) continue;
            const char* kind = nullptr;
            ftdi_chip_type type = TYPE_2232C;
            int ports = 1;
            // Standard FTDI IDs and chip revisions; only MPSSE-capable channels.
            if (desc.idProduct == 0x6010 && desc.bcdDevice == 0x0500) {
                kind = "FT2232C/D";
            } else if (desc.idProduct == 0x6010 && desc.bcdDevice == 0x0700) {
                kind = "FT2232H"; type = TYPE_2232H; ports = 2;
            } else if (desc.idProduct == 0x6011 && desc.bcdDevice == 0x0800) {
                kind = "FT4232H"; type = TYPE_4232H; ports = 2;
            } else if (desc.idProduct == 0x6014 && desc.bcdDevice == 0x0900) {
                kind = "FT232H"; type = TYPE_232H;
            } else {
                continue;
            }
            // Reading descriptors claims no interface and leaves the UART driver attached.
            libusb_device_handle* raw = nullptr;
            const int opened = libusb_open(devices[i], &raw);
            if (opened < 0 && !allowUnavailable)
                return usbError(opened, "USB descriptor open failed");
            std::unique_ptr<libusb_device_handle, decltype(&libusb_close)> handle(raw, libusb_close);
            const auto string = [&](uint8_t index) -> Result<std::string> {
                if (opened < 0) return usbError(opened, "USB descriptor open failed");
                if (index == 0) return std::string{};
                std::array<unsigned char, 256> buffer{};
                const int size = libusb_get_string_descriptor_ascii(raw, index, buffer.data(),
                                                                    static_cast<int>(buffer.size()));
                if (size < 0) return usbError(size, "USB string failed");
                return std::string(reinterpret_cast<char*>(buffer.data()), size);
            };
            auto serial = string(desc.iSerialNumber);
            if (!serial && !allowUnavailable) return std::unexpected(serial.error());
            auto description = string(desc.iProduct);
            if (!description && !allowUnavailable) return std::unexpected(description.error());
            const std::string accessError = !serial ? serial.error().message :
                                            !description ? description.error().message : "";
            const auto usbLocation = fmt::format("{}:{}", libusb_get_bus_number(devices[i]),
                                                libusb_get_device_address(devices[i]));
            for (int port = 0; port < ports; ++port) {
                std::string letter(1, static_cast<char>('A' + port));
                // Match D2XX's per-channel serials on multi-interface chips.
                channels.push_back({ProgrammerInfo{
                    .kind = kind,
                    .description = description && !description->empty() ?
                        *description + (type == TYPE_232H ? "" : " " + letter) : "",
                    .serial = serial && !serial->empty() ?
                        *serial + (type == TYPE_232H ? "" : letter) : "",
                    .location = letter,
                    .inUse = false, // libusb reports busy only when claiming the interface
                    .comPort = std::nullopt,
                    .usbLocation = usbLocation,
                    .accessError = accessError,
                }, devices[i], port == 0 ? INTERFACE_A : INTERFACE_B, type});
            }
        }
        return {};
    }
};
} // namespace

struct FtdiI2c::Impl {
    std::unique_ptr<ftdi_context, decltype(&ftdi_free)> ctx{ftdi_new(), ftdi_free};
    ~Impl() { if (ctx && ctx->usb_dev) ftdi_usb_close(ctx.get()); }

    Result<void> check(int status, const char* operation) {
        if (status >= 0) return {};
        return fail(ErrorCode::Ftdi, fmt::format("{} failed ({}): {}", operation, status,
                                               ftdi_get_error_string(ctx.get())));
    }

    Result<void> send(std::span<const uint8_t> bytes) {
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return fail(ErrorCode::InvalidArgument, "FTDI command buffer too large");
        const int sent = ftdi_write_data(ctx.get(), bytes.data(), static_cast<int>(bytes.size()));
        if (auto r = check(sent, "ftdi_write_data"); !r) return r;
        if (static_cast<std::size_t>(sent) != bytes.size())
            return fail(ErrorCode::I2cTransfer, "Short FTDI write; commands were not retried");
        return {};
    }

    Result<void> receive(std::span<uint8_t> bytes) {
        using Clock = std::chrono::steady_clock;
        const auto deadline = Clock::now() + std::chrono::seconds(1);
        std::size_t received = 0;
        while (received < bytes.size()) {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
            if (remaining.count() <= 0) return fail(ErrorCode::Timeout, "FTDI read timed out");
            ctx->usb_read_timeout = static_cast<int>(remaining.count());
            // Read one byte at a time from libftdi's internal buffer, keeping one
            // deadline even when USB returns only status bytes or partial data.
            const int count = ftdi_read_data(ctx.get(), bytes.data() + received, 1);
            if (count == LIBUSB_ERROR_TIMEOUT) return fail(ErrorCode::Timeout, "FTDI read timed out");
            if (auto r = check(count, "ftdi_read_data"); !r) return r;
            if (count > 1) return fail(ErrorCode::I2cTransfer, "Oversized FTDI read");
            received += static_cast<std::size_t>(count);
        }
        return {};
    }

    Result<void> setClock(uint32_t hz) {
        // Two-phase 12 MHz MPSSE: SCL = 6 MHz / (divisor + 1).
        // Round up the divisor so the selected clock never exceeds the request.
        if (hz < 92 || hz > 6000000)
            return fail(ErrorCode::InvalidArgument, "I2C clock must be in 92..6000000 Hz");
        const uint32_t divisor = (6000000u + hz - 1) / hz - 1;
        const std::array<uint8_t, 3> command{0x86, static_cast<uint8_t>(divisor),
                                           static_cast<uint8_t>(divisor >> 8)};
        return send(command);
    }

    Result<void> initialize(const Inventory::Channel& channel) {
        ctx->module_detach_mode = AUTO_DETACH_REATACH_SIO_MODULE;
        ctx->usb_read_timeout = 1000;
        ctx->usb_write_timeout = 1000;
        if (auto r = check(ftdi_set_interface(ctx.get(), channel.interface), "ftdi_set_interface"); !r)
            return r;
        const int opened = ftdi_usb_open_dev(ctx.get(), channel.device);
        if (opened < 0)
            return fail(opened == -5 ? ErrorCode::DeviceBusy : ErrorCode::Usb,
                        fmt::format("ftdi_usb_open_dev failed ({}): {}; check USB permissions/udev "
                                    "and other programs using the adapter", opened,
                                    ftdi_get_error_string(ctx.get())));
        if (ctx->type != channel.type) return fail(ErrorCode::Ftdi, "Unexpected FTDI chip type");
        if (auto r = check(ftdi_tcioflush(ctx.get()), "ftdi_tcioflush"); !r) return r;
        if (auto r = check(ftdi_set_latency_timer(ctx.get(), 1), "ftdi_set_latency_timer"); !r) return r;
        if (auto r = check(ftdi_set_bitmode(ctx.get(), 0, BITMODE_RESET), "MPSSE reset"); !r) return r;
        if (auto r = check(ftdi_set_bitmode(ctx.get(), 0, BITMODE_MPSSE), "MPSSE enable"); !r) return r;
        for (uint8_t invalid : {uint8_t{0xAA}, uint8_t{0xAB}}) {
            const std::array<uint8_t, 2> command{invalid, 0x87};
            if (auto r = send(command); !r) return r;
            std::array<uint8_t, 2> reply{};
            if (auto r = receive(reply); !r) return r;
            if (reply != std::array<uint8_t, 2>{0xFA, invalid})
                return fail(ErrorCode::Ftdi, "MPSSE synchronization response does not match");
        }
        if (ctx->type != TYPE_2232C) {
            // H-series: use the same 12 MHz base and two-phase clocking as FT2232D.
            constexpr std::array<uint8_t, 3> mode{0x8B, 0x8D, 0x97};
            if (auto r = send(mode); !r) return r;
        }
        constexpr std::array<uint8_t, 4> idle{0x85, 0x80, 3, 0x10}; // loopback off, release bus
        if (auto r = send(idle); !r) return r;
        return setClock(100000);
    }

    // Match LibMPSSE's GPIO timing and direction changes (ftdi_i2c.c in the
    // vendored SDK). AD0=SCL, AD1=SDA output, AD2=SDA input. AD4 stays low
    // as in LibMPSSE. No H-only commands.
    Result<void> start() {
        std::vector<uint8_t> command;
        // Set SDA high while SCL is low before a repeated START.
        command.insert(command.end(), {0x80, 2, 0x13});
        for (uint8_t value : {uint8_t{3}, uint8_t{1}}) {
            for (int i = 0; i < (value == 3 ? 10 : 20); ++i)
                command.insert(command.end(), {0x80, value, 0x13});
        }
        command.insert(command.end(), {0x80, 0, 0x13});
        return send(command);
    }

    Result<void> finish(Result<void> result) {
        std::vector<uint8_t> command;
        for (uint8_t value : {uint8_t{0}, uint8_t{1}, uint8_t{3}})
            for (int i = 0; i < 10; ++i) command.insert(command.end(), {0x80, value, 0x13});
        command.insert(command.end(), {0x80, 3, 0x10}); // release SCL/SDA
        if (auto stop = send(command); !stop) {
            if (result) return stop;
            result.error().message += "; STOP also failed: " + stop.error().message;
        }
        return result;
    }

    Result<void> sendByte(uint8_t byte) {
        const std::array<uint8_t, 12> command{
            0x80, 2, 0x13, 0x13, 7, byte, 0x80, 0, 0x11, 0x22, 0, 0x87};
        if (auto r = send(command); !r) return r;
        std::array<uint8_t, 1> ack{};
        if (auto r = receive(ack); !r) return r;
        if (ack[0] & 1u) return fail(ErrorCode::I2cNack, fmt::format("I2C NACK sending {:#04x}", byte));
        return {};
    }

    Result<void> writePhase(uint8_t address, std::span<const uint8_t> data) {
        if (auto r = start(); !r) return r;
        if (auto r = sendByte(static_cast<uint8_t>(address << 1)); !r) return r;
        for (uint8_t byte : data) if (auto r = sendByte(byte); !r) return r;
        return {};
    }

    Result<void> readPhase(uint8_t address, std::span<uint8_t> data) {
        if (auto r = start(); !r) return r;
        if (auto r = sendByte(static_cast<uint8_t>((address << 1) | 1)); !r) return r;
        for (std::size_t i = 0; i < data.size(); ++i) {
            const bool last = i + 1 == data.size();
            const std::array<uint8_t, 15> command{
                0x80, 0, 0x11, 0x22, 7,
                0x80, 0, static_cast<uint8_t>(last ? 0x11 : 0x13),
                0x13, 0, static_cast<uint8_t>(last ? 0xFF : 0),
                0x80, 0, 0x11, 0x87};
            if (auto r = send(command); !r) return r;
            if (auto r = receive(data.subspan(i, 1)); !r) return r;
        }
        return {};
    }
};

FtdiI2c::FtdiI2c(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FtdiI2c::FtdiI2c(FtdiI2c&&) noexcept = default;
FtdiI2c& FtdiI2c::operator=(FtdiI2c&&) noexcept = default;
FtdiI2c::~FtdiI2c() = default;

Result<std::vector<ProgrammerInfo>> FtdiI2c::enumerate() {
    Impl impl;
    Inventory inventory;
    if (auto r = inventory.scan(impl.ctx.get(), true); !r) return std::unexpected(r.error());
    std::vector<ProgrammerInfo> info;
    for (auto& channel : inventory.channels) info.push_back(std::move(channel.info));
    return info;
}

Result<FtdiI2c> FtdiI2c::open(const ProgrammerSelector& selector) {
    auto impl = std::make_unique<Impl>();
    Inventory inventory;
    if (auto r = inventory.scan(impl->ctx.get()); !r) return std::unexpected(r.error());
    const Inventory::Channel* selected = nullptr;
    for (const auto& channel : inventory.channels) {
        const auto& info = channel.info;
        if (selector.serial && *selector.serial != info.serial) continue;
        if (selector.description && *selector.description != info.description) continue;
        if (selector.location && *selector.location != info.location) continue;
        if (selected) return fail(ErrorCode::InvalidArgument,
                                  "Multiple FTDI channels match; narrow the selection with --serial");
        selected = &channel;
    }
    if (!selected) return fail(ErrorCode::DeviceNotFound, "No matching MPSSE-capable FTDI channel found");
    if (auto r = impl->initialize(*selected); !r) return std::unexpected(r.error());
    return FtdiI2c(std::move(impl));
}

Result<void> FtdiI2c::setClock(uint32_t hz) { return impl_->setClock(hz); }
Result<void> FtdiI2c::write(uint8_t address, std::span<const uint8_t> data) {
    if (address > 0x7F) return fail(ErrorCode::InvalidArgument, "I2C address must be 7-bit");
    return impl_->finish(impl_->writePhase(address, data));
}
Result<void> FtdiI2c::read(uint8_t address, std::span<uint8_t> data) {
    if (address > 0x7F || data.empty())
        return fail(ErrorCode::InvalidArgument, "I2C read needs a 7-bit address and nonempty buffer");
    return impl_->finish(impl_->readPhase(address, data));
}
Result<void> FtdiI2c::writeRead(uint8_t address, std::span<const uint8_t> tx, std::span<uint8_t> rx) {
    if (address > 0x7F || rx.empty())
        return fail(ErrorCode::InvalidArgument, "I2C writeRead needs a 7-bit address and nonempty read buffer");
    auto result = impl_->writePhase(address, tx);
    if (result) result = impl_->readPhase(address, rx);
    return impl_->finish(result);
}
} // namespace mstar::ftdi
