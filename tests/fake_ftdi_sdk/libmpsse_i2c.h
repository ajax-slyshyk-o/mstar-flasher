#pragma once
#include "ftd2xx.h"

// Test-only declarations corresponding to the vendored LibMPSSE 1.0.9 header.
constexpr DWORD I2C_TRANSFER_OPTIONS_START_BIT = 0x01;
constexpr DWORD I2C_TRANSFER_OPTIONS_STOP_BIT = 0x02;
constexpr DWORD I2C_TRANSFER_OPTIONS_BREAK_ON_NACK = 0x04;
constexpr DWORD I2C_TRANSFER_OPTIONS_NACK_LAST_BYTE = 0x08;
constexpr DWORD I2C_DISABLE_3PHASE_CLOCKING = 0x01;

enum I2C_CLOCKRATE {
    I2C_CLOCK_STANDARD_MODE = 100000,
    I2C_CLOCK_FAST_MODE = 400000,
    I2C_CLOCK_FAST_MODE_PLUS = 1000000,
    I2C_CLOCK_HIGH_SPEED_MODE = 3400000,
};
struct ChannelConfig {
    I2C_CLOCKRATE ClockRate;
    UCHAR LatencyTimer;
    DWORD Options;
    DWORD Pin;
    USHORT currentPinState;
};

extern "C" {
FT_STATUS I2C_GetNumChannels(DWORD* count);
FT_STATUS I2C_GetChannelInfo(DWORD index, FT_DEVICE_LIST_INFO_NODE* info);
FT_STATUS I2C_OpenChannel(DWORD index, FT_HANDLE* handle);
FT_STATUS I2C_InitChannel(FT_HANDLE handle, ChannelConfig* config);
FT_STATUS I2C_CloseChannel(FT_HANDLE handle);
FT_STATUS I2C_DeviceWrite(FT_HANDLE handle, UCHAR address, DWORD size, UCHAR* buffer,
                          LPDWORD transferred, DWORD options);
FT_STATUS I2C_DeviceRead(FT_HANDLE handle, UCHAR address, DWORD size, UCHAR* buffer,
                         LPDWORD transferred, DWORD options);
}
