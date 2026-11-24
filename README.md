# mstar-flasher

Cross-platform command-line utility for in-system programming of SPI-NAND
and SPI-NOR flash attached to MStar/SigmaStar SoCs, using the original
FT2232D-based ISP programmer as the first supported hardware backend.

See [doc/BLUEPRINT.md](doc/BLUEPRINT.md) for the full design and milestone
plan.

## Status

Hardware-independent `mstar-core` library with ISP/SPI-NAND logic and offline
unit tests. The FTDI backend uses D2XX + LibMPSSE on Windows and libftdi1/libusb
on Linux. Offline tests pass and Linux USB access has been checked on an original
FT2232C/D programmer. Target identification is still unreliable on the connected
board; electrical timing and complete dumps remain unverified. See the
[hardware acceptance record](doc/HARDWARE.md) for results and remaining checks.

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

On Linux, check build tools, USB libraries, permissions and attached programmers:

```console
./scripts/setup-linux.sh --check
```

To install missing prerequisites and configure USB access:

```console
sudo ./scripts/setup-linux.sh --with-build-tools
```

The script shows a plan and asks before changing anything. It installs missing
`libftdi1-dev`, `libusb-1.0-0-dev` and `pkg-config` packages on Ubuntu/Debian;
`--with-build-tools` also installs missing build utilities. It reports compiler
versions and `VCPKG_ROOT`, but does not install vcpkg or upgrade an old compiler.
Run `--help` for noninteractive and skip options.

USB setup writes `/etc/udev/rules.d/60-mstar-ftdi.rules` for FTDI product IDs
`6010`, `6011` and `6014`, grants `plugdev`/desktop-session access and tells
ModemManager to ignore those devices. It leaves the kernel UART driver enabled.
Existing rules are backed up before replacement; repeat runs leave matching
rules alone. Re-plug the programmer afterward, and log out/in if group membership
changed. Run `--check` again as your normal user. Check mode exits 1 when USB
prerequisites remain missing; build-tool checks are informational.

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

List connected flashers:

```console
./build/linux/mstar-cli list
```

This prints each flasher's type, description, serial and channel, plus its USB
bus/address on Linux (or COM port on Windows when available). On Linux, devices
with restricted USB permissions still appear with their type and USB location;
a note explains why their serial/description could not be read. Listing does
not enter ISP mode or communicate with the target flash.

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

## Tracing I2C transactions

`probe`, `read`, and `i2c-test` accept `--trace-i2c`:

```console
./build/linux/mstar-cli probe --serial ggggggggA --trace-i2c 2>probe-i2c.log
./build/linux/mstar-cli i2c-test --serial ggggggggA --trace-i2c
```

The trace goes to stderr and records the requested clock, seven-bit address,
transmitted/received bytes and transport errors. `W` means write, `R` read, and
`WR` a combined write/read with repeated START. Buffers larger than 32 bytes are
summarized by length; failed reads do not print potentially stale data. Software
traces cannot verify electrical timing or prove that a reported ACK is genuine.
See [hardware acceptance](doc/HARDWARE.md) for the bench procedure.

## Reading a NAND dump

```console
./build/linux/mstar-cli read backup.bin
./build/linux/mstar-cli read region.bin --offset 0x1000 --size 0x10000
```

Reads stop on the first uncorrectable ECC error by default. To continue with
`0xFF` placeholders for affected pages, explicitly request a recovery dump:

```console
./build/linux/mstar-cli read recovery.bin --fill-on-ecc-error
```

| Exit code | Meaning |
|---|---|
| `0` | The requested range was read and the output file was written successfully. |
| `1` | The operation failed (read, range, open, write, flush or close error). |
| `2` | Recovery completed, but some pages were replaced with `0xFF`; data was lost. |

Each replaced page is reported on stderr. On failure, any partial output is
retained with an incomplete-file diagnostic; it must not be treated as a complete
backup. Existing output files are overwritten. Current ECC interpretation is
still limited; see the next steps in [the blueprint](doc/BLUEPRINT.md#current-progress-and-next-steps-2026-11-24).

## Linux release archive

```console
./scripts/publish-linux.sh --jobs 4
```

This builds the `linux-release` preset, runs offline tests and creates
`build/publish/mstar-flasher-linux-<architecture>.tar.gz`. Use `--output-dir DIR`
to choose another destination. The archive contains `mstar-cli`, this README,
and `scripts/setup-linux.sh`; after extracting, run `./mstar-cli list` from its
top-level directory. Compatible system runtime libraries (including libstdc++,
libftdi1 and libusb) are required. The script does not upload or install anything.
