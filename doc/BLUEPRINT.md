\# MStar / SigmaStar Cross-Platform In-System Flasher



\## 1. Project goal



Create a cross-platform command-line utility for \*\*in-system programming of SPI-NAND and SPI-NOR attached to MStar/SigmaStar SoCs\*\*, without desoldering the flash.



The first supported programmer is the original MStar/SigmaStar programmer based on \*\*FTDI FT2232D\*\*.



Initial supported host platforms:



\- Windows 10/11 x64 with MSVC.

\- Linux x86-64 with GCC or Clang.

\- Keep the architecture portable enough for macOS, but Windows and Linux are the first-class targets.



Initial programmer stack:



```text

PC

&#x20;│

&#x20;│ D2XX + LibMPSSE-I2C

&#x20;▼

FT2232D channel A

&#x20;│

&#x20;│ I2C

&#x20;▼

MStar / SigmaStar ISP slave

&#x20;│

&#x20;│ internal SPI bridge

&#x20;▼

SPI-NAND / SPI-NOR

```



The FT2232D is explicitly supported by FTDI's MPSSE and LibMPSSE-I2C stack. FTDI documents one MPSSE-capable channel on FT2232D, suitable for the I²C ISP path. citeturn774050search0turn843508view0



Later:



```text

&#x20;                      I2cMaster

&#x20;                         │

&#x20;             ┌───────────┴───────────┐

&#x20;             │                       │

&#x20;       FtdiMpsseI2c              Ch341I2c

&#x20;             │                       │

&#x20;    D2XX + LibMPSSE               libusb

&#x20;             │                       │

&#x20;          FT2232D                  CH341A

```



Nothing above `I2cMaster` should depend on the programmer hardware.



\---



\# 2. Important non-goals for the first milestone



Do \*\*not\*\* initially implement:



\- flash erase;

\- flash programming;

\- bad-block relocation;

\- raw OOB writing;

\- UART console;

\- CH341 support;

\- GUI.



The first hardware milestone is intentionally read-only:



```console

mstar-flasher list

mstar-flasher probe

```



Expected output should eventually resemble:



```text

Programmer:

&#x20; FTDI device: FT2232D

&#x20; Serial: XXXXXXXX

&#x20; MPSSE channel: A

&#x20; I2C clock: 100000 Hz



Target:

&#x20; MStar ISP address: 0x49

&#x20; ISP activation: OK



Flash:

&#x20; JEDEC ID: EF AA 22

&#x20; Type: SPI-NAND

&#x20; Model: ...

&#x20; Capacity: ...

```



`probe` must never erase or program anything.



\---



\# 3. Language and build system



Use:



```text

C++23

CMake

CMakePresets.json

```



Prefer standard C++ facilities:



\- `std::span`

\- `std::expected`

\- `std::filesystem`

\- RAII

\- `std::chrono`

\- `std::string\_view`



Avoid platform-specific code outside hardware backend directories.



Recommended third-party C++ dependencies:



\- \*\*CLI11\*\* for command-line parsing.

\- \*\*Catch2\*\* for tests.



CLI11 is cross-platform and dependency-light; Catch2 integrates cleanly with CMake. citeturn479883search1turn479883search0



Do not require the FTDI SDK to build the core library or unit tests.



\---



\# 4. Repository structure



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

│       │   ├── i2c\_master.hpp

│       │   ├── console.hpp

│       │   └── programmer.hpp

│       │

│       ├── isp/

│       │   └── mstar\_isp.hpp

│       │

│       └── flash/

│           ├── spi\_bus.hpp

│           ├── spi\_flash.hpp

│           ├── spi\_nand.hpp

│           ├── spi\_nor.hpp

│           ├── nand\_geometry.hpp

│           └── flash\_part.hpp

│

├── src/

│   ├── app/

│   │   └── main.cpp

│   │

│   ├── transport/

│   │   └── ftdi/

│   │       ├── ftdi\_device.cpp

│   │       ├── ftdi\_i2c.cpp

│   │       └── ftdi\_console.cpp

│   │

│   ├── isp/

│   │   └── mstar\_isp.cpp

│   │

│   └── flash/

│       ├── spi\_nand.cpp

│       ├── spi\_nor.cpp

