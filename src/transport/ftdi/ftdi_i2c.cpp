#include "mstar/transport/ftdi/ftdi_i2c.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#include <ftd2xx.h>
#include <libmpsse_i2c.h>

#include "ftdi_device.hpp"

namespace mstar::ftdi {
namespace {
constexpr uint32_t kDefaultClockHz = 100000;
constexpr UCHAR kLatencyTimerMs = 16;
constexpr DWORD kIoTimeoutMs = 1000;
constexpr std::size_t kMaxBatchBytes = 1024;
constexpr std::size_t kControlBytes = 16;
constexpr std::size_t kCheckedPrefixBytes = 4;
// Preserve settling time that the bytewise USB round trips used to provide.
// Without these holds, this FT2232D board returns false ACKs / EF 7F 7F IDs.
constexpr int kSettleGpioWrites = 20;

std::string hexByte(uint8_t value) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%02X", value);
    return buf;
}

Error ftdiError(ErrorCode code, const char* what, FT_STATUS status) {
    return Error{code, std::string(what) + " failed with status " + std::to_string(status)};
}

// Match the existing LibMPSSE/Linux pin assignment and GPIO timing:
// AD0=SCL, AD1=SDA output, AD2=SDA input, AD4 held low.
// Use only FT2232D-compatible two-phase commands.
void gpio(std::vector<uint8_t>& command, uint8_t value, uint8_t direction, int count = 1) {
    for (int i = 0; i < count; ++i)
        command.insert(command.end(), {0x80, value, direction});
}

void appendStart(std::vector<uint8_t>& command) {
    // Raise SDA with SCL low first, including before a repeated START.
    gpio(command, 0, 0x11);
    gpio(command, 2, 0x13);
    gpio(command, 3, 0x13, 10);
    gpio(command, 1, 0x13, 20);
    gpio(command, 0, 0x13);
}

void appendWriteByte(std::vector<uint8_t>& command, uint8_t value) {
    command.insert(command.end(), {
        0x80, 2, 0x13, 0x13, 7, value});
    // Keep the last data bit stable as SCL falls, then release SDA and let
    // it settle before sampling the slave ACK on AD2.
    gpio(command, (value & 1) ? 2 : 0, 0x13, kSettleGpioWrites);
    gpio(command, 0, 0x11, kSettleGpioWrites);
    command.insert(command.end(), {0x22, 0});
}

void appendReadByte(std::vector<uint8_t>& command, bool last) {
    command.insert(command.end(), {
        0x80, 0, 0x11, 0x22, 7,
        0x80, 0, static_cast<uint8_t>(last ? 0x11 : 0x13),
        0x13, 0, static_cast<uint8_t>(last ? 0xFF : 0),
        0x80, 0, 0x11});
    // Hold SCL low after releasing the master ACK before the next byte.
    gpio(command, 0, 0x11, kSettleGpioWrites);
}
} // namespace

struct FtdiI2c::Impl {
    FT_HANDLE handle = nullptr;
    uint32_t clockHz = kDefaultClockHz;
    bool usable = false;

    ~Impl() {
        if (handle != nullptr) I2C_CloseChannel(handle);
    }

    Result<void> ready() const {
        if (!usable)
            return std::unexpected(Error{ErrorCode::I2cTransfer,
                "FTDI channel is unusable after an I/O or initialization failure; close and reopen it"});
        return {};
    }

    Result<void> reinit() {
        usable = false;
        ChannelConfig config{};
        config.ClockRate = static_cast<I2C_CLOCKRATE>(clockHz);
        config.LatencyTimer = kLatencyTimerMs;
        // FT2232D cannot execute the H-series three-phase clocking command.
        // Enabling it corrupts the response stream and produces false ACKs.
        config.Options = I2C_DISABLE_3PHASE_CLOCKING;
        config.Pin = 0;
        config.currentPinState = 0;

        if (FT_STATUS status = I2C_InitChannel(handle, &config); status != FT_OK)
            return std::unexpected(ftdiError(ErrorCode::Ftdi, "I2C_InitChannel", status));
        // LibMPSSE still owns discovery, initialization and handle lifetime.
        // Transfers use D2XX directly with bounded waits and explicit lengths.
        if (FT_STATUS status = FT_SetTimeouts(handle, kIoTimeoutMs, kIoTimeoutMs); status != FT_OK)
            return std::unexpected(ftdiError(ErrorCode::Ftdi, "FT_SetTimeouts", status));
        usable = true;
        return {};
    }

