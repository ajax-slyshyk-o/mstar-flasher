# Hardware acceptance

## Status — 2026-11-24

**Partial: USB setup and enumeration pass; target identification and complete
dump acceptance do not pass.** No logic analyzer is connected, so waveform
validation remains pending. No flash program or erase operation was performed.
No flash dump was attempted because identification is unreliable.

Tested source: commit `109c076` plus the `--trace-i2c` changes accompanying this
record. Local transcripts are under `build/hardware-acceptance/2026-11-24/`
(ignored build artifacts); the significant results are preserved below.

## Bench configuration

| Item | Observed or supplied value |
| --- | --- |
| Host | Ubuntu 24.04.5, libftdi1 1.5, libusb 1.0.27 |
| Build | `linux` Debug, GCC 13.3, CMake 3.28.3, FTDI enabled |
| Programmer | User-described SigmaStar programmer; USB manufacturer `MSTAR`, product `Mstar USB Debug Tool` |
| USB | VID:PID `0403:6010`, FT2232C/D, location `1:11` during this session |
| Selection | USB serial `gggggggg`; CLI channel A serial `ggggggggA` |
| Target | User's custom board; SoC model/revision not yet recorded |
| Flash | Winbond according to user; exact part, NAND/NOR type and capacity unconfirmed |
| Wiring/power | Programmer connected to board; connector pinout, voltages, pull-ups and ground connection not independently verified |
| Boot state | User reports JEDEC access also has problems once the board starts Linux; bootloader stop could not be confirmed after the power-cycle retry |
| Analyzer | None connected |

The transport assumes AD0=SCL, AD1=SDA output and AD2=SDA input. These are
software assumptions, not a verified pinout of this programmer's connector or
74HC08 routing. Do not infer board wiring from them.

## Executed checks

Commands were run with host USB access, outside the execution sandbox that hides
USB device nodes. `--trace-i2c` logs API transactions and returned data; it is not
a waveform capture.

```sh
./scripts/setup-linux.sh --check
./build/linux/mstar-cli list
./build/linux/mstar-cli i2c-test --serial ggggggggA --i2c-clock 100000 --trace-i2c
./build/linux/mstar-cli probe --serial ggggggggA --i2c-clock 100000 --trace-i2c
./build/linux/mstar-cli i2c-test --serial ggggggggA --i2c-address 0x7f --i2c-clock 100000 --trace-i2c
./build/linux/mstar-cli probe --serial ggggggggA --i2c-clock 10000 --trace-i2c
# User power-cycled the board before the following command:
./build/linux/mstar-cli probe --serial ggggggggA --i2c-clock 100000 --stay-in-isp --trace-i2c
```

| Check | Result | Acceptance |
| --- | --- | --- |
| Host setup | Check passes; udev rule installed, user in `plugdev` | Pass |
| Enumeration | FT2232C/D, `Mstar USB Debug Tool A`, `ggggggggA`, channel A, USB `1:11` | Pass |
| `i2c-test`, 100 kHz, `0x49` | Exit 0; reports ACK for write of `00` | Inconclusive: negative control also ACKs |
| `probe`, 100 kHz | Exit 0; unknown ID `FF 9F BF FF 3F FF FF 7F` | Fail |
| Negative control, reserved `0x7f` | Exit 0; unexpectedly reports ACK | Fail; ACK path/physical bus needs investigation |
| `probe`, 10 kHz | Exit 1; `SPI_END failed: I2C NACK sending 0x12`; exit-ISP write succeeds | Fail |
| `probe` after power cycle, 100 kHz, `--stay-in-isp` | Exit 0; unknown ID `AD 2A 32 92 B3 50 B5 F5` | Fail; pre-Linux state not confirmed |
| START/STOP, clock, ACK/NACK, repeated START | No analyzer | Pending |
| Two complete, matching dumps | Not attempted | Pending |

Current `probe` returns zero for an unknown ID when the transport reports
success. Acceptance therefore requires a stable, correct identification in
addition to the exit code. The displayed `ISP activation: OK` means the write
returned success; given the negative-control result, it does not prove that the
SoC entered ISP. Similarly, the last command omitted ISP exit, but the message
about a halted target is not independently verified.

