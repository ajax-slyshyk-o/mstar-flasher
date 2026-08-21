// FT2232D device enumeration via D2XX (Milestone 1).

#include "ftdi_device.hpp"

#include <cstring>
#include <string>
#include <string_view>

#include <ftd2xx.h>

namespace mstar::ftdi::detail {

namespace {

std::string_view deviceTypeName(FT_DEVICE type) {
    switch (type) {
        case FT_DEVICE_BM: return "FT232BM";
        case FT_DEVICE_AM: return "FT8U232AM";
        case FT_DEVICE_100AX: return "FT100AX";
        case FT_DEVICE_2232C: return "FT2232C/D";
        case FT_DEVICE_232R: return "FT232R";
        case FT_DEVICE_2232H: return "FT2232H";
        case FT_DEVICE_4232H: return "FT4232H";
        case FT_DEVICE_232H: return "FT232H";
        case FT_DEVICE_X_SERIES: return "FT-X";
        default: return "unknown FTDI device";
    }
}

} // namespace

std::string channelFromSerial(std::string_view serial) {
    if (serial.empty()) {
        return {};
    }
    char last = serial.back();
    if (last >= 'A' && last <= 'D') {
        return std::string(1, last);
    }
    return {};
}

std::string boundedString(const char* data, std::size_t capacity) {
    return std::string(data, strnlen(data, capacity));
}

Result<std::vector<ProgrammerInfo>> enumerateDevices() {
    DWORD numDevices = 0;
    if (FT_STATUS status = FT_CreateDeviceInfoList(&numDevices); status != FT_OK) {
        return std::unexpected(Error{
            ErrorCode::Ftdi,
            "FT_CreateDeviceInfoList failed with status " + std::to_string(status)});
    }

    std::vector<ProgrammerInfo> devices;
    if (numDevices == 0) {
        return devices;
    }

    std::vector<FT_DEVICE_LIST_INFO_NODE> nodes(numDevices);
    if (FT_STATUS status = FT_GetDeviceInfoList(nodes.data(), &numDevices); status != FT_OK) {
        return std::unexpected(Error{
            ErrorCode::Ftdi,
            "FT_GetDeviceInfoList failed with status " + std::to_string(status)});
    }

    devices.reserve(numDevices);
    for (DWORD i = 0; i < numDevices; ++i) {
        const auto& node = nodes[i];
        std::string serial = boundedString(node.SerialNumber, sizeof(node.SerialNumber));

        devices.push_back(ProgrammerInfo{
            .kind = std::string(deviceTypeName(static_cast<FT_DEVICE>(node.Type))),
            .description = boundedString(node.Description, sizeof(node.Description)),
            .serial = serial,
            .location = channelFromSerial(serial),
            .inUse = (node.Flags & FT_FLAGS_OPENED) != 0,
        });
    }

    return devices;
}

} // namespace mstar::ftdi::detail