    Result<void> ioFailure(Error error) {
        usable = false;
        // A short command write can end inside an MPSSE instruction. Appending
        // STOP bytes could then clock them as payload. A failed read can leave
        // late replies queued. Disable MPSSE out of band and prohibit reuse;
        // never replay a command or let stale bytes become another call's ACK.
        if (FT_STATUS status = FT_SetBitMode(handle, 0, 0); status != FT_OK)
            error.message += "; MPSSE reset also failed with status " + std::to_string(status);
        error.message += "; FTDI channel unusable; close and reopen it (commands were not retried)";
        return std::unexpected(std::move(error));
    }

    Result<void> send(std::span<const uint8_t> command) {
        DWORD actual = 0;
        const auto requested = static_cast<DWORD>(command.size()); // internally bounded buffers
        const FT_STATUS status = FT_Write(
            handle, const_cast<uint8_t*>(command.data()), requested, &actual);
        if (status != FT_OK)
            return ioFailure(ftdiError(ErrorCode::I2cTransfer, "FT_Write", status));
        if (actual != requested)
            return ioFailure(Error{ErrorCode::I2cTransfer,
                "Short/invalid FTDI write: " + std::to_string(actual) + "/" +
                std::to_string(requested) + " command bytes"});
        return {};
    }

    Result<void> receive(std::span<uint8_t> data) {
        DWORD actual = 0;
        const auto requested = static_cast<DWORD>(data.size());
        const FT_STATUS status = FT_Read(handle, data.data(), requested, &actual);
        if (status != FT_OK)
            return ioFailure(ftdiError(ErrorCode::I2cTransfer, "FT_Read", status));
        // D2XX returns FT_OK with a short (including zero-length) read on timeout.
        if (actual != requested)
            return ioFailure(Error{actual < requested ? ErrorCode::Timeout : ErrorCode::I2cTransfer,
                "Short/invalid FTDI read: " + std::to_string(actual) + "/" +
                std::to_string(requested) + " response bytes"});
        return {};
    }

    std::size_t batchSize() const {
        // At most about 225 ms of wire time, even at diagnostic slow clocks.
        // Keep responses and command buffers bounded; a 2 KiB program payload
        // normally needs two batches. The driver continuously drains USB RX.
        return std::clamp<std::size_t>(clockHz / 40, 1, kMaxBatchBytes);
    }

    Result<void> addressPhase(uint8_t address, bool reading) {
        std::vector<uint8_t> command;
        appendStart(command);
        appendWriteByte(command, static_cast<uint8_t>((address << 1) | (reading ? 1 : 0)));
        command.push_back(0x87);
        if (auto r = send(command); !r) return r;
        std::array<uint8_t, 1> ack{};
        if (auto r = receive(ack); !r) return r;
        if (ack[0] & 1)
            return std::unexpected(Error{ErrorCode::I2cNack,
                "I2C " + std::string(reading ? "read" : "write") +
                " address " + hexByte(address) + " was NACKed"});
        return {};
    }

    Result<void> writePhase(uint8_t address, std::span<const uint8_t> data) {
        if (auto r = addressPhase(address, false); !r) return r;
        // Control commands retain immediate per-byte NACK handling. For longer
        // writes, check the prefix separately (includes the ISP/NAND header)
        // before any bulk payload is submitted.
        const auto checked = data.size() <= kControlBytes ? data.size() : kCheckedPrefixBytes;
        std::vector<uint8_t> command;
        std::array<uint8_t, kMaxBatchBytes> acks{};
        for (std::size_t offset = 0; offset < data.size();) {
            const auto count = offset < checked ? std::size_t{1} :
                (std::min)(batchSize(), data.size() - offset);
            command.clear();
            for (auto value : data.subspan(offset, count)) appendWriteByte(command, value);
            command.push_back(0x87); // one flush and one ACK read for the whole batch
            if (auto r = send(command); !r) return r;
            if (auto r = receive(std::span(acks).first(count)); !r) return r;
            for (std::size_t i = 0; i < count; ++i) {
                if (acks[i] & 1)
                    return std::unexpected(Error{ErrorCode::I2cNack,
                        "I2C write to " + hexByte(address) + " was NACKed at data byte " +
                        std::to_string(offset + i) + "; remaining batches were not sent"});
            }
            offset += count;
        }
        return {};
    }

