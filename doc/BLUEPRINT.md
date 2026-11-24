# MStar / SigmaStar Cross-Platform In-System Flasher

> Linux implementation update: the `linux` preset uses system libftdi1 >= 1.5
> and libusb with a native MPSSE backend, following the TEA2017 Linux transport
> approach. It keeps FT2232D two-phase clocking and the LibMPSSE GPIO sequences;
> Windows still uses D2XX + LibMPSSE. The D2XX/LibMPSSE stack below describes
> the original plan. Linux has offline backend tests; bench validation is pending.

## 1. Project goal

Create a cross-platform command-line utility for **in-system programming of SPI-NAND and SPI-NOR attached to MStar/SigmaStar SoCs**, without desoldering the flash.

The first supported programmer is the original MStar/SigmaStar programmer based on **FTDI FT2232D**.

Initial supported host platforms:

- Windows 10/11 x64 with MSVC.
- Linux x86-64 with GCC or Clang.
- Keep the architecture portable enough for macOS, but Windows and Linux are the first-class targets.

Initial programmer stack:

```text
PC
 │
 │ D2XX + LibMPSSE-I2C
 ▼
FT2232D channel A
 │
 │ I2C
 ▼
MStar / SigmaStar ISP slave
 │
 │ internal SPI bridge
 ▼
SPI-NAND / SPI-NOR
```

The FT2232D is explicitly supported by FTDI's MPSSE and LibMPSSE-I2C stack. FTDI documents one MPSSE-capable channel on FT2232D, suitable for the I²C ISP path. citeturn774050search0turn843508view0

Later:

```text
                       I2cMaster
                          │
              ┌───────────┴───────────┐
              │                       │
        FtdiMpsseI2c              Ch341I2c
              │                       │
     D2XX + LibMPSSE               libusb
              │                       │
           FT2232D                  CH341A
```

Nothing above `I2cMaster` should depend on the programmer hardware.

---

## 2. Important non-goals for the first milestone

Do **not** initially implement:

- flash erase;
- flash programming;
- bad-block relocation;
- raw OOB writing;
- UART console;
- CH341 support;
- GUI.

The first hardware milestone is intentionally read-only:

```console
mstar-flasher list
mstar-flasher probe
```

Expected output should eventually resemble:

```text
Programmer:
  FTDI device: FT2232D
  Serial: XXXXXXXX
  MPSSE channel: A
  I2C clock: 100000 Hz

Target:
  MStar ISP address: 0x49
  ISP activation: OK

Flash:
  JEDEC ID: EF AA 22
  Type: SPI-NAND
  Model: ...
  Capacity: ...
```

`probe` must never erase or program anything.

---

## 3. Language and build system

Use:

```text
C++23
CMake
CMakePresets.json
```

Prefer standard C++ facilities:

- `std::span`
- `std::expected`
- `std::filesystem`
- RAII
- `std::chrono`
- `std::string_view`

Avoid platform-specific code outside hardware backend directories.

Recommended third-party C++ dependencies:

- **CLI11** for command-line parsing.
- **Catch2** for tests.

CLI11 is cross-platform and dependency-light; Catch2 integrates cleanly with CMake. citeturn479883search1turn479883search0

Do not require the FTDI SDK to build the core library or unit tests.

---

## 4. Repository structure

Initialize the repository approximately as:

```text
mstar-flasher/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── LICENSE
│
├── cmake/
│   ├── FindFTD2XX.cmake
│   └── FindLibMPSSE.cmake
│
├── include/
│   └── mstar/
│       ├── error.hpp
│       ├── result.hpp
│       │
│       ├── transport/
│       │   ├── i2c_master.hpp
│       │   ├── console.hpp
│       │   └── programmer.hpp
│       │
│       ├── isp/
│       │   └── mstar_isp.hpp
│       │
│       └── flash/
│           ├── spi_bus.hpp
│           ├── spi_flash.hpp
│           ├── spi_nand.hpp
│           ├── spi_nor.hpp
│           ├── nand_geometry.hpp
│           └── flash_part.hpp
│
├── src/
│   ├── app/
│   │   └── main.cpp
│   │
│   ├── transport/
│   │   └── ftdi/
│   │       ├── ftdi_device.cpp
│   │       ├── ftdi_i2c.cpp
│   │       └── ftdi_console.cpp
│   │
│   ├── isp/
│   │   └── mstar_isp.cpp
│   │
│   └── flash/
│       ├── spi_nand.cpp
│       ├── spi_nor.cpp
│       └── flash_database.cpp
│
├── tests/
│   ├── mock_i2c.hpp
│   ├── test_mstar_isp.cpp
│   ├── test_spi_nand.cpp
│   └── test_flash_database.cpp
│
└── docs/
    ├── BLUEPRINT.md
    ├── MSTAR_ISP.md
    └── HARDWARE.md
```

