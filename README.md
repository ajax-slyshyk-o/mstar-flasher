# mstar-flasher

Cross-platform command-line utility for in-system programming of SPI-NAND
and SPI-NOR flash attached to MStar/SigmaStar SoCs, using the original
FT2232D-based ISP programmer as the first supported hardware backend.

See [doc/BLUEPRINT.md](doc/BLUEPRINT.md) for the full design and milestone
plan.

## Status

Milestone 0 (project skeleton): hardware-independent `mstar-core` library
(`Result`/`Error`, `I2cMaster`, `SpiBus`, `MstarIsp`), CLI skeleton, and
unit tests covering the MStar ISP protocol sequence against a
`MockI2cMaster`. No FTDI/D2XX/LibMPSSE calls yet.

## Building

Requires [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set,
and CMake >= 3.25.

The `windows` preset uses Ninja + MSVC, so it must be run from a
**Developer PowerShell/Command Prompt for Visual Studio** (plain
PowerShell/cmd will not find `cl.exe`):

```console
cmake --preset windows
cmake --build --preset windows
ctest --preset windows
```

On Linux:

```console
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
```

By default `MSTAR_ENABLE_FTDI` is `OFF`, so the core library, CLI and
tests build without the FTDI SDK. Enable it once the FTDI backend is
implemented:

```console
cmake --preset windows -DMSTAR_ENABLE_FTDI=ON \
  -DFTD2XX_ROOT=C:/SDK/FTDI/D2XX \
  -DLIBMPSSE_ROOT=C:/SDK/FTDI/LibMPSSE
```
