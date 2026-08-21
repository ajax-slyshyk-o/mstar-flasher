#pragma once

#include <vector>

#include "mstar/result.hpp"
#include "mstar/transport/programmer.hpp"

namespace mstar::ftdi::detail {

/// Enumerates FTDI devices visible to D2XX. Does not open or communicate
/// with any device, so it is safe to call with no programmer attached or
/// while other software holds a device open.
Result<std::vector<ProgrammerInfo>> enumerateDevices();

} // namespace mstar::ftdi::detail
