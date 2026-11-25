# Original Flash Tool transport and programming performance

Investigation date: 2026-11-25. CLI baseline: commit `bbebcf6`.

The main Windows throughput problem is the byte-at-a-time LibMPSSE path.
The original tool builds whole transactions in memory and submits large MPSSE
command buffers through D2XX. Its 16 ms latency timer matches ours.

## Evidence and limits

- Original executable: `D:\bin\FlashTool_5_0_60\Flash_Tool_5.0.60.exe`, x86.
- SHA-256: `1ef53586d40a6803d0addce2be1b7d9381edac575215e28fe0d083c974a01f43`.
- D2XX driver DLL: x86 `ftd2xx.dll`, version 3.2.21.1.
- Disassembly plus a three-minute observer attached to the running process.
  The observer recorded D2XX arguments, buffers, return values and timestamps;
  it did not issue flash commands. The user operated the original tool and
  reported completing flashing with verification disabled.
- The observer detached at the end of the capture. All 4,938 captured
  `FT_Write` and 1,176 `FT_Read` calls returned success and full lengths.
  This is driver-level evidence, not independent verification of flash contents.
- The programming sequence contains 215 distinct program-execute row addresses.
  The interval from the first large program-buffer call to the end of the last
  is 28.748 seconds, about 15.0 KiB/s for 215 x 2,048 main bytes. This interval
  includes intervening reads and status/control traffic, but excludes initial
  setup/erase and any final verification.
- Timings include instrumentation overhead and use millisecond timestamps.
  They are not an uninstrumented benchmark or an optimized-CLI speed prediction.

Local artifacts are under ignored `build/vendor-analysis/`:

- `vendor-ftdi-20261125.jsonl`: complete captured buffers and events.
- `capture_ftdi.py`: observer; attaches only when explicitly run.
- `summarize_ftdi.py`: offline decoder and timing summary.
- `metadata.json`, `ftdi-disassembly.txt`, `nand-read-disassembly.txt`,
  `nand-write-disassembly.txt`, `ftdi-calls.txt`: static analysis.

Reproduce the summary without touching hardware:

```powershell
.\build\vendor-analysis\.venv\Scripts\python.exe .\build\vendor-analysis\summarize_ftdi.py .\build\vendor-analysis\vendor-ftdi-20261125.jsonl
```

These artifacts are local investigation output, not shipped project dependencies.

## How the original tool transfers pages

