#pragma once

// Test-only subset of D2XX declarations used by ftdi_i2c.cpp. These headers
// belong only to mstar-d2xx-tests; production uses FTDI's actual SDK.
#include <cstdint>

using DWORD = std::uint32_t;
using UCHAR = unsigned char;
using USHORT = std::uint16_t;
using LPDWORD = DWORD*;
using FT_HANDLE = void*;
using FT_STATUS = std::uint32_t;

constexpr FT_STATUS FT_OK = 0;
constexpr FT_STATUS FT_INVALID_HANDLE = 1;
constexpr FT_STATUS FT_IO_ERROR = 4;

struct FT_DEVICE_LIST_INFO_NODE {
    DWORD Flags;
    DWORD Type;
    DWORD ID;
    DWORD LocId;
    char SerialNumber[16];
    char Description[64];
    FT_HANDLE ftHandle;
};

extern "C" {
FT_STATUS FT_Write(FT_HANDLE handle, void* buffer, DWORD size, LPDWORD transferred);
FT_STATUS FT_Read(FT_HANDLE handle, void* buffer, DWORD size, LPDWORD transferred);
FT_STATUS FT_SetTimeouts(FT_HANDLE handle, DWORD readTimeout, DWORD writeTimeout);
FT_STATUS FT_SetBitMode(FT_HANDLE handle, UCHAR mask, UCHAR mode);
}
