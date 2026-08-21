#include "mstar/transport/ftdi/ftdi_i2c.hpp"

#include <cstdio>
#include <optional>
#include <string>

#include <ftd2xx.h>
#include <libmpsse_i2c.h>

#include "ftdi_device.hpp"

// Milestone 2: real I2C transactions via D2XX + LibMPSSE-I2C.

namespace mstar::ftdi {

namespace {

constexpr uint32_t kDefaultClockHz = 100000;
constexpr UCHAR kLatencyTimerMs = 16;

constexpr DWORD kWriteOptions =
    I2C_TRANSFER_OPTIONS_START_BIT | I2C_TRANSFER_OPTIONS_STOP_BIT |
    I2C_TRANSFER_OPTIONS_BREAK_ON_NACK;
// No STOP: leaves the bus ready for a repeated START (see writeRead()).
constexpr DWORD kWriteNoStopOptions =
    I2C_TRANSFER_OPTIONS_START_BIT | I2C_TRANSFER_OPTIONS_BREAK_ON_NACK;
constexpr DWORD kReadOptions =
    I2C_TRANSFER_OPTIONS_START_BIT | I2C_TRANSFER_OPTIONS_STOP_BIT |
    I2C_TRANSFER_OPTIONS_BREAK_ON_NACK | I2C_TRANSFER_OPTIONS_NACK_LAST_BYTE;

std::string hexByte(uint8_t value) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%02X", value);
    return buf;
}

Error ftdiError(ErrorCode code, const char* what, FT_STATUS status) {
    return Error{code, std::string(what) + " failed with status " + std::to_string(status)};
}

} // namespace

struct FtdiI2c::Impl {
    FT_HANDLE handle = nullptr;
    uint32_t clockHz = kDefaultClockHz;

    ~Impl() {
        if (handle != nullptr) {
            I2C_CloseChannel(handle);
        }
    }

    Result<void> reinit() {
        ChannelConfig config{};
        config.ClockRate = static_cast<I2C_CLOCKRATE>(clockHz);
        config.LatencyTimer = kLatencyTimerMs;
        // 3-phase clocking is an FT2232H/FT4232H/FT232H-only MPSSE feature.
        // I2C_InitChannel enables it unless told otherwise, and the FT2232D
        // used by the MStar programmer doesn't understand that command:
        // it silently desyncs the MPSSE reply stream, which makes every
        // subsequent ACK bit read back as a false ACK.
        config.Options = I2C_DISABLE_3PHASE_CLOCKING;
        config.Pin = 0;
        config.currentPinState = 0;

        if (FT_STATUS status = I2C_InitChannel(handle, &config); status != FT_OK) {
            return std::unexpected(ftdiError(ErrorCode::Ftdi, "I2C_InitChannel", status));
        }
        return {};
    }

    // Issues a bare STOP condition, used to release the bus after a write
    // that was meant to be followed by a repeated START fails partway.
    void recoverBusWithStop(uint8_t address) {
        DWORD transferred = 0;
        UCHAR dummy = 0;
        I2C_DeviceWrite(handle, address, 0, &dummy, &transferred, I2C_TRANSFER_OPTIONS_STOP_BIT);
    }
};

