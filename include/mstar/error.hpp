#pragma once

#include <string>

namespace mstar {

enum class ErrorCode {
    Usb,
    Ftdi,
    DeviceNotFound,
    DeviceBusy,

    I2cNack,
    I2cTransfer,
    Timeout,

    IspActivationFailed,
    IspProtocol,

    FlashUnknown,
    FlashProtocol,
    EccUncorrectable,
    BadBlock,

    VerifyMismatch,
    InvalidArgument,
    Io,
};

struct Error {
    ErrorCode code;
    std::string message;
};

} // namespace mstar
