# mstar-flasher

Cross-platform command-line utility for in-system programming of SPI-NAND
and SPI-NOR flash attached to MStar/SigmaStar SoCs, using the original
FT2232D-based ISP programmer as the first supported hardware backend.

See [doc/BLUEPRINT.md](doc/BLUEPRINT.md) for the full design and milestone
plan.

## Status

Hardware-independent `mstar-core` library with ISP/SPI-NAND logic and offline
unit tests. On Windows, LibMPSSE initializes the FTDI channel and bounded D2XX
batches carry data while checking every address/data ACK. Linux uses libftdi1/libusb.
The Windows transport has passed matching 8 KiB reads and ECC-checked verification
on a W25N02KV, plus an unused-address NACK check. Full dumps, physical programming,
and logic-analyzer acceptance remain pending. See the
[hardware acceptance record](doc/HARDWARE.md) and
[transport performance investigation](doc/VENDOR_PERFORMANCE.md).

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
backup. Existing output files are overwritten after identification, range checks
and ECC configuration succeed.

Reads enable and verify internal ECC and the chip's per-page read mode, then
restore settings changed by the command, including on failure and with
`--stay-in-isp`. Configuration is checked before every page. Unknown ECC profiles,
inconsistent status, and configuration/transport failures abort with exit code 1;
`--fill-on-ecc-error` only substitutes confirmed uncorrectable pages.

Corrected pages are returned normally (exit 0 if the operation succeeds). Stderr
reports corrected-bit counts or ranges and an ECC summary. The threshold count
is a subset of corrected pages: Winbond KV reports a configurable threshold;
GigaDevice and W25N01GV report correction at their maximum strength.
See [ECC profiles and sources](doc/ECC.md) for supported parts and limitations.
Raw backups and marker checks are available in the write workflow below.
General OOB image handling, bad-block remapping, and SPI-NOR reads remain pending.

## Writing and verifying a NAND image

`write` accepts a main-area image with block-aligned size and offset. It checks
bad-block markers, erases/programs the requested blocks, and verifies every
image byte through ECC-checked reads. Add `--backup PATH` to save and compare
a raw main+OOB backup before erasing.
Marked blocks abort the operation; image addresses are not shifted.

```console
./build/linux/mstar-cli write firmware.bin --serial ggggggggA --dry-run
./build/linux/mstar-cli write firmware.bin --serial ggggggggA
./build/linux/mstar-cli write firmware.bin --serial ggggggggA --backup before-write.raw
./build/linux/mstar-cli verify firmware.bin --serial ggggggggA
```

`--serial` is optional for `write`, including `--dry-run`: when omitted, the
first detected programmer is selected. Supply it to select a specific device.
`write` programs by default; `--dry-run` checks the plan and markers without
programming. `--yes` has been removed. Backup is optional for both modes;
when supplied, `--backup PATH` requires a new path and also creates `PATH.json`.
To write several images, use the vendor `filename : offset` config format:

```console
./build/linux/mstar-cli write --config OnebinnandBurnImgConfig.cfg
```

Image paths are relative to the config directory. Config writes fill the rest
of each image's last block with `0xFF`; add `--preserve-gaps` to retain those
existing main-area bytes. Whole blocks between images stay untouched. Preserved
tails are read before any erase, including when backup is omitted. With
`--backup`, all affected blocks are backed up and compared before any erase.
`--dry-run` performs these preflight steps and any requested backup, then stops.

The backup includes spare/parity bytes and cannot be passed directly to `write`.
Main-area programming regenerates ECC but does not preserve user OOB metadata.
The implementation is offline-tested; no programming trial has been performed
on the connected board. See [writing and bad-block testing](doc/WRITING.md) for
the workflow, backup format and limits.

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
