#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>
#include "mstar/result.hpp"

namespace mstar::app {
struct WriteImage {
    std::filesystem::path path;
    uint64_t offset = 0;
    uint64_t sourceSize = 0;
    std::vector<uint8_t> bytes;
};

// Load every input before opening the programmer. Paths in a vendor config
// are relative to the config directory, not the process working directory.
Result<std::vector<WriteImage>> loadWriteImages(const std::filesystem::path& image,
    const std::filesystem::path& config, uint64_t offset, uint64_t maximum);
// Validate the entire plan before reads or mutations. Config images are padded
// with FF to complete erase blocks; ordinary image alignment stays strict.
Result<void> prepareWriteImages(std::vector<WriteImage>& images, uint64_t capacity,
    uint64_t alignment, bool configMode, bool writing);
}
