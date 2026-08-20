#pragma once

#include <expected>

#include "mstar/error.hpp"

namespace mstar {

template <class T>
using Result = std::expected<T, Error>;

} // namespace mstar
