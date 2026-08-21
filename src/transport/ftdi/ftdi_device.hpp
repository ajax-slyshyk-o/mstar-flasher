#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "mstar/result.hpp"
#include "mstar/transport/programmer.hpp"

namespace mstar::ftdi::detail {

/// Enumerates FTDI devices visible to D2XX. Does not open or communicate
/// with any device, so it is safe to call with no programmer attached or
/// while other software holds a device open.
Result<std::vector<ProgrammerInfo>> enumerateDevices();

/// FTDI appends a channel letter (A, B, ...) to the serial number of
/// multi-port devices such as the FT2232D (documented in ftd2xx.h's EEPROM
/// programming notes), so it doubles as a channel identifier here. Empty if
/// none can be determined.
std::string channelFromSerial(std::string_view serial);

/// SerialNumber/Description fields returned by D2XX/LibMPSSE are fixed-size
/// buffers that are not guaranteed to be NUL-terminated at capacity.
std::string boundedString(const char* data, std::size_t capacity);

} // namespace mstar::ftdi::detail