Later add:

```text
src/transport/ch341/
```

without changing `isp/` or `flash/`.

---

## 5. Core abstraction: I2cMaster

The MStar ISP implementation must depend only on a generic I²C master.

Start with:

```cpp
class I2cMaster {
public:
    virtual ~I2cMaster() = default;

    virtual Result<void> setClock(uint32_t hz) = 0;

    virtual Result<void> write(
        uint8_t address,
        std::span<const uint8_t> data
    ) = 0;

    virtual Result<void> read(
        uint8_t address,
        std::span<uint8_t> data
    ) = 0;

    virtual Result<void> writeRead(
        uint8_t address,
        std::span<const uint8_t> tx,
        std::span<uint8_t> rx
    ) = 0;
};
```

`writeRead()` means:

```text
START
address + W
tx...
REPEATED START
address + R
rx...
STOP
```

This semantic distinction is important for the MStar protocol.

LibMPSSE supports independently controlling START and STOP conditions, so `writeRead()` can be implemented as a write with START but no STOP followed by a read with START + STOP. citeturn175578view0

Do not leak `FT_HANDLE`, `FT_STATUS`, LibMPSSE constants or libusb types through this interface.

---

## 6. FT2232D backend

Use:

```text
FTDI D2XX
+
FTDI LibMPSSE-I2C
```

rather than raw MPSSE for version 1.

FTDI currently recommends LibMPSSE-I2C for MPSSE-based I²C and provides libraries, examples and source. citeturn774050search0turn774050search3

The backend should implement:

```cpp
class FtdiI2c final : public I2cMaster {
public:
    static Result<std::vector<FtdiDeviceInfo>> enumerate();

    static Result<FtdiI2c> open(
        const FtdiDeviceSelector& selector
    );

    Result<void> setClock(uint32_t hz) override;
    Result<void> write(...) override;
    Result<void> read(...) override;
    Result<void> writeRead(...) override;
};
```

Use LibMPSSE functions such as:

```text
I2C_GetNumChannels
I2C_GetChannelInfo
I2C_OpenChannel
I2C_InitChannel
I2C_DeviceWrite
I2C_DeviceRead
I2C_CloseChannel
```

The FTDI guide confirms FT2232D contributes one I²C-capable MPSSE channel. citeturn843508view0

### Device selection

Do not assume programmer index `0`.

Support selecting by:

```text
serial number
description
location/channel
```

The CLI should eventually support:

```console
mstar-flasher list

mstar-flasher probe --serial XXXXXXXX
```

If only one compatible programmer exists, automatic selection is acceptable.

### Default I²C clock

Start conservatively at:

```text
100 kHz
```

Make it configurable:

```console
--i2c-clock 100000
```

Do not optimize speed until communication is proven reliable.

---

## 7. MStar ISP protocol layer

Create:

```cpp
class MstarIsp final : public SpiBus {
public:
    explicit MstarIsp(
        I2cMaster& i2c,
        uint8_t ispAddress = 0x49
    );

    Result<void> enter();
    Result<void> leave();

    Result<void> transaction(
        std::span<const uint8_t> tx,
        std::span<uint8_t> rx
    ) override;
};
```

Default ISP address:

```text
0x49
```

Keep it configurable because some MStar variants use different addresses.

The MStar/SigmaStar ISP mechanism exposes the SoC's SPI bus over an I²C slave. The known protocol uses the activation string `MSTAR` and command bytes `0x10`, `0x11`, `0x12`, etc. The `snander_electricboogaloo` project is the main behavioral reference for this layer. citeturn884236view0

### Enter ISP

Send in **one I²C transaction**:

```text
"MSTAR"

4D 53 54 41 52
```

to the ISP slave.

Do not split this string into multiple I²C writes.

### SPI write

For SPI data:

```text
I2C payload:

10 <SPI bytes...>
```

where:

```text
0x10 = MSTARDDC_SPI_WRITE
```

### SPI read

To receive SPI data:

```text
write:
11

repeated START

read:
<data...>
```

where:

```text
0x11 = MSTARDDC_SPI_READ
```

### End SPI transaction

Send:

```text
12
```

which releases the emulated SPI chip-select.

### Other known commands

Keep constants for:

```text
0x10  SPI_WRITE
0x11  SPI_READ
0x12  SPI_END
0x20  STATUS
0x24  RESET / EXIT
```