    Result<void> readPhase(uint8_t address, std::span<uint8_t> data) {
        if (auto r = addressPhase(address, true); !r) return r;
        std::vector<uint8_t> command;
        for (std::size_t offset = 0; offset < data.size();) {
            const auto count = (std::min)(batchSize(), data.size() - offset);
            command.clear();
            for (std::size_t i = 0; i < count; ++i)
                appendReadByte(command, offset + i + 1 == data.size());
            command.push_back(0x87);
            if (auto r = send(command); !r) return r;
            if (auto r = receive(data.subspan(offset, count)); !r) return r;
            offset += count;
        }
        return {};
    }

    Result<void> finish(Result<void> result) {
        // NACKs leave the parser synchronized, so always send STOP. Transport
        // failures already disabled the engine; never append bytes to an
        // uncertain command stream.
        if (!usable) return result;
        std::vector<uint8_t> command;
        gpio(command, 0, 0x11); // lower SCL before driving SDA for STOP
        for (uint8_t value : {uint8_t{0}, uint8_t{1}, uint8_t{3}})
            gpio(command, value, 0x13, 10);
        gpio(command, 3, 0x10); // release SCL/SDA, retain AD4 low
        if (auto stop = send(command); !stop) {
            if (result) return stop;
            result.error().message += "; STOP also failed: " + stop.error().message;
        }
        return result;
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
        if (selector.firstMatch) break;
    }

    if (!chosen) {
        return std::unexpected(Error{ErrorCode::DeviceNotFound, "No matching FTDI channel found"});
    }

    // Allocate the owner before acquiring the handle. Once open succeeds,
    // Impl owns cleanup on both initialization failure and normal destruction.
    auto impl = std::make_unique<Impl>();
    if (FT_STATUS status = I2C_OpenChannel(*chosen, &impl->handle); status != FT_OK) {
        return std::unexpected(ftdiError(ErrorCode::DeviceBusy, "I2C_OpenChannel", status));
    }

    impl->clockHz = kDefaultClockHz;

    if (auto result = impl->reinit(); !result) {
        return std::unexpected(result.error());
    }

    return FtdiI2c(std::move(impl));
}

Result<void> FtdiI2c::setClock(uint32_t hz) {
    if (hz < 92 || hz > 6000000)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "I2C clock must be in 92..6000000 Hz"});
    if (auto r = impl_->ready(); !r) return r;
    impl_->clockHz = hz;
    return impl_->reinit();
}

Result<void> FtdiI2c::write(uint8_t address, std::span<const uint8_t> data) {
    if (address > 0x7F)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "I2C address must be 7-bit"});
    if (auto r = impl_->ready(); !r) return r;
    return impl_->finish(impl_->writePhase(address, data));
}

Result<void> FtdiI2c::read(uint8_t address, std::span<uint8_t> data) {
    if (address > 0x7F || data.empty())
        return std::unexpected(Error{ErrorCode::InvalidArgument,
            "I2C read needs a 7-bit address and nonempty buffer"});
    if (auto r = impl_->ready(); !r) return r;
    return impl_->finish(impl_->readPhase(address, data));
}

Result<void> FtdiI2c::writeRead(
    uint8_t address, std::span<const uint8_t> tx, std::span<uint8_t> rx
) {
    if (address > 0x7F || rx.empty())
        return std::unexpected(Error{ErrorCode::InvalidArgument,
            "I2C writeRead needs a 7-bit address and nonempty read buffer"});
    if (auto r = impl_->ready(); !r) return r;
    auto result = impl_->writePhase(address, tx);
    if (result) result = impl_->readPhase(address, rx);
    return impl_->finish(result);
}

} // namespace mstar::ftdi
