# mstar-flasher

Cross-platform command-line utility for in-system programming of SPI-NAND
and SPI-NOR flash attached to MStar/SigmaStar SoCs, using the original
FT2232D-based ISP programmer as the first supported hardware backend.

See [doc/BLUEPRINT.md](doc/BLUEPRINT.md) for the full design and milestone
plan.

## Status

Hardware-independent `mstar-core` library with ISP/SPI-NAND logic and offline
unit tests. The FTDI backend uses D2XX + LibMPSSE on Windows and libftdi1/libusb
on Linux. Linux builds and transfer sequences are tested without hardware;
electrical timing and real programmer operation still need bench validation.

## Building

Requires [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set,
Ninja, CMake >= 3.28, and a C++23 compiler (`std::expected` support;
GCC 13 works on Linux).

The `windows` preset uses Ninja + MSVC, so it must be run from a
**Developer PowerShell/Command Prompt for Visual Studio** (plain
PowerShell/cmd will not find `cl.exe`):

```console
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
```

On Linux, install the system USB development packages (Ubuntu/Debian):

```console
sudo apt install pkg-config libftdi1-dev libusb-1.0-0-dev
```

The `linux` profile enables FTDI and uses libftdi1 >= 1.5. It does not use
the Windows SDK archives. C++ dependencies still come from vcpkg.

```console
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
```

The executable is `build/linux/mstar-cli`. Linux enumeration lists MPSSE
channels: A on FT2232C/D and FT232H, A/B on FT2232H and FT4232H, using standard
FTDI USB IDs. Multi-channel serials include the channel suffix (for example,
`MSTARA`); use the serial shown by `list` with `--serial`. USB access requires
appropriate device permissions/udev rules. The backend detaches only the
selected interface's kernel driver and requests reattachment on close.

`MSTAR_ENABLE_FTDI` defaults to `ON`. For an offline build without USB SDKs:

```console
cmake --preset linux -B build/linux-no-ftdi -DMSTAR_ENABLE_FTDI=OFF
cmake --build build/linux-no-ftdi
ctest --test-dir build/linux-no-ftdi --output-on-failure
```

Windows uses the SDK archives in `archives/`, or explicit SDK paths:

```console
cmake --preset windows -DMSTAR_ENABLE_FTDI=ON \
  -DFTD2XX_ROOT=C:/SDK/FTDI/D2XX \
  -DLIBMPSSE_ROOT=C:/SDK/FTDI/LibMPSSE
```