│       └── flash\_database.cpp

│

├── tests/

│   ├── mock\_i2c.hpp

│   ├── test\_mstar\_isp.cpp

│   ├── test\_spi\_nand.cpp

│   └── test\_flash\_database.cpp

│

└── docs/

&#x20;   ├── BLUEPRINT.md

&#x20;   ├── MSTAR\_ISP.md

&#x20;   └── HARDWARE.md

```



Later add:



```text

src/transport/ch341/

```



without changing `isp/` or `flash/`.



\---



\# 5. Core abstraction: I2cMaster



The MStar ISP implementation must depend only on a generic I²C master.



Start with:



```cpp

class I2cMaster {

public:

&#x20;   virtual \~I2cMaster() = default;



&#x20;   virtual Result<void> setClock(uint32\_t hz) = 0;



&#x20;   virtual Result<void> write(

&#x20;       uint8\_t address,

&#x20;       std::span<const uint8\_t> data

&#x20;   ) = 0;



&#x20;   virtual Result<void> read(

&#x20;       uint8\_t address,

&#x20;       std::span<uint8\_t> data

&#x20;   ) = 0;



&#x20;   virtual Result<void> writeRead(

&#x20;       uint8\_t address,

&#x20;       std::span<const uint8\_t> tx,

&#x20;       std::span<uint8\_t> rx

&#x20;   ) = 0;

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



Do not leak `FT\_HANDLE`, `FT\_STATUS`, LibMPSSE constants or libusb types through this interface.



\---



\# 6. FT2232D backend



Use:



```text

FTDI D2XX

\+

FTDI LibMPSSE-I2C

```



rather than raw MPSSE for version 1.



FTDI currently recommends LibMPSSE-I2C for MPSSE-based I²C and provides libraries, examples and source. citeturn774050search0turn774050search3



The backend should implement:



```cpp

class FtdiI2c final : public I2cMaster {

public:

&#x20;   static Result<std::vector<FtdiDeviceInfo>> enumerate();



&#x20;   static Result<FtdiI2c> open(

&#x20;       const FtdiDeviceSelector\& selector

&#x20;   );



&#x20;   Result<void> setClock(uint32\_t hz) override;

&#x20;   Result<void> write(...) override;

&#x20;   Result<void> read(...) override;

&#x20;   Result<void> writeRead(...) override;

};

```



Use LibMPSSE functions such as:



```text

I2C\_GetNumChannels

I2C\_GetChannelInfo

I2C\_OpenChannel

I2C\_InitChannel

I2C\_DeviceWrite

I2C\_DeviceRead

I2C\_CloseChannel

```



The FTDI guide confirms FT2232D contributes one I²C-capable MPSSE channel. citeturn843508view0



\### Device selection



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



\### Default I²C clock



Start conservatively at:



```text

100 kHz

```



Make it configurable:



```console

\--i2c-clock 100000

```



Do not optimize speed until communication is proven reliable.



\---



\# 7. MStar ISP protocol layer



Create:



```cpp

class MstarIsp final : public SpiBus {

public:

&#x20;   explicit MstarIsp(

&#x20;       I2cMaster\& i2c,

&#x20;       uint8\_t ispAddress = 0x49

&#x20;   );



&#x20;   Result<void> enter();

&#x20;   Result<void> leave();



&#x20;   Result<void> transaction(

&#x20;       std::span<const uint8\_t> tx,

&#x20;       std::span<uint8\_t> rx

&#x20;   ) override;

};

```



Default ISP address:



```text

0x49

```



Keep it configurable because some MStar variants use different addresses.



The MStar/SigmaStar ISP mechanism exposes the SoC's SPI bus over an I²C slave. The known protocol uses the activation string `MSTAR` and command bytes `0x10`, `0x11`, `0x12`, etc. The `snander\_electricboogaloo` project is the main behavioral reference for this layer. citeturn884236view0



\### Enter ISP



Send in \*\*one I²C transaction\*\*:



```text

"MSTAR"



4D 53 54 41 52

```



to the ISP slave.



Do not split this string into multiple I²C writes.



\### SPI write



For SPI data:



```text

I2C payload:



10 <SPI bytes...>

```



where:



```text

0x10 = MSTARDDC\_SPI\_WRITE

```



\### SPI read



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

0x11 = MSTARDDC\_SPI\_READ

```



\### End SPI transaction



Send:



```text

12

```



which releases the emulated SPI chip-select.



\### Other known commands



Keep constants for:



```text

0x10  SPI\_WRITE

0x11  SPI\_READ

0x12  SPI\_END

0x20  STATUS

0x24  RESET / EXIT

```



Do not initially depend on `STATUS` for basic operation unless hardware testing demonstrates it is required.



\### Critical SPI transaction rule



`0x12` must only be sent when the complete SPI transaction should end.



For example, reading JEDEC ID should conceptually be:



```text

I2C WRITE:

&#x20;   10 9F



I2C WRITE + repeated-start READ:

&#x20;   11

&#x20;   <ID bytes>



I2C WRITE:

&#x20;   12

```



Do not release CS between `0x9F` and reading the ID.



\---



\# 8. SpiBus abstraction



The flash code must not know that SPI is transported through MStar ISP.



Use:



```cpp

class SpiBus {

public:

&#x20;   virtual \~SpiBus() = default;



&#x20;   virtual Result<void> transaction(

&#x20;       std::span<const uint8\_t> tx,

&#x20;       std::span<uint8\_t> rx

&#x20;   ) = 0;

};

```



Therefore:



```text

SPI-NAND

&#x20;  │

&#x20;  ▼

SpiBus

&#x20;  │

&#x20;  ▼

MstarIsp

&#x20;  │

&#x20;  ▼

I2cMaster

&#x20;  │

&#x20;  ▼

FT2232D

```



Later another SPI transport could theoretically be added without rewriting NAND support.



\---



\# 9. SPI-NAND layer



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

&#x20;   uint32\_t pageSize;

&#x20;   uint32\_t oobSize;

&#x20;   uint32\_t pagesPerBlock;

&#x20;   uint32\_t blocksPerLun;

&#x20;   uint32\_t luns;

};



struct FlashPart {

&#x20;   std::string\_view vendor;

&#x20;   std::string\_view model;



&#x20;   std::array<uint8\_t, 4> id;

&#x20;   size\_t idLength;



&#x20;   NandGeometry geometry;



&#x20;   uint32\_t eccStepSize;

&#x20;   uint32\_t eccStrength;

};

```



Separate geometry, vendor-specific ECC interpretation and flash operations.



\### ECC



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



\### OOB



Support OOB/raw mode only after ordinary page reads are stable.



Eventually expose something similar to:



```console

\--raw

\--include-oob

\--ecc on

\--ecc off

```



but these are later milestones.



\---



\# 10. Flash database



Do not hard-code flash logic into `spi\_nand.cpp`.



Use a table/database layer:



```text

JEDEC ID

&#x20; ↓

FlashPart

&#x20; ↓

geometry + ECC/vendor callbacks

```



Useful references for part definitions and NAND behavior:



\- SNANDer.

\- `snander\_electricboogaloo`.

\- `ufprog`.



`ufprog` is particularly useful as a modern reference because it has explicit abstractions for controllers, interfaces, SPI-NAND, ECC and vendor-specific part tables. citeturn479883view0



Treat these repositories as \*\*reference implementations\*\*, not as code to paste blindly.



\---



\# 11. Error handling



Use `std::expected`.



Example:



```cpp

enum class ErrorCode {

&#x20;   Usb,

&#x20;   Ftdi,

&#x20;   DeviceNotFound,

&#x20;   DeviceBusy,



&#x20;   I2cNack,

&#x20;   I2cTransfer,

&#x20;   Timeout,



&#x20;   IspActivationFailed,

&#x20;   IspProtocol,



&#x20;   FlashUnknown,

&#x20;   FlashProtocol,

&#x20;   EccUncorrectable,

&#x20;   BadBlock,



&#x20;   VerifyMismatch,

&#x20;   InvalidArgument,

&#x20;   Io

};



struct Error {

&#x20;   ErrorCode code;

&#x20;   std::string message;

};



template<class T>

using Result = std::expected<T, Error>;

```



Translate FTDI-specific errors inside the FTDI backend.



No `FT\_STATUS` should escape into the MStar or NAND layers.



\---



\# 12. Logging and protocol tracing



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



\---



\# 13. CLI design



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



\---



\# 14. Testing strategy



This is important: \*\*the MStar ISP protocol should be testable without hardware\*\*.



Create:



```cpp

class MockI2cMaster final : public I2cMaster

```



which records transactions.



A unit test for JEDEC ID should verify the exact sequence:



```text

write 0x49:

&#x20;   "MSTAR"



write 0x49:

&#x20;   10 9F



writeRead 0x49:

&#x20;   tx = 11

&#x20;   rx = 3 bytes



write 0x49:

&#x20;   12

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



\---



\# 15. Hardware validation strategy



Before implementing flash writes:



\### Test A — FTDI enumeration



Verify:



```console

mstar-flasher list

```



finds the original FT2232D programmer.



\### Test B — I²C electrical activity



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



\### Test C — MSTAR activation



Send:



```text

MSTAR

```



and verify ACKs.



\### Test D — SPI JEDEC ID



Send SPI `0x9F` through MStar ISP.



Compare the detected ID with the actual flash mounted on the board.



\### Test E — repeated reads



Read the same region multiple times and ensure identical results.



Only after these pass should erase/program commands be enabled.



\---



\# 16. FT2232D UART console — later milestone



Keep UART separate from `I2cMaster`.



Use:



```cpp

class Console {

public:

&#x20;   virtual \~Console() = default;



&#x20;   virtual Result<void> setBaudRate(uint32\_t baud) = 0;

&#x20;   virtual Result<size\_t> read(std::span<uint8\_t>) = 0;

&#x20;   virtual Result<size\_t> write(std::span<const uint8\_t>) = 0;

};

```



FT2232D channel B can later be accessed through D2XX as UART.



Expected architecture:



```text

FT2232D

&#x20;├── Channel A → MPSSE → I2C ISP

&#x20;└── Channel B → UART console

```



Do not implement this until the original programmer's \*\*74HC08 gating/routing\*\* has been understood.



Document its PCB wiring in:



```text

docs/HARDWARE.md

```



\---



\# 17. Future CH341 backend



CH341 support must require \*\*no changes\*\* to:



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

&#x20;   ch341\_device.cpp

&#x20;   ch341\_i2c.cpp

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



\---



\# 18. CMake configuration



Provide options:



```cmake

option(MSTAR\_BUILD\_TESTS "Build unit tests" ON)

option(MSTAR\_ENABLE\_FTDI "Enable FTDI programmer backend" ON)

option(MSTAR\_ENABLE\_CH341 "Enable CH341 backend" OFF)

```



FTDI SDK paths should be discoverable through variables such as:



```text

FTD2XX\_ROOT

LIBMPSSE\_ROOT

```



Example configuration:



```console

cmake -S . -B build ^

&#x20; -DFTD2XX\_ROOT=C:/SDK/FTDI/D2XX ^

&#x20; -DLIBMPSSE\_ROOT=C:/SDK/FTDI/LibMPSSE

```



Linux equivalent:



```console

cmake -S . -B build \\

&#x20; -DFTD2XX\_ROOT=/opt/ftdi \\

&#x20; -DLIBMPSSE\_ROOT=/opt/ftdi/libmpsse

```



Tests and the core library must build when:



```cmake

MSTAR\_ENABLE\_FTDI=OFF

```



This makes CI independent of proprietary/vendor SDK binaries.



\---



\# 19. CMake targets



Prefer separate targets:



```text

mstar-core

&#x20;   Hardware-independent:

&#x20;   MStar ISP

&#x20;   SPI bus

&#x20;   SPI NAND/NOR

&#x20;   flash database



mstar-ftdi

&#x20;   D2XX + LibMPSSE backend



mstar-cli

&#x20;   CLI executable



mstar-tests

&#x20;   Unit tests

```



Later:



```text

mstar-ch341

```



Dependency graph:



```text

mstar-cli

&#x20;  │

&#x20;  ├── mstar-core

&#x20;  │

&#x20;  └── mstar-ftdi



mstar-tests

&#x20;  │

&#x20;  └── mstar-core

```



`mstar-core` must never link against D2XX, LibMPSSE or libusb.



\---



\# 20. Licensing rule



Decide the new project's license independently.



Important:



`snander\_electricboogaloo` and SNANDer are GPL-family projects. The former identifies itself as GPL-2.0, while Droid-MAX documents SNANDer as GPL v2 or later. citeturn884236view0turn591648view4



Therefore:



\*\*Unless this new project intentionally uses a GPL-compatible license, do not copy source code from those repositories.\*\*



Use them to understand:



```text

protocol

command sequencing

flash behavior

hardware behavior

```



and write a clean C++ implementation.



The same rule applies when consulting `ufprog`: check the license of any specific component before copying code.



\---



\# 21. Reference material



\## Essential FTDI documentation



\*\*FTDI D2XX drivers\*\*



Required host driver/library for the FT2232D implementation.



\[FTDI D2XX Drivers](https://ftdichip.com/drivers/d2xx-drivers/?utm\_source=chatgpt.com)



\*\*D2XX Programmer's Guide\*\*



Reference for device enumeration, opening interfaces, UART operation and D2XX error handling.



\[D2XX Programmer's Guide](https://ftdichip.com/wp-content/uploads/2025/06/D2XX\_Programmers\_Guide.pdf?utm\_source=chatgpt.com)



\*\*LibMPSSE-I2C downloads, examples and source\*\*



Primary implementation library for the FT2232D I²C backend.



\[FTDI LibMPSSE-I2C](https://ftdichip.com/software-examples/mpsse-projects/libmpsse-i2c-examples/?utm\_source=chatgpt.com)



\*\*AN\_177 — LibMPSSE-I2C User Guide\*\*



API and transfer-options reference.



\[LibMPSSE-I2C User Guide AN\_177](https://www.ftdichip.com/old2020/Support/Documents/AppNotes/AN\_177\_User\_Guide\_For\_LibMPSSE-I2C.pdf)



\*\*FT2232D product documentation\*\*



Hardware/MPSSE reference for the original programmer.



\[FTDI FT2232D product page](https://ftdichip.com/products/ft2232d/?utm\_source=chatgpt.com)



\---



\## Essential MStar/SigmaStar references



\*\*fifteenhex/snander\_electricboogaloo\*\*



Most important reference for the MStar DDC/ISP transport abstraction.



\[snander\_electricboogaloo repository](https://github.com/fifteenhex/snander\_electricboogaloo?utm\_source=chatgpt.com)



Pay particular attention to:



```text

src/mstarddc\_spi.c

src/i2c\_controller.\*

src/spi\_controller.\*

src/spi\_nand\_flash.\*

src/spi\_nand\_ids.\*

```



\*\*OpenIPC/snander-mstar\*\*



Compact MStar + CH341-oriented reference.



\[OpenIPC snander-mstar repository](https://github.com/OpenIPC/snander-mstar?utm\_source=chatgpt.com)



Useful files:



```text

src/spi\_controller.c

src/spi\_nand\_flash.c

src/ch341a\_i2c.c

src/ch341a\_spi.c

```



\*\*Linux Chenxing MStar ISP reverse-engineering notes\*\*



Important protocol and original-programmer hardware reference.



\[MStar ISP / SERDB reverse-engineering notes](https://github.com/linux-chenxing/linux-chenxing.org/blob/master/isp/index.md?utm\_source=chatgpt.com)



This is particularly useful for:



```text

ISP address

SERDB address

"MSTAR"

"SERDB"

0x10 / 0x11 / 0x12 commands

original FT2232D programmer hardware

```



\---



\## SPI-NAND references



\*\*Droid-MAX/SNANDer\*\*



Useful for:



```text

SPI-NAND command handling

chip database

ECC handling

bad blocks

CH341 transport

Windows/Linux/macOS build examples

```



\[Droid-MAX SNANDer repository](https://github.com/Droid-MAX/SNANDer?utm\_source=chatgpt.com)



\*\*ufprog\*\*



Useful modern architecture/reference for:



```text

SPI-NAND vendor tables

geometry

ECC abstraction

OOB layouts

ONFI

controller/interface separation

```



\[ufprog repository](https://github.com/hackpascal/ufprog?utm\_source=chatgpt.com)



\---



\## Future CH341 dependency



\*\*libusb\*\*



Use for the eventual CH341 backend.



\[libusb repository](https://github.com/libusb/libusb?utm\_source=chatgpt.com)



\---



\## Development dependencies



\*\*CLI11\*\*



\[CLI11 repository](https://github.com/CLIUtils/CLI11?utm\_source=chatgpt.com)



\*\*Catch2\*\*



\[Catch2 repository](https://github.com/catchorg/Catch2?utm\_source=chatgpt.com)



\---



\# 22. Implementation milestones



\## Milestone 0 — project skeleton



Copilot should:



1\. Generate the directory structure.

2\. Create `mstar-core`.

3\. Define `Result`, `Error`, `I2cMaster` and `SpiBus`.

4\. Create `MockI2cMaster`.

5\. Configure Catch2.

6\. Configure CLI11.

7\. Add Windows/Linux CMake presets.

8\. Make CI build without FTDI libraries.



Acceptance condition:



```text

Windows build: PASS

Linux build: PASS

unit tests: PASS

hardware code: not required

```



\## Milestone 1 — FTDI discovery



Implement:



```console

mstar-flasher list

```



Acceptance condition:



```text

original FT2232D programmer is enumerated

serial/description/channel are displayed

```



\## Milestone 2 — FTDI I²C



Implement `FtdiI2c`.



Acceptance condition:



```text

START/address/data/STOP verified on logic analyzer

0x49 ACK observed

```



\## Milestone 3 — MStar ISP + JEDEC ID



Implement `MstarIsp`.



Acceptance condition:



```console

mstar-flasher probe

```



successfully:



```text

enters MStar ISP

sends SPI 0x9F

reads flash JEDEC ID

```



No flash modification.



\## Milestone 4 — flash identification and read



Implement enough SPI-NAND/SPI-NOR support to identify the connected flash and read data.



Add:



```console

mstar-flasher read backup.bin

```



Acceptance condition:



```text

two complete reads produce identical files

```



\## Milestone 5 — NAND ECC/OOB/bad-block handling



Implement robust NAND status interpretation.



Do not add programming until reads and ECC handling are trusted.



\## Milestone 6 — erase/write/verify



Add destructive operations with explicit confirmation.



Default workflow:



```text

backup

erase required blocks

program

verify

```



Verification failure must return non-zero exit status.



\## Milestone 7 — UART console



Reverse-engineer FT2232D channel B and the 74HC08 routing.



Add:



```console

mstar-flasher console

```



\## Milestone 8 — CH341



Implement `Ch341I2c` using libusb.



No MStar/NAND layer changes should be required.



\---



\# 23. Instructions specifically for GitHub Copilot



When initializing this project:



1\. \*\*Do not port large pieces of SNANDer code.\*\*

2\. Use reference projects to understand behavior only.

3\. Keep all FTDI API calls under `src/transport/ftdi/`.

4\. `mstar-core` must have zero dependency on FTDI or libusb.

5\. Use RAII for every FTDI/I²C handle.

6\. Use `std::expected` for recoverable errors.

7\. Do not implement flash write/erase until `probe` and `read` are complete.

8\. Make protocol layers testable with `MockI2cMaster`.

9\. Write unit tests for exact MStar I²C transaction sequences before hardware testing.

10\. Do not silently ignore NACKs, short transfers, ECC errors or verify failures.

11\. Preserve START/STOP/repeated-START semantics exactly.

12\. Keep ISP addresses and I²C frequency configurable.

13\. Do not hard-code a particular FTDI USB index.

14\. Prefer small focused classes over a single large flasher class.

15\. Every destructive feature must be explicitly opt-in.



The first Copilot implementation task is:



> Initialize a buildable C++23/CMake project according to this blueprint. Implement only the hardware-independent interfaces, error/result types, `MockI2cMaster`, CLI skeleton, CMake targets and unit-test infrastructure. Add stub FTDI backend classes but do not yet call D2XX or LibMPSSE. Add a unit test that models the expected MStar ISP JEDEC-ID sequence (`MSTAR`, SPI write `0x9F`, SPI read, SPI end). The project must compile and run tests on both Windows and Linux without the FTDI SDK installed.