Selected trace from the first probe:

```text
I2C W 0x49: 4D 53 54 41 52 -> OK
I2C W 0x49: 10 FF -> OK
I2C W 0x49: 12 -> OK
I2C W 0x49: 10 0F C0 -> OK
I2C WR 0x49: 11 -> BF
I2C W 0x49: 12 -> OK
I2C W 0x49: 10 0F C0 -> OK
I2C WR 0x49: 11 -> F0
I2C W 0x49: 12 -> OK
I2C W 0x49: 10 9F 00 -> OK
I2C WR 0x49: 11 -> FF 9F BF FF 3F FF FF 7F
I2C W 0x49: 12 -> OK
I2C W 0x49: 24 -> OK
```

## Remaining procedure

1. Record the SoC, board revision, exact flash marking, connector mapping and
   measured signal voltage. Confirm the board is stopped before Linux starts
   using its boot console or a known working vendor-tool procedure. A power
   cycle followed by a delayed host command alone does not establish this.
2. Repeat identification and the negative control in that confirmed state.
   Investigate unexpected ACKs before trusting data: compare with the target
   disconnected, then check SDA sensing, pull-ups, routing and UART activity.
   The `i2c-test` command sends a `00` data byte after the address; this is not
   an address-only bus scan. Use `0x7f` only as a deliberate diagnostic on this
   isolated bench, where no device is expected to acknowledge it.
3. Connect an analyzer to board-side SCL, SDA and common ground. Capture
   `i2c-test` at 100 kHz: START, `0x49` write address (`0x92` on the wire),
   address ACK, `00`, data ACK, STOP. Confirm a genuine NACK for the negative
   control. Capture `probe`: the `11` write followed by repeated START,
   `0x49` read address (`0x93`), returned bytes, final master NACK and STOP.
   Save captures, decoder settings, actual clock and rise-time measurements.
4. Require repeatable JEDEC bytes matching the physical part and a verified
   table entry/geometry. The current read path is SPI-NAND; an unidentified
   Winbond part must not be assumed to be supported NAND. Validate ECC behavior
   on the physical part before treating a dump as an archival
   backup. Software profiles are now implemented; see [ECC.md](ECC.md).
5. Read a small sample twice first, compare it and measure throughput. Then read
   two full main-area dumps without `--size` or `--fill-on-ecc-error`, retaining
   logs and checking each command's exit code. Use distinct new output paths
   because `read` overwrites existing files. For an identified, supported part:

   ```sh
   ./build/linux/mstar-cli read sample-a.bin --serial ggggggggA --size 4096 --stay-in-isp --trace-i2c 2>sample-a.log
   ./build/linux/mstar-cli read sample-b.bin --serial ggggggggA --size 4096 --stay-in-isp --trace-i2c 2>sample-b.log
   cmp sample-a.bin sample-b.bin
   ./build/linux/mstar-cli read dump-a.bin --serial ggggggggA --stay-in-isp 2>dump-a.log
   ./build/linux/mstar-cli read dump-b.bin --serial ggggggggA 2>dump-b.log
   stat -c '%n %s bytes' dump-a.bin dump-b.bin
   sha256sum dump-a.bin dump-b.bin
   cmp dump-a.bin dump-b.bin
   ```

   Keep the target in the verified ISP state between operations; if it resumes
   booting, restore that state before proceeding. Both full reads must exit 0,
   have the exact main-area capacity, and compare equal. Record elapsed time,
   ECC diagnostics, hashes and sizes here. Matching hashes alone cannot prove
   correctness if both reads return the same invalid data. OOB coverage remains
   follow-up work after the per-chip ECC implementation in blueprint step 4;
   physical ECC validation is still required.

Only mark hardware acceptance complete once the identification, waveform and
full-dump evidence above is present. Tracing itself is covered by offline tests
for transfer preservation, error propagation, buffer summarization and CLI
stderr routing.

Builds and CTest suites pass with these changes: Linux Debug (5/5), Linux
Release (5/5), and FTDI disabled (4/4). Release still emits the pre-existing
GCC 13 vector-insert warning in `MstarIsp::transaction`; no native Windows build
was run during this session.
