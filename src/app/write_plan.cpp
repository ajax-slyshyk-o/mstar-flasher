#include "write_plan.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <string_view>
#include <fmt/format.h>

namespace mstar::app {
namespace {
auto invalid(std::string message) {
    return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(message)});
}
auto ioError(std::string message) {
    return std::unexpected(Error{ErrorCode::Io, std::move(message)});
}
std::string_view trim(std::string_view s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == s.npos) return {};
    return s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
}
}

Result<std::vector<WriteImage>> loadWriteImages(const std::filesystem::path& image,
    const std::filesystem::path& config, uint64_t offset, uint64_t maximum) {
    std::vector<WriteImage> images;
    if (config.empty()) {
        images.push_back({image, offset, 0, {}});
    } else {
        std::ifstream input(config, std::ios::binary);
        if (!input) return ioError(fmt::format("Cannot open config: {}", config.string()));
        std::string line;
        size_t lineNumber = 0;
        while (std::getline(input, line)) {
            ++lineNumber;
            std::string_view text = line;
            if (lineNumber == 1 && text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
            text = trim(text);
            if (text.empty() || text.front() == '#' || text.front() == ';') continue;
            auto syntaxError = [&] {
                return invalid(fmt::format("{}:{}: expected filename : offset (decimal or 0x hex)",
                                           config.string(), lineNumber));
            };
            const auto colon = text.rfind(':');
            if (colon == text.npos) return syntaxError();
            auto filename = trim(text.substr(0, colon));
            auto number = trim(text.substr(colon + 1));
            if (filename.size() >= 2 && filename.front() == '"' && filename.back() == '"')
                filename = filename.substr(1, filename.size() - 2);
            if (filename.empty() || filename.find('\0') != filename.npos || number.empty()) return syntaxError();
            int base = 10;
            if (number.starts_with("0x") || number.starts_with("0X")) {
                number.remove_prefix(2);
                base = 16;
            }
            uint64_t address = 0;
            const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), address, base);
            if (number.empty() || error != std::errc{} || end != number.data() + number.size()) return syntaxError();
            std::string portablePath(filename);
            std::replace(portablePath.begin(), portablePath.end(), '\\', '/');
            const auto path = (config.parent_path() / portablePath).lexically_normal();
            images.push_back({path, address, 0, {}});
        }
        if (input.bad()) return ioError(fmt::format("Failed to read config: {}", config.string()));
        if (images.empty()) return invalid("Config contains no images");
    }
    uint64_t total = 0;
    for (auto& entry : images) {
        std::ifstream input(entry.path, std::ios::binary | std::ios::ate);
        if (!input) return ioError(fmt::format("Cannot open image: {}", entry.path.string()));
        const auto length = input.tellg();
        if (length <= 0 || static_cast<uint64_t>(length) > maximum - total)
            return invalid(fmt::format("Images must be nonempty and total no larger than a supported flash ({} bytes): {}",
                                       maximum, entry.path.string()));
        entry.sourceSize = static_cast<uint64_t>(length);
        total += entry.sourceSize;
        entry.bytes.resize(static_cast<size_t>(length));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(entry.bytes.data()), static_cast<std::streamsize>(entry.bytes.size()));
        if (!input) return ioError(fmt::format("Failed to read complete input image: {}", entry.path.string()));
    }
    return images;
}

Result<void> prepareWriteImages(std::vector<WriteImage>& images, uint64_t capacity,
    uint64_t alignment, bool configMode, bool writing) {
    std::sort(images.begin(), images.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    uint64_t previousEnd = 0;
    for (const auto& entry : images) {
        if (entry.offset > capacity || entry.bytes.size() > capacity - entry.offset)
            return invalid(fmt::format("Image range exceeds flash capacity: {}", entry.path.string()));
        if (entry.offset % alignment || (!configMode && entry.bytes.size() % alignment))
            return invalid(fmt::format("{} requires offset{} aligned to {} bytes: {}",
                writing ? "Write" : "Verify", configMode ? "" : " and image size", alignment, entry.path.string()));
        const auto padding = configMode ? (alignment - entry.bytes.size() % alignment) % alignment : 0;
        if (padding > capacity - entry.offset - entry.bytes.size())
            return invalid(fmt::format("Padded image range exceeds flash capacity: {}", entry.path.string()));
        if (entry.offset < previousEnd)
            return invalid(fmt::format("Overlapping image/erase ranges at {:#x}: {}", entry.offset, entry.path.string()));
        previousEnd = entry.offset + entry.bytes.size() + padding;
    }
    // All ranges have passed before any padding allocation or flash operation.
    if (configMode) {
        for (auto& entry : images)
            entry.bytes.resize(entry.bytes.size() + (alignment - entry.bytes.size() % alignment) % alignment, 0xFF);
    }
    return {};
}
}