The original executable calls D2XX directly; no LibMPSSE DLL was imported or
observed loaded. The decoded MPSSE stream uses GPIO transitions for START/STOP,
`0x11` for outgoing bytes, `0x20` for incoming bytes, `0x13` for outgoing
ACK/NACK clocks, and `0x87` to flush received data to the host. Opcode meanings
are from [FTDI AN_108](https://ftdichip.com/Support/Documents/AppNotes/AN_108_Command_Processor_for_MPSSE_and_MCU_Host_Bus_Emulation_Modes.pdf).

| Operation | Captured behavior | Timing across 215 calls |
| --- | --- | --- |
| Read a 2,176-byte main+spare page | One 26,245-byte MPSSE `FT_Write`, then one `FT_Read(2176)` | Command submission median 57 ms, range 42-67 ms |
| Load a program buffer | One 20,734-byte MPSSE `FT_Write` | Median 50 ms, range 39-63 ms |
| Collect the page read | One `FT_Read(2176)`, after waiting for receive data | Median 0 ms, range 0-1 ms; data was already queued |

MPSSE buffer sizes include command framing and GPIO operations; they are not
flash payload sizes. In particular, the near-zero `FT_Read` duration does not
mean that reading a NAND page takes zero time.

Representative decoded sequence, using 7-bit ISP address `0x49`
(wire addresses `0x92/0x93`):

1. Load NAND page into cache: `10 13 <row>`, followed by ISP `12`.
2. Read status using `10 0F C0`, ISP `11` + repeated START/read, then `12`.
3. Cache read: `10 03 00 00 00`, then ISP `11`, repeated START and 2,176
   read bytes in a single command stream. Finish with STOP and ISP `12`.
4. A single program-buffer submission combines WREN `10 06`, ISP `12`,
   program-load `10 02 00 00 <data>`, and ISP `12`.
5. Submit program-execute `10 10 <row>`, then ISP `12`.
6. Poll status until ready; the sample changes from `03` to `00`.
   Finish with write-disable `10 04` and ISP `12`.

Two details should be investigated separately from transport performance:

- The 215 full-page reads precede programming and repeatedly target the first
  page of the current 64-page block, rather than the current destination page.
  This is consistent with a block-marker check; its purpose has not been
  confirmed from the caller. It is not a full readback verification pass.
- Each program-load sends 2,048 main bytes followed by five bytes
  `FF FF FF FF 00` at the start of spare space. This is a vendor OOB policy,
  not a requirement established for our main-area writer. Do not copy this
  sequence into the writer as part of a transport optimization.

## Clock and ACK behavior

All seven captured initializations set the latency timer to 16 ms and submit
`85 86 0D 00`: disable loopback and set divisor 13. For the FT2232D clock,
`12 MHz / (2 * (13 + 1))` gives approximately 428.6 kHz for clocked bits.
The calculation follows [FTDI's divisor documentation](https://www.ftdichip.com/Support/Knowledgebase/i2c_initdevice.htm).
This is a configured clock calculation, not a measured board-side waveform;
GPIO commands and transfer gaps also affect effective throughput.
Our default is 100 kHz.

The captured fast path clocks outgoing ACK slots with `13 00 FF` and does not
sample slave ACK bits: there are no input-bit MPSSE commands in the entire
decoded capture. Status reads return NAND data and do not substitute for
checking I2C address/data ACKs. Preserve our error reporting when adopting
batching; the vendor's lack of sampled ACKs should not become our behavior.

Static addresses in this exact executable support the runtime observation:

| Virtual address | Observed role |
| --- | --- |
| `0x444B48` | Append a byte to the pending MPSSE buffer |
| `0x444B60` | Flush the pending buffer with `FT_Write` |
| `0x444BB0` | Query receive queue and retrieve data with `FT_Read` |
| `0x444DB8` | Build clock-divisor command |
| `0x445148` / `0x445190` | Enqueue read/write byte commands |
| `0x44523C` | Build combined write/read and flush after all read commands |
| `0x4453D0` | Enqueue write buffers without immediately flushing |
| `0x445710` | Enqueue a one-byte ISP command and flush |
| `0x456FF4` / `0x458134` | NAND read/program helpers |

## Why our Windows path is slow

`src/transport/ftdi/ftdi_i2c.cpp` calls `I2C_DeviceRead/Write` without
`I2C_TRANSFER_OPTIONS_FAST_TRANSFER_BYTES`.

In the bundled source
`build/windows/_deps/LIBMPSSE-src/release/source/ftdi_i2c.c`:

- `I2C_DeviceRead` loops through `I2C_Read8bitsAndGiveAck` (around lines
  599-605). Each byte issues a command write, SEND_IMMEDIATE and a one-byte
  host read (lines 1093-1108).
- `I2C_DeviceWrite` loops through `I2C_Write8bitsAndGetAck` (around lines
  699-704). Each byte issues a command write and a one-byte ACK read, with
  `INFRA_SLEEP(1)` between them (lines 999-1022).
- Consequently a 2,048-byte payload costs roughly 2,048 driver write/read pairs,
  before control traffic. Bulk transfer removes that host synchronization cost.
- Merely selecting the library's fast flag is insufficient: its fast path
  does not implement BREAK_ON_NACK, reads the address ACK without rejecting
  NACK, and the public fast-write dispatch does not expose data ACKs for our
  caller to validate. Check exact lengths and ACKs explicitly.

Verification being disabled in the vendor run also affects total elapsed time.
Our mandatory final verification is a separate read pass. Batching can speed
up that pass as well; the performance evidence does not justify removing it.

## Next implementation and validation

1. Add batched MPSSE data transfers in the Windows transport. Start with reads,
   keeping a checked address phase, repeated START, final master NACK, STOP,
   timeouts, short-transfer checks and cleanup.
2. Batch program-load data and collect all ACK samples in one receive buffer.
   Validate them before returning success and before sending program-execute.
   Keep control operations checked. A submitted batch cannot stop immediately
   at its first NACK; document that distinction and never retry an ambiguous
   mutation. Preserve the existing FT2232D two-phase clocking workaround.
3. Add focused transport tests for generated command/response lengths, ACK
   failures, short reads/writes, START/STOP and repeated START, and failure
   cleanup. Existing simulated NAND tests alone do not exercise the real
   LibMPSSE/D2XX bottleneck.
4. Benchmark small identical reads twice at 100 kHz and compare contents with
   the existing path. Then evaluate approximately 400 kHz separately, including
   a negative ACK control. Change batching and clock in separate steps so the
   cause of failures and speed improvements remains clear.
5. Validate a controlled write plus full readback using the optimized transport
   when a target range is selected for that test. Record page throughput and
   whole-command time separately. Keep optional backups and mandatory verify.

No production source or default clock changed during this investigation.