Do not initially depend on `STATUS` for basic operation unless hardware testing demonstrates it is required.

### Critical SPI transaction rule

`0x12` must only be sent when the complete SPI transaction should end.

For example, reading JEDEC ID should conceptually be:

```text
I2C WRITE:
    10 9F

I2C WRITE + repeated-start READ:
    11
    <ID bytes>

I2C WRITE:
    12
```

Do not release CS between `0x9F` and reading the ID.

---

## 8. SpiBus abstraction

The flash code must not know that SPI is transported through MStar ISP.

Use:

```cpp
class SpiBus {
public:
    virtual ~SpiBus() = default;

    virtual Result<void> transaction(
        std::span<const uint8_t> tx,
        std::span<uint8_t> rx
    ) = 0;
};
```

Therefore:

```text
SPI-NAND
   │
   ▼
SpiBus
   │
   ▼
MstarIsp
   │
   ▼
I2cMaster
   │
   ▼
FT2232D
```

Later another SPI transport could theoretically be added without rewriting NAND support.

---

## 9. SPI-NAND layer

Do not immediately copy the complete SNANDer NAND implementation.

Start with a small clean implementation.

First operations:

```text
reset
read JEDEC ID
GET FEATURE
SET FEATURE
PAGE READ
READ FROM CACHE
read status
```

Only after reliable reads:

```text
WRITE ENABLE
PROGRAM LOAD
PROGRAM EXECUTE
BLOCK ERASE
```

Define basic data structures:

```cpp
struct NandGeometry {
    uint32_t pageSize;
    uint32_t oobSize;
    uint32_t pagesPerBlock;
    uint32_t blocksPerLun;
    uint32_t luns;
};

struct FlashPart {
    std::string_view vendor;
    std::string_view model;

    std::array<uint8_t, 4> id;
    size_t idLength;

    NandGeometry geometry;

    uint32_t eccStepSize;
    uint32_t eccStrength;
};
```

Separate geometry, vendor-specific ECC interpretation and flash operations.

### ECC

Default mode for version 1:

```text
on-die ECC enabled
```

Do not initially attempt host-side ECC generation.

The software must eventually distinguish:

```text
ECC OK
ECC corrected
ECC threshold reached
ECC uncorrectable
```

because this is essential for reliable NAND dumping and verification.

### OOB

Support OOB/raw mode only after ordinary page reads are stable.

Eventually expose something similar to:

```console
--raw
--include-oob
--ecc on
--ecc off
```

but these are later milestones.

---

## 10. Flash database

Do not hard-code flash logic into `spi_nand.cpp`.

Use a table/database layer:

```text
JEDEC ID
  ↓
FlashPart
  ↓
geometry + ECC/vendor callbacks
```

Useful references for part definitions and NAND behavior:

- SNANDer.
- `snander_electricboogaloo`.
- `ufprog`.

`ufprog` is particularly useful as a modern reference because it has explicit abstractions for controllers, interfaces, SPI-NAND, ECC and vendor-specific part tables. citeturn479883view0

Treat these repositories as **reference implementations**, not as code to paste blindly.

---

## 11. Error handling

Use `std::expected`.

Example:

```cpp
enum class ErrorCode {
    Usb,
    Ftdi,
    DeviceNotFound,
    DeviceBusy,

    I2cNack,
    I2cTransfer,
    Timeout,

    IspActivationFailed,
    IspProtocol,

    FlashUnknown,
    FlashProtocol,
    EccUncorrectable,
    BadBlock,

    VerifyMismatch,
    InvalidArgument,
    Io
};

struct Error {
    ErrorCode code;
    std::string message;
};

template<class T>
using Result = std::expected<T, Error>;
```

Translate FTDI-specific errors inside the FTDI backend.

No `FT_STATUS` should escape into the MStar or NAND layers.

---

## 12. Logging and protocol tracing

Implement two levels:

```text
normal logging
protocol trace
```

Example:

```console
mstar-flasher probe --verbose
mstar-flasher probe --trace-i2c
```

Trace format should make debugging with a logic analyzer easy:

```text
I2C W 0x49 : 4D 53 54 41 52
I2C W 0x49 : 10 9F
I2C WR 0x49: 11 -> EF AA 22
I2C W 0x49 : 12
```

Do not log giant page buffers by default.

For large transfers log:

```text
I2C W 0x49 : 2113 bytes
```

with an optional explicit hex-dump mode.

---

## 13. CLI design

Initial CLI:

```console
mstar-flasher list

mstar-flasher probe
mstar-flasher probe --serial SERIAL
mstar-flasher probe --i2c-address 0x49
mstar-flasher probe --i2c-clock 100000
```

Next milestone:

```console
mstar-flasher read backup.bin
```

Later:

```console
mstar-flasher verify firmware.bin

mstar-flasher write firmware.bin
mstar-flasher erase
```

Destructive commands must require explicit confirmation:

```console
mstar-flasher write firmware.bin --yes
```

or interactive confirmation.

Never make `probe` destructive.

---

## 14. Testing strategy

This is important: **the MStar ISP protocol should be testable without hardware**.

Create:

```cpp
class MockI2cMaster final : public I2cMaster
```

which records transactions.

A unit test for JEDEC ID should verify the exact sequence:

```text
write 0x49:
    "MSTAR"

write 0x49:
    10 9F

writeRead 0x49:
    tx = 11
    rx = 3 bytes

write 0x49:
    12
```

Also test:

```text
NACK during MSTAR
short I2C transfer
unknown flash ID
timeout
unexpected status
ECC uncorrectable
verify mismatch
```

CI must run without an FTDI programmer attached.

Hardware tests are a separate manual/integration test suite.

---

## 15. Hardware validation strategy

Before implementing flash writes:

### Test A — FTDI enumeration

Verify:

```console
mstar-flasher list
```

finds the original FT2232D programmer.

### Test B — I²C electrical activity

Run:

```console
mstar-flasher i2c-test
```

and inspect SDA/SCL with a logic analyzer.

Verify:

```text
START
7-bit address 0x49
ACK
STOP
```

### Test C — MSTAR activation

Send:

```text
MSTAR
```

and verify ACKs.

### Test D — SPI JEDEC ID

Send SPI `0x9F` through MStar ISP.

Compare the detected ID with the actual flash mounted on the board.

### Test E — repeated reads

Read the same region multiple times and ensure identical results.

Only after these pass should erase/program commands be enabled.

---

## 16. FT2232D UART console — later milestone

Keep UART separate from `I2cMaster`.

Use:

```cpp
class Console {
public:
    virtual ~Console() = default;

    virtual Result<void> setBaudRate(uint32_t baud) = 0;
    virtual Result<size_t> read(std::span<uint8_t>) = 0;
    virtual Result<size_t> write(std::span<const uint8_t>) = 0;
};
```

FT2232D channel B can later be accessed through D2XX as UART.

Expected architecture:

```text
FT2232D
 ├── Channel A → MPSSE → I2C ISP
 └── Channel B → UART console
```

Do not implement this until the original programmer's **74HC08 gating/routing** has been understood.

Document its PCB wiring in:

```text
docs/HARDWARE.md
```

---

## 17. Future CH341 backend

CH341 support must require **no changes** to:

```text
MstarIsp
SpiBus
SpiNand
SpiNor
flash database
CLI command logic
```

Only add:

```text
src/transport/ch341/
    ch341_device.cpp
    ch341_i2c.cpp
```

with:

```cpp
class Ch341I2c final : public I2cMaster
```

Use `libusb`.

SNANDer already demonstrates a cross-platform CH341 implementation and builds for Windows, Linux and macOS using libusb, so it is a useful implementation reference. citeturn591648view2turn591648view3

The final programmer selection could become:

```console
mstar-flasher probe --programmer ftdi
mstar-flasher probe --programmer ch341
```

with automatic detection as the default.

---

## 18. CMake configuration

Provide options:

```cmake
option(MSTAR_BUILD_TESTS "Build unit tests" ON)
option(MSTAR_ENABLE_FTDI "Enable FTDI programmer backend" ON)
option(MSTAR_ENABLE_CH341 "Enable CH341 backend" OFF)
```

FTDI SDK paths should be discoverable through variables such as:

```text
FTD2XX_ROOT
LIBMPSSE_ROOT
```

Example configuration:

```console
cmake -S . -B build ^
  -DFTD2XX_ROOT=C:/SDK/FTDI/D2XX ^
  -DLIBMPSSE_ROOT=C:/SDK/FTDI/LibMPSSE
```

Linux uses system libftdi1/libusb development packages instead of these SDK paths
(see README for prerequisites):

```console
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
```

Tests and the core library must build when:

```cmake
MSTAR_ENABLE_FTDI=OFF
```

This makes CI independent of proprietary/vendor SDK binaries.

---

## 19. CMake targets

Prefer separate targets:

```text
mstar-core
    Hardware-independent:
    MStar ISP
    SPI bus
    SPI NAND/NOR
    flash database

mstar-ftdi
    D2XX + LibMPSSE backend

mstar-cli
    CLI executable

mstar-tests
    Unit tests
```

