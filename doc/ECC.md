# SPI-NAND ECC handling

`read` uses an explicit ECC profile from the identified flash-table entry.
There is no vendor-wide fallback: an unknown profile or inconsistent strength
prevents reading. Hardware acceptance is still incomplete; these profiles have
been verified against documentation and tested offline, not validated on every
physical part. See [HARDWARE.md](HARDWARE.md).

## Profiles

`ECCS` below means status feature `C0h`, bits 5:4. All five profiles interpret
`00` as no correction and `10` as uncorrectable. They differ as follows:

| Part / ID | Correction strength | ECCS `01` | ECCS `11` | Required `B0h` bits |
| --- | --- | --- | --- | --- |
| GD5F2GM7UExxG / `C8 92` | 8 bits / 528 bytes | `F0h[5:4]` gives 1–4, 5, 6 or 7 | Corrected 8; at limit | ECC_EN, bit 4 |
| GD5F1GM9UEYIGR / `C8 91 01` | 8 bits / 528 bytes | `F0h[5:4]` gives 1–4, 5, 6 or 7 | Corrected 8; at limit | ECC_EN and NR, bits 4 and 3 |
| W25N01GV / `EF AA 21` | 1 bit / 512 bytes | Corrected 1; at limit | Invalid in buffer mode; abort | ECC-E and BUF, bits 4 and 3 |
| W25N01KV / `EF AE 21` | 4 bits / 512 bytes | Corrected; count from `30h[6:4]` | Corrected above device threshold | ECC-E and BUF, bits 4 and 3 |
| W25N02KV / `EF AA 22` | 8 bits / 512 bytes | Corrected; count from `30h[7:4]` | Corrected above device threshold | ECC-E and BUF, bits 4 and 3 |

Counts describe the maximum within an ECC step, not a total across the page.
The GigaDevice 1–4 bucket is reported as a range. Winbond KV counts must be
nonzero and within the part's strength when `ECCS` reports correction; otherwise
the read fails with a protocol error. Failure to read the extended status also
aborts; the implementation does not invent a count or silently assume clean data.
The Winbond threshold register is left unchanged.

## Configuration and cleanup

After identification and range validation, `configureRead(part)` reads `B0h`,
rejects active OTP access (bit 6), and enables missing ECC/read-mode bits using
read-modify-write. A changed setting is polled for readiness and read back.
Unrelated defined bits are preserved; GigaDevice reserved bits are written zero
as required by its tables. In particular, **GM7 bit 3 is a protection lock**:
it must not be set as though it were GM9's normal-read selector.

`readPage(page, buffer)` requires successful configuration, validates its range,
and rechecks ECC, read mode and OTP state before each page. It decodes the final
ready status and obtains extended ECC information before reading cache data.
The result contains `Clean`, `Corrected` or `Threshold` plus a bit-count range.
Uncorrectable pages return `EccUncorrectable`; invalid status and lost read
configuration return `FlashProtocol`. W25N01GV `11` describes failed continuous
reads and cannot be a valid status in the buffer mode required here.

The CLI restores only bits it changed before leaving ISP, even if setup or a
read fails, output cannot be opened, or `--stay-in-isp` was requested. Ownership
is recorded before the feature write because an unsuccessful transfer may still
reach the chip. Restoration uses read-modify-write and verifies readback. A
restoration failure makes the command fail, even if all output bytes were written.
Core callers must call `restoreReadConfiguration()` on every exit path; errors
are returned explicitly rather than hidden by a destructor. `reset()` restores
pending settings and invalidates preparation for subsequent page reads.

## CLI results

Corrected pages retain their data. Each correction reports its page and count
or range on stderr. A final summary counts clean, corrected and uncorrectable
pages; threshold/max-strength pages are a subset of corrected pages. This is
ECC read accounting, not a count of bytes successfully flushed to the output.

Exit codes remain 0 for a successful read, 1 for an aborted operation, and 2
for a completed recovery dump with substituted pages. `--fill-on-ecc-error`
continues only after confirmed uncorrectable ECC. It cannot bypass invalid
status, configuration failures, transport errors or output failures. Setup
failures occur before truncating the output file. Partial output is retained
after failures during reading.

No raw/ECC-off mode, OOB output, bad-block remapping, erased-page repair,
automatic read retry, SPI-NOR support, program or erase is added here. Those
require their own formats, policies and acceptance tests. ECC step size includes
metadata bytes on the GigaDevice parts. The table's physical OOB sizes are now
128 bytes for GM9 and 96 bytes for W25N01KV; this does not change main-area dumps.

## Sources and cross-checks

Manufacturer datasheets are linked through distributor mirrors where direct
manufacturer downloads require a login. Register tables, not another project's
fallback behavior, determine the profiles above.

- [GigaDevice GD5F2GM7, Rev. 1.5, July 2024](https://uploadcdn.oneyac.com/attachments/files/brand_pdf/gigadevice/CA/7B/GD5F2GM7REB2GR.pdf):
  features, section 12.1/table 12-1, section 12.2/table 12-3 and section 12.7.
- [GigaDevice GD5F1GM9, Rev. 1.0, May 2025](https://www.mouser.lt/pdfDocs/DS-01251-GD5F1GM9RE-Rev10.pdf):
  features, tables 6-1, 6-3 and 6-8; sections 6.2 and 18. The table in section 18
  describes encoded ECC status, so this implementation uses the explicit
  `C0h`/`F0h` tables rather than treating opcode `7Ch` as an exact bit count.
- [Winbond W25N01GV, Rev. R, July 2023](https://www.mouser.com/datasheet/2/949/Winbond_Electronics_09072023_W25N01GV_Rev_R_070323-3313374.pdf):
  sections 7.2.4, 7.2.7 and 7.3.2, including the restriction on status `11`.
- [Winbond W25N01KVxxxE, Rev. A, October 2023](https://www.mouser.com/catalog/specsheets/Winbond_W25N01KVxxxE_Datasheet_Rev.A.pdf):
  sections 7.2–7.4 and the 96-byte spare-area layout.
- [Winbond W25N02KVxxIE, Rev. M, March 2024](https://datasheet.lcsc.com/datasheet/pdf/4fb5cc7d78699a7b24dee01e55144be9.pdf?productCode=C22399242):
  sections 7.2–7.4, especially the `30h` maximum-bit-flip report.
- [Linux Winbond driver](https://github.com/torvalds/linux/blob/master/drivers/mtd/nand/spi/winbond.c):
  cross-check for strengths, buffer mode and the KV extended status register.
- User-provided local references under `../github/mstar/`:
  [OpenIPC/microsnander at d15a96f](https://github.com/OpenIPC/microsnander/tree/d15a96fcf840f0644ac7945e6a3e759b4c208dd2)
  and [McMCCRU/SNANDer at 5fbd17f](https://github.com/McMCCRU/SNANDer/tree/5fbd17ff0d7ca11d7abd85e0f163136b9f046380).
  Reviewed `src/spi_nand_flash.c` and microsnander's `tests/nandsim.c` for
  ECC setup/failure handling and part geometry. Microsnander has the corrected
  96-byte KV spare size; this SNANDer revision still lists 64 bytes. Neither
  reference replaces the datasheet-specific correction reporting above.

Offline tests cover every profile and primary status, extended counts/ranges,
unknown profiles, OTP rejection, configuration drift, setup/restore failures,
read failures at each transfer stage, and CLI data/exit-code behavior. Hardware
validation remains part of blueprint step 3.

Validation: Linux Debug and Release each pass all 5 CTest suites; the
FTDI-disabled build passes all 4 suites. No hardware commands or native Windows
build were run for this change.
