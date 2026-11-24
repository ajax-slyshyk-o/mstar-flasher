# SPI-NAND writing and verification

Writing is implemented for the five NAND profiles in [ECC.md](ECC.md), with
offline tests. No erase/program command has been run on the connected board.
The user's requested priority is to develop writing now and leave SPI-NOR for
later; incomplete bench acceptance does not prevent this software work.

## Commands

The first implementation accepts main-area images at fixed device addresses.
For `write`, both the image size and offset must be whole erase blocks (currently
128 KiB). `verify` requires page alignment (currently 2 KiB). Empty images,
out-of-range requests and unaligned requests are rejected. There is no implicit
padding, block skipping, remapping, or preservation of bytes outside a partial
block: partial blocks are rejected.

```sh
# Check the selected range and save/verify a backup, without erase/program:
./build/linux/mstar-cli write firmware.bin --serial ggggggggA \
    --offset 0 --backup before-test.raw --dry-run

# Back up to a NEW path, erase the image range, program and verify:
./build/linux/mstar-cli write firmware.bin --serial ggggggggA \
    --offset 0 --backup before-write.raw --yes

# Compare an existing main-area image with the device:
./build/linux/mstar-cli verify firmware.bin --serial ggggggggA --offset 0
```

`write` requires an explicit programmer serial and a new backup path. `--yes`
authorizes the destructive operation; `--dry-run` stops after preflight/backup,
even if `--yes` is also present. Both commands accept `--trace-i2c`,
`--i2c-address`, `--i2c-clock`, and `--stay-in-isp`. The last option omits ISP
exit but still restores flash settings. Success is exit 0; any failure is nonzero.
There is no standalone CLI `erase` or raw/OOB programming command yet.

## Write workflow

1. Load the image into memory so later file changes cannot alter the operation.
2. Identify a supported part and require a second matching JEDEC response.
3. Validate the range and configure ECC/per-page reads.
4. Check spare-area bad-block markers across the entire requested range. A
   marked or unreadable block aborts before any erase or program.
5. Read every source page with ECC checking, then save its raw main+OOB bytes.
   Finish and close the backup, reopen it, and compare it with a second raw read.
   A mismatch or any file/read error prevents programming. No ECC placeholders
   are permitted in this workflow.
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

`--backup PATH` creates two new files without overwriting existing files:

- `PATH`: headerless pages in ascending device-address order, each containing
  `page_size` raw main bytes followed by `physical_oob_size` raw spare bytes.
  ECC is disabled temporarily for each raw read and then restored/verified.
- `PATH.json`: format `mstar-raw-pages-v1`, part/JEDEC identity, main-area offset
  and size, page/spare sizes, pages per block, and the backup ECC mode.

The backup covers the image range, not the entire device. Its expected length
is `(main_area_size / page_size) * (page_size + physical_oob_size)`. It contains
physical spare/parity bytes and may contain raw bit errors that on-die ECC would
correct. It is **not a main-area image** and must not be passed directly to
`write`. Raw restoration or conversion requires a tool that understands this
layout; automatic restore is not implemented here.

Backups are flushed, closed and compared through file reads, but no platform
specific durable-storage sync is implemented. Keep the files and manifest on
reliable storage. Partial backups are retained after failure; their presence
alone does not establish a complete backup. A successful dry run reports that
the backup comparison finished. After a failed erase/program/verification,
flash may be partially changed; the original verified backup remains available.

Device addresses retain any pre-existing chip-internal bad-block links. This
implementation neither edits those links nor claims access to hidden/remapped
physical blocks. It does not infer that unmarked blocks are healthy: operation
status and readback must also succeed.

## Testing bad-block handling

Offline tests inject non-`FFh` spare markers on the first or second page, as well
as a marker in W25N01GV's second spare byte. They assert that an affected range
issues zero erase/program commands. Marker transport failures and failed ECC
restoration also abort; they are never interpreted as a good block.

The stateful NAND model additionally checks write-enable, erase-before-program,
cache replacement, 1-to-0 programming behavior, neighboring-block preservation,
status failures, busy timeouts, failed acknowledgements, and verification
mismatches. CLI tests run the real CLI/ISP code against this model, checking the
backup bytes/manifest, confirmation, alignment, error exits and cleanup.

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
