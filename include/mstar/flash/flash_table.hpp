#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "mstar/flash/flash_part.hpp"

namespace mstar {

/// Built-in table of known SPI-NAND parts. Will eventually be
/// supplemented by parts loaded from a user-supplied file; see the
/// project notes on serdepp/toml11/nlohmann-json for that future work.
const std::vector<FlashPart>& builtinFlashTable();

/// Matches `id` against `table` by comparing each entry's declared
/// idLength leading bytes. Returns the first match, or nullopt if none
/// of the table's entries match.
std::optional<FlashPart> lookupFlashPart(
    std::span<const uint8_t> id,
    const std::vector<FlashPart>& table = builtinFlashTable()
);

} // namespace mstar