FtdiI2c::FtdiI2c(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FtdiI2c::FtdiI2c(FtdiI2c&&) noexcept = default;
FtdiI2c& FtdiI2c::operator=(FtdiI2c&&) noexcept = default;
FtdiI2c::~FtdiI2c() = default;

Result<std::vector<ProgrammerInfo>> FtdiI2c::enumerate() {
    return detail::enumerateDevices();
}

Result<FtdiI2c> FtdiI2c::open(const ProgrammerSelector& selector) {
    DWORD numChannels = 0;
    if (FT_STATUS status = I2C_GetNumChannels(&numChannels); status != FT_OK) {
        return std::unexpected(ftdiError(ErrorCode::Ftdi, "I2C_GetNumChannels", status));
    }
    if (numChannels == 0) {
        return std::unexpected(Error{
            ErrorCode::DeviceNotFound, "No MPSSE-capable FTDI channels found"});
    }

    std::optional<DWORD> chosen;
    for (DWORD i = 0; i < numChannels; ++i) {
        FT_DEVICE_LIST_INFO_NODE info{};
        if (I2C_GetChannelInfo(i, &info) != FT_OK) {
            continue;
        }

        std::string serial = detail::boundedString(info.SerialNumber, sizeof(info.SerialNumber));
        std::string description =
            detail::boundedString(info.Description, sizeof(info.Description));
        std::string channel = detail::channelFromSerial(serial);

        if (selector.serial && *selector.serial != serial) continue;
        if (selector.description && *selector.description != description) continue;
        if (selector.location && *selector.location != channel) continue;

        if (chosen.has_value()) {
            return std::unexpected(Error{
                ErrorCode::InvalidArgument,
                "Multiple FTDI channels match the given selector; narrow the "
                "selection with --serial"});
        }
        chosen = i;
    }

    if (!chosen) {
        return std::unexpected(Error{ErrorCode::DeviceNotFound, "No matching FTDI channel found"});
    }

    FT_HANDLE handle = nullptr;
    if (FT_STATUS status = I2C_OpenChannel(*chosen, &handle); status != FT_OK) {
        return std::unexpected(ftdiError(ErrorCode::DeviceBusy, "I2C_OpenChannel", status));
    }

    auto impl = std::make_unique<Impl>();
    impl->handle = handle;
    impl->clockHz = kDefaultClockHz;

    if (auto result = impl->reinit(); !result) {
        I2C_CloseChannel(handle);
        return std::unexpected(result.error());
    }

    return FtdiI2c(std::move(impl));
}

Result<void> FtdiI2c::setClock(uint32_t hz) {
    impl_->clockHz = hz;
    return impl_->reinit();
}

Result<void> FtdiI2c::write(uint8_t address, std::span<const uint8_t> data) {
    DWORD transferred = 0;
    // LibMPSSE's C API takes a non-const buffer even for writes; it does
    // not modify it.
    FT_STATUS status = I2C_DeviceWrite(
        impl_->handle, address, static_cast<DWORD>(data.size()),
        const_cast<UCHAR*>(data.data()), &transferred, kWriteOptions);

    if (status != FT_OK) {
        return std::unexpected(ftdiError(ErrorCode::I2cTransfer, "I2C_DeviceWrite", status));
    }
    if (transferred != data.size()) {
        return std::unexpected(Error{
            ErrorCode::I2cNack,
            "I2C write to " + hexByte(address) + " was NACKed after " +
                std::to_string(transferred) + "/" + std::to_string(data.size()) + " bytes"});
    }
    return {};
}

Result<void> FtdiI2c::read(uint8_t address, std::span<uint8_t> data) {
    DWORD transferred = 0;
    FT_STATUS status = I2C_DeviceRead(
        impl_->handle, address, static_cast<DWORD>(data.size()), data.data(), &transferred,
        kReadOptions);

    if (status != FT_OK) {
        return std::unexpected(ftdiError(ErrorCode::I2cTransfer, "I2C_DeviceRead", status));
    }
    if (transferred != data.size()) {
        return std::unexpected(Error{
            ErrorCode::I2cTransfer,
            "I2C read from " + hexByte(address) + " returned " + std::to_string(transferred) +
                "/" + std::to_string(data.size()) + " bytes"});
    }
    return {};
}

Result<void> FtdiI2c::writeRead(
    uint8_t address,
    std::span<const uint8_t> tx,
    std::span<uint8_t> rx
) {
    DWORD txTransferred = 0;
    FT_STATUS status = I2C_DeviceWrite(
        impl_->handle, address, static_cast<DWORD>(tx.size()),
        const_cast<UCHAR*>(tx.data()), &txTransferred, kWriteNoStopOptions);

    if (status != FT_OK || txTransferred != tx.size()) {
        impl_->recoverBusWithStop(address);
        if (status != FT_OK) {
            return std::unexpected(ftdiError(ErrorCode::I2cTransfer, "I2C_DeviceWrite", status));
        }
        return std::unexpected(Error{
            ErrorCode::I2cNack,
            "I2C write to " + hexByte(address) + " was NACKed before the repeated START"});
    }

    DWORD rxTransferred = 0;
    status = I2C_DeviceRead(
        impl_->handle, address, static_cast<DWORD>(rx.size()), rx.data(), &rxTransferred,
        kReadOptions);

    if (status != FT_OK) {
        return std::unexpected(ftdiError(ErrorCode::I2cTransfer, "I2C_DeviceRead", status));
    }
    if (rxTransferred != rx.size()) {
        return std::unexpected(Error{
            ErrorCode::I2cTransfer,
            "I2C read from " + hexByte(address) + " returned " + std::to_string(rxTransferred) +
                "/" + std::to_string(rx.size()) + " bytes"});
    }
    return {};
}

} // namespace mstar::ftdi
