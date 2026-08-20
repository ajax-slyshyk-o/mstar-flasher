#pragma once

#include <optional>
#include <string>

namespace mstar {

/// Hardware-independent description of an enumerated programmer device.
struct ProgrammerInfo {
    std::string kind;         // e.g. "FT2232D"
    std::string description;
    std::string serial;
    std::string location;     // e.g. MPSSE channel
};

/// Criteria for selecting a specific programmer among several enumerated
/// devices. Do not assume index 0 is the desired device.
struct ProgrammerSelector {
    std::optional<std::string> serial;
    std::optional<std::string> description;
    std::optional<std::string> location;
};

} // namespace mstar
