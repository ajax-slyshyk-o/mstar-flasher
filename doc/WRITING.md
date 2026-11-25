# SPI-NAND writing and verification

Writing is implemented for the five NAND profiles in [ECC.md](ECC.md), with
offline tests. No erase/program command has been run on the connected board.
Short marker scans have succeeded on the W25N02KV board, but a complete backup
dry run still encounters intermittent register/identification failures; see
[the hardware investigation](HARDWARE.md#config-write-investigation--2026-11-25).
The user's requested priority is to develop writing now and leave SPI-NOR for
later; incomplete bench acceptance does not prevent this software work.

## Commands

The first implementation accepts main-area images at fixed device addresses.
For single-image `write`, both the image size and offset must be whole erase blocks (currently
128 KiB). `verify` requires page alignment (currently 2 KiB). Empty images,
out-of-range requests and unaligned requests are rejected. There is no implicit
padding, block skipping, remapping, or preservation of bytes outside a partial
block: partial blocks are rejected.

```sh
# Check the selected range without erase/program or a backup:
./build/linux/mstar-cli write firmware.bin --serial ggggggggA --offset 0 --dry-run

# Erase, program and verify without a backup:
./build/linux/mstar-cli write firmware.bin --serial ggggggggA --offset 0

# Check the selected range and save/verify an optional backup, without erase/program:
./build/linux/mstar-cli write firmware.bin --serial ggggggggA \
    --offset 0 --backup before-test.raw --dry-run

# Back up to a NEW path, erase the image range, program and verify:
./build/linux/mstar-cli write firmware.bin --serial ggggggggA \
    --offset 0 --backup before-write.raw

# Compare an existing main-area image with the device:
./build/linux/mstar-cli verify firmware.bin --serial ggggggggA --offset 0
```

`write` uses the first detected programmer when `--serial` is omitted, including
with `--dry-run`. Supply `--serial` to select a specific programmer. `write`
erases/programs by default; `--yes` has been removed. `--dry-run` stops after
preflight and any requested backup without erasing/programming. `--backup PATH`
is optional in either mode and requires a new path when supplied. Omitting it
skips backup creation, comparison and their full-page reads. Both commands accept `--trace-i2c`,
`--i2c-address`, `--i2c-clock`, and `--stay-in-isp`. The last option omits ISP
exit but still restores flash settings. Success is exit 0; any failure is nonzero.
There is no standalone CLI `erase` or raw/OOB programming command yet.

Progress is flushed to stderr during marker checks, any backup creation and
comparison, preserved-tail reads, programming and image verification. Each stage reports completed
pages/blocks, percentage, the last completed device page/block, elapsed time
and an estimated remaining time for that stage. Updates appear after the first
completed item, then at most once per second and at stage completion. A blocked
transport call cannot produce further progress until it returns. Backup creation
reads each page through ECC and again as raw bytes; backup comparison performs
another raw read before any erase. These passes can be slow on Windows, where
the current LibMPSSE transfer mode exchanges USB data one byte at a time.

## Vendor multi-image configs

`write --config FILE` accepts the supplied `OnebinnandBurnImgConfig.cfg` format:

```text
cis.bin : 0x00000
cis.bin : 0x20000
IPL.cipher2.sig.bin : 0x140000
IPL_CUST.cipher2.sig.bin : 0x1A0000
u-boot_spinand.xz.img.sig.bin : 0x200000
```

```sh
./build/linux/mstar-cli write --config OnebinnandBurnImgConfig.cfg \
    --backup before-config.raw

# Preserve bytes after each file within its last erase block:
./build/linux/mstar-cli write --config OnebinnandBurnImgConfig.cfg \
    --preserve-gaps --backup before-preserved-write.raw
```

Add `--dry-run` to check the complete plan without erasing/programming.
The backup is saved and compared only when `--backup` is supplied. The config replaces the positional
image and `--offset`; combining them is an error. `--preserve-gaps` requires
`--config`. Programmer selection and other write options work as above.

Paths are resolved relative to the config's directory. Spaces, optionally
double-quoted paths and Windows backslash separators are accepted. Offsets are
decimal or `0x`/`0X` hexadecimal. Blank lines, full-line `#`/`;` comments,
CRLF and a UTF-8 BOM are accepted. Each other line must contain exactly a
filename and offset separated by the final colon; trailing comments are not
accepted. Repeating an image at different addresses is supported.

Config offsets must be erase-block aligned. File sizes need not be aligned:
by default, the remainder of each file's final block is filled with `0xFF`.
With `--preserve-gaps`, those main-area bytes are read with ECC and included in
the programmed data instead. Both policies leave **whole blocks between image
ranges untouched**. Neither policy preserves OOB metadata in rewritten blocks;
on-chip ECC parity is regenerated. Overlapping erase ranges are rejected,
including duplicate addresses and overlaps caused by rounding file sizes up.

All files are loaded before opening the programmer. After identification, all
ranges are validated, sorted by address and checked for bad-block markers
before the first erase. Requested backups are saved and compared, and all
preserved tails are read, before any erase. Without a backup, `--preserve-gaps`
reads only pages containing preserved tail bytes. The write verifies every byte in all
affected blocks, including filled or preserved tails. A failure stops the batch;
previously programmed blocks are not rolled back. Config syntax compatibility
does not add vendor-specific CIS generation, bad-block remapping or OOB formats.

## Write workflow

1. Load the image into memory so later file changes cannot alter the operation.
2. Identify a supported part and require a second matching JEDEC response.
3. Validate the range and configure ECC/per-page reads.
4. Check spare-area bad-block markers across the entire requested range. A
   marked or unreadable block aborts before any erase or program.
5. When `--backup` is supplied, read every source page with ECC checking, then
   save its raw main+OOB bytes. Finish and close the backup, reopen it, and
   compare it with a second raw read. A mismatch or any file/read error prevents
   programming. Without backup, skip these full-page passes. With
   `--preserve-gaps`, read all required tail pages through ECC before any erase.
   No ECC placeholders are permitted in this workflow.
6. Unless this is a dry run, temporarily clear volatile block-protection bits
   and verify the change. Hardware protection and OTP locks are not bypassed.
7. Recheck each block's markers, erase it, and program full pages in ascending
   order. Check WEL before mutation and E-FAIL/P-FAIL after completion. Do not
   retry failed erase/program commands: the device may already have acted.
8. Read back the complete image range through ECC and compare every byte.
9. Disable writes, restore changed protection/ECC/read-mode settings, then exit
   ISP unless requested otherwise. Cleanup failures make the command fail.

`02h` loads replace the cache, leaving unloaded spare bytes at `FFh`; internal
ECC generates new parity. Existing user OOB metadata is **not** preserved by a
main-area image write. This format is unsuitable for an image that requires
explicit OOB metadata or a different bad-block mapping policy.

## Backup format and failures

The optional `--backup PATH` creates two new files without overwriting existing files:

- `PATH`: headerless pages in ascending device-address order, each containing
  `page_size` raw main bytes followed by `physical_oob_size` raw spare bytes.
  ECC is disabled temporarily for each raw read and then restored/verified.
- `PATH.json` for single-image writes: format `mstar-raw-pages-v1`, part/JEDEC identity, main-area offset
  and size, page/spare sizes, pages per block, and the backup ECC mode.

Config writes use manifest format `mstar-raw-ranges-v2`. The raw file contains
only affected blocks, concatenated in ascending device-address order. The
manifest includes the same part/geometry fields, `config`, `tail_policy`
(`fill-ff` or `preserve`) and a `ranges` array. Each entry records `image`,
`image_size` (original bytes), `main_area_offset`, `main_area_size` (whole
blocks), `backup_offset` and `backup_size` (byte offsets/sizes in the raw file).
Do not interpret this sparse backup as one contiguous device range.

The backup covers the image range, not the entire device. Its expected length
is `(main_area_size / page_size) * (page_size + physical_oob_size)`. It contains
physical spare/parity bytes and may contain raw bit errors that on-die ECC would
correct. It is **not a main-area image** and must not be passed directly to
`write`. Raw restoration or conversion requires a tool that understands this
layout; automatic restore is not implemented here.

Backups are flushed, closed and compared through file reads, but no platform
specific durable-storage sync is implemented. Keep the files and manifest on
reliable storage. Partial backups are retained after failure; their presence
alone does not establish a complete backup. A successful dry run with
`--backup` reports that the backup comparison finished. After a failed
erase/program/verification, flash may be partially changed. A requested,
verified backup remains available; writes without `--backup` create no backup.

Device addresses retain any pre-existing chip-internal bad-block links. This
implementation neither edits those links nor claims access to hidden/remapped
physical blocks. It does not infer that unmarked blocks are healthy: operation
status and readback must also succeed.

Raw-read setup/restoration failures report `B0` register values. Unexpected
mode/lock bits abort without being copied into a feature-register write. Raw
cleanup verifies all defined configuration bits, not only ECC, and retains
pending restoration on failure. These checks do not suppress or automatically
retry an unreliable connection.

## Testing bad-block handling

Offline tests inject non-`FFh` spare markers on the first or second page, as well
as a marker in W25N01GV's second spare byte. They assert that an affected range
issues zero erase/program commands. Marker transport failures and failed ECC
restoration also abort; they are never interpreted as a good block.

The stateful NAND model additionally checks write-enable, erase-before-program,
cache replacement, 1-to-0 programming behavior, neighboring-block preservation,
status failures, busy timeouts, failed acknowledgements, and verification
mismatches. CLI tests run the real CLI/ISP code against this model, checking the
optional backup bytes/manifest, alignment, error exits and cleanup. Tests also
check that omitted backups cause no full raw reads, dry runs issue no mutations,
removed `--yes` is rejected, and tail preservation works without a backup.
Config tests also inspect the model's final bytes independently: repeated
images, partial pages, multi-block files, both tail policies and untouched
gaps. Missing/malformed inputs, overlaps, bad markers in later ranges and late
backup read failures abort before any erase. The supplied five-entry config
and its actual files were exercised with simulated programming/readback;
that is not a hardware or vendor-tool equivalence test.

For hardware, first inspect existing markers using a reference tool and compare
the result with our write dry run over that range. The dry run reports the first
marked block and stops without erasing. Test an unmarked range separately. A
device with no known marked blocks exercises only the good-block path; it does
not validate detection of a real bad block. No deliberate wear-out or factory
marker modification is required. A damaged block discovered during programming
is reported as a failure; automatic marking/remapping is not implemented.

Spare markers are read with ECC off on the first two pages. Manufacturer factory
markers are on the first page; checking the second also respects common
software-marked blocks. The first spare byte is checked for each profile, plus
the second byte for W25N01GV. Any non-`FFh` value rejects the block. This is
conservative for layouts that repurpose those bytes. Main-area byte 0 is not
used as a marker because a used good block can contain ordinary data there.

## References

Use the manufacturer datasheets linked in [ECC.md](ECC.md): GM7 sections 7,
9–10 and 12.4; GM9 sections 8, 11–12 and 15; Winbond GV sections 8.2.5–8.2.15;
Winbond KV program/erase instructions and initial-invalid-block sections.
The commands are WREN `06h`, WRDI `04h`, load `02h`, execute `10h`, erase `D8h`.
GigaDevice uses load then WREN; Winbond uses WREN then load. Block protection is
handled per family, preserving register locks and hardware write-protect control.

The supplied [microsnander](https://github.com/OpenIPC/microsnander/tree/d15a96fcf840f0644ac7945e6a3e759b4c208dd2)
reference was reviewed for program ordering and raw marker inspection. No large
code sections were ported. The software tests do not establish electrical
correctness or replace an eventual controlled write/readback test on the board.