Later:

```text
mstar-ch341
```

Dependency graph:

```text
mstar-cli
   │
   ├── mstar-core
   │
   └── mstar-ftdi

mstar-tests
   │
   └── mstar-core
```

`mstar-core` must never link against D2XX, LibMPSSE or libusb.

---

## 20. Licensing rule

Decide the new project's license independently.

Important:

`snander_electricboogaloo` and SNANDer are GPL-family projects. The former identifies itself as GPL-2.0, while Droid-MAX documents SNANDer as GPL v2 or later. citeturn884236view0turn591648view4

Therefore:

**Unless this new project intentionally uses a GPL-compatible license, do not copy source code from those repositories.**

Use them to understand:

```text
protocol
command sequencing
flash behavior
hardware behavior
```

and write a clean C++ implementation.

The same rule applies when consulting `ufprog`: check the license of any specific component before copying code.

---

## 21. Reference material

### Essential FTDI documentation

**FTDI D2XX drivers**

Required host driver/library for the FT2232D implementation.

[FTDI D2XX Drivers](https://ftdichip.com/drivers/d2xx-drivers/?utm_source=chatgpt.com)

**D2XX Programmer's Guide**

Reference for device enumeration, opening interfaces, UART operation and D2XX error handling.

[D2XX Programmer's Guide](https://ftdichip.com/wp-content/uploads/2025/06/D2XX_Programmers_Guide.pdf?utm_source=chatgpt.com)

**LibMPSSE-I2C downloads, examples and source**

Primary implementation library for the FT2232D I²C backend.

[FTDI LibMPSSE-I2C](https://ftdichip.com/software-examples/mpsse-projects/libmpsse-i2c-examples/?utm_source=chatgpt.com)

**AN_177 — LibMPSSE-I2C User Guide**

API and transfer-options reference.

[LibMPSSE-I2C User Guide AN_177](https://www.ftdichip.com/old2020/Support/Documents/AppNotes/AN_177_User_Guide_For_LibMPSSE-I2C.pdf)

**FT2232D product documentation**

Hardware/MPSSE reference for the original programmer.

[FTDI FT2232D product page](https://ftdichip.com/products/ft2232d/?utm_source=chatgpt.com)

---

### Essential MStar/SigmaStar references

**fifteenhex/snander_electricboogaloo**

Most important reference for the MStar DDC/ISP transport abstraction.

[snander_electricboogaloo repository](https://github.com/fifteenhex/snander_electricboogaloo?utm_source=chatgpt.com)

Pay particular attention to:

```text
src/mstarddc_spi.c
src/i2c_controller.*
src/spi_controller.*
src/spi_nand_flash.*
src/spi_nand_ids.*
```

**OpenIPC/snander-mstar**

Compact MStar + CH341-oriented reference.

[OpenIPC snander-mstar repository](https://github.com/OpenIPC/snander-mstar?utm_source=chatgpt.com)

Useful files:

```text
src/spi_controller.c
src/spi_nand_flash.c
src/ch341a_i2c.c
src/ch341a_spi.c
```

**Linux Chenxing MStar ISP reverse-engineering notes**

Important protocol and original-programmer hardware reference.

[MStar ISP / SERDB reverse-engineering notes](https://github.com/linux-chenxing/linux-chenxing.org/blob/master/isp/index.md?utm_source=chatgpt.com)

This is particularly useful for:

```text
ISP address
SERDB address
"MSTAR"
"SERDB"
0x10 / 0x11 / 0x12 commands
original FT2232D programmer hardware
```

---

### SPI-NAND references

**Droid-MAX/SNANDer**

Useful for:

```text
SPI-NAND command handling
chip database
ECC handling
bad blocks
CH341 transport
Windows/Linux/macOS build examples
```

[Droid-MAX SNANDer repository](https://github.com/Droid-MAX/SNANDer?utm_source=chatgpt.com)

**ufprog**

Useful modern architecture/reference for:

```text
SPI-NAND vendor tables
geometry
ECC abstraction
OOB layouts
ONFI
controller/interface separation
```

[ufprog repository](https://github.com/hackpascal/ufprog?utm_source=chatgpt.com)

---

### Future CH341 dependency

**libusb**

Use for the eventual CH341 backend.

[libusb repository](https://github.com/libusb/libusb?utm_source=chatgpt.com)

---

### Development dependencies

**CLI11**

[CLI11 repository](https://github.com/CLIUtils/CLI11?utm_source=chatgpt.com)

**Catch2**

[Catch2 repository](https://github.com/catchorg/Catch2?utm_source=chatgpt.com)

---

## 22. Implementation milestones

### Current progress and next steps (2026-11-24)

Implementation and hardware acceptance are tracked separately. Current user
priority: implement NAND writing now; SPI-NOR can wait. Intermittent initial
connection failures also occur with the official software, according to the
user. They remain a bench observation, not a gate on further software work.
No hardware erase/program trial has been performed.


- Milestone 0: project/core/test infrastructure implemented; Linux Debug and
  Release builds verified. Windows revalidation and automated CI remain open.
- Milestone 1: `list` detects the original FT2232C/D; USB permissions are needed
  for serial/description access and are configured on this host. Linux setup
  and release packaging are available.
- Milestones 2-3: FTDI I2C, MStar ISP and `probe` implemented, with offline
  protocol/backend tests. Linux logic-analyzer and target validation remain open.
- Milestone 4: five NAND table entries and main-area `read` with offset/size.
  SPI-NOR and evidence of two matching complete dumps remain open.
- Milestone 5: per-chip ECC configuration/decoding/reporting implemented and
  offline-tested; raw backup/marker checks are implemented for writing. General
  OOB image support and bad-block mapping remain open. Milestone 6 has an
  offline-tested write/verify workflow; milestones 7-8 remain unimplemented.

Progress and remaining work (writing prioritized by the user):

1. **Make reads report failures reliably — implemented and offline-tested.**
   Abort on uncorrectable ECC by default. `--fill-on-ecc-error` explicitly enables
   0xFF placeholders and continuation. Exit codes: 0 = clean dump, 1 = aborted
   read or output error, 2 = completed recovery dump containing substituted pages.
   Check writes, flush and close; reject overflowing ranges. Keep any partial
   output with an explicit incomplete-file diagnostic. Cover behavior using the
   real CLI with a fake programmer and failing output devices, without hardware.
   Validation: Linux Debug, Release and FTDI-disabled test suites pass. Hardware
   acceptance remains step 3; per-chip ECC is implemented in step 4.
2. **Fix error-path cleanup — implemented and offline-tested.**
   ISP transactions attempt SPI_END once after success or a transfer failure,
   without replaying the payload. The original error code/message is preserved;
   cleanup failures are appended, or reported directly if the transfer succeeded.
   The D2XX backend allocates its handle owner before opening the channel and
   relies on that owner to close it once, including on initialization failure.
   Failure-injection tests reproduced both bugs before the fixes. Tests cover
   write/read/end failures, combined failures, failed opens, reinitialization,
   and moves. The real D2XX I2C source is compiled against test-only SDK stubs.
   Validation: Linux Debug, Release and FTDI-disabled suites pass. Native Windows
   SDK/driver validation and hardware acceptance remain pending.
3. **Complete hardware acceptance — partial; target validation pending.**
   Host USB setup passes and the original FT2232C/D enumerates on Linux.
   `--trace-i2c` is implemented for probe/read/i2c-test and offline-tested.
   Bench probes return unstable, unknown JEDEC IDs; a reserved-address negative
   control unexpectedly ACKs. A user-coordinated power-cycle retry also failed;
   stopping the board before Linux starts could not be confirmed. The user
   reports Winbond flash, but the exact part and wiring remain unverified.
   No logic analyzer is connected and no dumps were attempted. See
   [HARDWARE.md](HARDWARE.md) for commands, results and the remaining procedure:
   establish reliable identification, verify START/address/ACK/STOP and repeated
   START on a logic analyzer, then compare two complete dumps.
4. **Implement per-chip ECC handling — implemented and offline-tested.**
   All five table entries have explicit datasheet-backed profiles. Reads enable
   and verify ECC and per-page mode, check configuration before each page, and
   restore changed settings on success/failure. Report corrected bit ranges or
   counts, threshold/max-strength events and uncorrectable pages. Invalid status
   and failed configuration/status reads abort even with --fill-on-ecc-error.
   See [ECC.md](ECC.md) for sources, API, cleanup behavior and test coverage.
   Follow-up: general OOB image support and bad-block remapping remain open;
   raw backups and marker checks are implemented in step 5. SPI-NOR is deferred;
   physical validation of the ECC profiles remains part of step 3.
5. **Implement NAND erase/program/verify — implemented and offline-tested.**
   `write` selects the first detected programmer unless --serial is supplied.
   It requires a new raw backup path and --yes (or --dry-run). Validate block alignment, inspect markers, compare the backup
   with a second raw read, erase/program full pages, and verify every byte.
   Preserve fixed addresses: marked blocks abort rather than being skipped.
   Check WEL/E-FAIL/P-FAIL, never retry ambiguous mutations, and restore flash
   settings on failure. Standalone `verify` performs ECC-checked comparison.
   See [WRITING.md](WRITING.md) for backup format, limits and bad-block tests.
   Hardware programming validation and automatic raw restore remain pending.
   SPI-NOR, UART and CH341 remain later work.

Detailed completion checklists follow. Checked items have implementation or
validation evidence; unchecked items remain pending. Offline tests do not count
as hardware acceptance. Commands use the current executable name, `mstar-cli`.

### Milestone 0 — project skeleton

**Status: implementation mostly complete; CI and native Windows validation pending.**

- [x] Generate the directory structure.
- [x] Create hardware-independent `mstar-core`.
- [x] Define `Result`, `Error`, `I2cMaster` and `SpiBus`.
- [x] Create `MockI2cMaster` and `MockSpiBus`.
- [x] Configure Catch2.
- [x] Configure CLI11.
- [x] Add Windows/Linux CMake presets.
- [x] Support a local build without FTDI libraries.
- [ ] Automate the FTDI-disabled build in CI.

Acceptance:

- [x] Linux Debug build and tests: 6/6 CTest suites pass.
- [x] Linux Release build and tests: 6/6 CTest suites pass.
- [x] Linux FTDI-disabled build and tests: 5/5 CTest suites pass.
- [ ] Revalidate a native Windows build and its tests with the actual SDKs.

The D2XX backend is exercised on Linux using test-only SDK stubs; that does not
establish native Windows SDK/driver compatibility. Hardware is not required for
this milestone.

### Milestone 1 — FTDI discovery

**Status: complete for the original programmer on Linux.**

- [x] Implement `mstar-cli list`.
- [x] Enumerate the original FT2232C/D programmer on the host.
- [x] Display serial, description and channel.
- [x] Check Linux USB permissions and udev setup.

Hardware evidence: `Mstar USB Debug Tool A`, serial `ggggggggA`, channel A,
USB location `1:11` during the session. See [HARDWARE.md](HARDWARE.md).
Native Windows validation remains pending under milestone 0.

### Milestone 2 — FTDI I²C

**Status: implemented and offline-tested; electrical acceptance pending.**

- [x] Implement `FtdiI2c` using libftdi1/libusb on Linux and D2XX/LibMPSSE on Windows.
- [x] Exercise transfer sequences and error paths with fake backend APIs.
- [x] Add `mstar-cli i2c-test` and `--trace-i2c` for bench diagnostics.
- [ ] Verify START/address/data/STOP and repeated START on a logic analyzer.
- [ ] Confirm a genuine target ACK at `0x49` and a NACK for the negative control.

The software reported ACKs at both `0x49` and reserved address `0x7f`; this is
not reliable ACK evidence. No logic analyzer was connected. See
[HARDWARE.md](HARDWARE.md) for the failed checks and remaining procedure.

### Milestone 3 — MStar ISP + JEDEC ID

**Status: implemented and offline-tested; target acceptance pending.**

- [x] Implement `MstarIsp` activation, SPI write/read/end and ISP exit.
- [x] Implement `mstar-cli probe` with SPI `0x9F` identification.
- [x] Test command sequences, error propagation and SPI_END cleanup.
- [x] Keep probing free of flash program/erase operations.
- [ ] Confirm that the real target enters MStar ISP.
- [ ] Read a stable, valid JEDEC ID matching the connected flash.

Bench probes returned unstable, unknown IDs, including after a coordinated
power cycle. A pre-Linux boot state could not be confirmed. A successful
transport return or `ISP activation: OK` message does not satisfy acceptance.

### Milestone 4 — flash identification and read

**Status: SPI-NAND main-area reads implemented; SPI-NOR and hardware acceptance pending.**

- [x] Add five SPI-NAND table entries and identification by JEDEC ID.
- [x] Implement `mstar-cli read backup.bin` for the NAND main area.
- [x] Support byte offset/size and validate ranges without overflow.
- [x] Report read/output failures and retain incomplete files with diagnostics.
- [x] Make ECC substitution opt-in with `--fill-on-ecc-error` and exit code 2.
- [x] Test CLI output data, errors and exit codes using a fake programmer.
- [ ] Implement SPI-NOR identification and reads.
- [ ] Identify the actual connected flash and confirm its geometry.
- [ ] Produce two complete dumps of the expected size that compare identical.

No hardware dump has been attempted because target communication is unreliable.
The two-dump acceptance requirement remains open.

### Milestone 5 — NAND ECC/OOB/bad-block handling

**Status: ECC, raw backups and marker checks implemented; general OOB/remapping pending.**

- [x] Assign explicit ECC profiles to all five supported NAND parts.
- [x] Enable/verify ECC and per-page read mode; reject active OTP access.
- [x] Recheck read configuration before each page.
- [x] Decode per-chip primary/extended status and corrected-bit counts or ranges.
- [x] Report clean, corrected, threshold/max-strength and uncorrectable pages.
- [x] Abort on invalid status or configuration/transport failures, including in recovery mode.
- [x] Restore changed read settings on success and failure; report restoration errors.
- [x] Test decoding, setup, restoration and CLI behavior without hardware.
- [ ] Validate ECC behavior on physical parts after reliable target access is established.
- [x] Add ECC-off raw main+OOB backups with a documented manifest format.
- [x] Inspect spare markers and abort writes to marked or unreadable blocks.
- [x] Test marker detection and zero-mutation behavior with simulated NAND.
- [ ] Validate marker detection against reference tools on hardware.
- [ ] Add general OOB image input/output and automatic raw restore.
- [ ] Implement explicit bad-block mapping/skipping policies if needed.

See [ECC.md](ECC.md) for profiles, datasheet sources and reviewed reference
projects. The user authorized write-path implementation while physical
read/ECC validation remains pending; that status is tracked separately.

### Milestone 6 — erase/write/verify

**Status: write/verify implemented and offline-tested; hardware programming pending.**

- [x] Add `write` with optional serial, new backup path and --yes confirmation.
- [x] Add --dry-run for marker checks and verified backup without erase/program.
- [x] Validate block-aligned input/range and require repeated matching JEDEC ID.
- [x] Back up raw main+OOB, close the files and compare against a second read.
- [x] Erase required blocks and program full pages in ascending order.
- [x] Check write-enable and erase/program status; never retry failed mutations.
- [x] Verify every image byte and return non-zero on mismatch or cleanup failure.
- [x] Add standalone `verify` and restore protection/ECC settings on exit.
- [x] Test successful writes, marked blocks, timeouts, failed transfers and mismatches.
- [ ] Validate a controlled write/readback on the physical target with a verified backup.
- [ ] Add a standalone CLI erase command or automatic backup restore if needed.

Current scope is fixed-address main-area images; no partial-block update,
raw/OOB image programming or automatic bad-block remapping. See [WRITING.md](WRITING.md).

### Milestone 7 — UART console

**Status: pending; interfaces/stubs only.**

- [ ] Reverse-engineer FT2232D channel B and the 74HC08 routing.
- [ ] Implement the UART transport.
- [ ] Add `mstar-cli console` and validate it on hardware.

### Milestone 8 — CH341

**Status: pending.**

- [ ] Implement `Ch341I2c` using libusb.
- [ ] Integrate and validate it without changing the MStar/NAND layers.

The CMake option currently rejects enabling CH341 because no backend exists.

---

## 23. Instructions specifically for GitHub Copilot

When initializing this project:

1. **Do not port large pieces of SNANDer code.**
2. Use reference projects to understand behavior only.
3. Keep all FTDI API calls under `src/transport/ftdi/`.
4. `mstar-core` must have zero dependency on FTDI or libusb.
5. Use RAII for every FTDI/I²C handle.
6. Use `std::expected` for recoverable errors.
7. Keep hardware write/erase opt-in and backed up/verified. The user-authorized
   implementation priority and pending hardware acceptance are tracked in section 22.
8. Make protocol layers testable with `MockI2cMaster`.
9. Write unit tests for exact MStar I²C transaction sequences before hardware testing.
10. Do not silently ignore NACKs, short transfers, ECC errors or verify failures.
11. Preserve START/STOP/repeated-START semantics exactly.
12. Keep ISP addresses and I²C frequency configurable.
13. Do not hard-code a particular FTDI USB index.
14. Prefer small focused classes over a single large flasher class.
15. Every destructive feature must be explicitly opt-in.

The first Copilot implementation task is:

> Initialize a buildable C++23/CMake project according to this blueprint. Implement only the hardware-independent interfaces, error/result types, `MockI2cMaster`, CLI skeleton, CMake targets and unit-test infrastructure. Add stub FTDI backend classes but do not yet call D2XX or LibMPSSE. Add a unit test that models the expected MStar ISP JEDEC-ID sequence (`MSTAR`, SPI write `0x9F`, SPI read, SPI end). The project must compile and run tests on both Windows and Linux without the FTDI SDK installed.