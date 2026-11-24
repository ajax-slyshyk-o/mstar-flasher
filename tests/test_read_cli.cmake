# Run the real CLI with a fake FTDI backend; never access an attached programmer.
file(MAKE_DIRECTORY "${TEST_ROOT}")

function(run_read name expected_exit)
    execute_process(COMMAND "${EXE}" read ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT "${result}" STREQUAL "${expected_exit}")
        message(FATAL_ERROR "${name}: expected exit ${expected_exit}, got ${result}\n${output}\n${error}")
    endif()
    if(NOT expected_exit EQUAL 0 AND output MATCHES "Read [0-9]+ bytes")
        message(FATAL_ERROR "${name}: failure printed a clean-read summary: ${output}")
    endif()
    set(stdout "${output}" PARENT_SCOPE)
    set(stderr "${error}" PARENT_SCOPE)
endfunction()

function(expect_error text)
    string(FIND "${stderr}" "${text}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Missing diagnostic '${text}': ${stderr}")
    endif()
endfunction()

function(expect_size path expected)
    file(SIZE "${path}" actual)
    if(NOT actual EQUAL expected)
        message(FATAL_ERROR "${path}: expected ${expected} bytes, got ${actual}")
    endif()
endfunction()

function(expect_hex path expected)
    file(READ "${path}" actual HEX)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "${path}: expected '${expected}', got '${actual}'")
    endif()
endfunction()

run_read(clean 0 "${TEST_ROOT}/clean.bin" --size 8)
expect_hex("${TEST_ROOT}/clean.bin" "0001020304050607")
if(NOT stdout MATCHES "Read 8 bytes")
    message(FATAL_ERROR "Missing clean-read summary: ${stdout}")
endif()

run_read(sliced 0 "${TEST_ROOT}/slice.bin" --offset 2046 --size 6)
expect_hex("${TEST_ROOT}/slice.bin" "feff11121314")

run_read(default-ecc 1 "${TEST_ROOT}/ecc.bin" --serial ecc --size 6144)
expect_size("${TEST_ROOT}/ecc.bin" 2048)
expect_error("Read failed at page 1")
expect_error("--fill-on-ecc-error")
expect_error("may be incomplete")

run_read(first-page-ecc 1 "${TEST_ROOT}/first-ecc.bin" --serial ecc --offset 2048 --size 4)
expect_size("${TEST_ROOT}/first-ecc.bin" 0)
expect_error("Read failed at page 1")

run_read(recovery 2 "${TEST_ROOT}/recovery.bin" --serial ecc --size 6144 --fill-on-ecc-error)
expect_size("${TEST_ROOT}/recovery.bin" 6144)
file(READ "${TEST_ROOT}/recovery.bin" filled OFFSET 2048 LIMIT 2048 HEX)
string(REPEAT "ff" 2048 expected_fill)
if(NOT filled STREQUAL expected_fill)
    message(FATAL_ERROR "Recovery did not replace exactly the bad page with 0xFF")
endif()
file(READ "${TEST_ROOT}/recovery.bin" next_page OFFSET 4096 LIMIT 4 HEX)
if(NOT next_page STREQUAL "22232425")
    message(FATAL_ERROR "Recovery did not continue reading the next good page")
endif()
expect_error("page 1 has uncorrectable ECC")
expect_error("1 page(s) filled with 0xFF")
expect_error("data was lost")

run_read(sliced-recovery 2 "${TEST_ROOT}/slice-recovery.bin" --serial ecc
    --offset 2046 --size 6 --fill-on-ecc-error)
expect_hex("${TEST_ROOT}/slice-recovery.bin" "feffffffffff")

run_read(multiple-ecc 2 "${TEST_ROOT}/multiple-ecc.bin" --serial ecc-all
    --size 4096 --fill-on-ecc-error)
expect_size("${TEST_ROOT}/multiple-ecc.bin" 4096)
expect_error("2 page(s) filled with 0xFF")

run_read(clean-recovery-option 0 "${TEST_ROOT}/clean-option.bin" --size 8 --fill-on-ecc-error)
expect_hex("${TEST_ROOT}/clean-option.bin" "0001020304050607")

run_read(transport 1 "${TEST_ROOT}/transport.bin" --serial transport --size 4096 --fill-on-ecc-error)
expect_size("${TEST_ROOT}/transport.bin" 2048)
expect_error("injected transport error")
expect_error("may be incomplete")

run_read(transport-after-ecc 1 "${TEST_ROOT}/transport-after-ecc.bin" --serial ecc-then-transport
    --size 6144 --fill-on-ecc-error)
expect_size("${TEST_ROOT}/transport-after-ecc.bin" 4096)
expect_error("page 1 has uncorrectable ECC")
expect_error("injected transport error")

# Invalid ranges must not truncate an existing file, including uint64 overflow.
file(WRITE "${TEST_ROOT}/preserved.bin" "keep me")
run_read(overflow 1 "${TEST_ROOT}/preserved.bin" --offset 8 --size 18446744073709551615)
expect_error("exceeds the flash size")
expect_hex("${TEST_ROOT}/preserved.bin" "6b656570206d65")
run_read(outside-flash 1 "${TEST_ROOT}/preserved.bin" --offset 268435457)
expect_error("beyond the end of the flash")
expect_hex("${TEST_ROOT}/preserved.bin" "6b656570206d65")

run_read(empty-at-end 0 "${TEST_ROOT}/empty.bin" --offset 268435456)
expect_size("${TEST_ROOT}/empty.bin" 0)

file(MAKE_DIRECTORY "${TEST_ROOT}/directory")
run_read(open-error 1 "${TEST_ROOT}/directory" --size 8)
expect_error("Failed to open output file")

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND EXISTS "/dev/full")
    # A small buffered write fails only on flush; a larger dump also exercises
    # the per-write check. /dev/full rejects writes without changing any file.
    run_read(flush-error 1 /dev/full --size 8)
    expect_error("Failed to flush output file")
    expect_error("may be incomplete")
    run_read(write-error 1 /dev/full --size 16384)
    expect_error("Failed to write output file")
    expect_error("may be incomplete")
endif()

run_read(trace 0 "${TEST_ROOT}/trace.bin" --size 8 --trace-i2c)
expect_hex("${TEST_ROOT}/trace.bin" "0001020304050607")
expect_error("I2C W 0x49: 4D 53 54 41 52 -> OK")
expect_error("I2C WR 0x49: 11 -> 2048 bytes")
expect_error("I2C W 0x49: 24 -> OK")
if(stdout MATCHES "I2C W")
    message(FATAL_ERROR "Protocol trace leaked onto stdout")
endif()

# Corrected data is retained, and threshold pages remain a subset of corrected.
run_read(mixed-ecc 0 "${TEST_ROOT}/mixed.bin" --serial mixed --size 6144)
expect_size("${TEST_ROOT}/mixed.bin" 6144)
expect_error("page 1 corrected (maximum 1..4 bit(s) per ECC step)")
expect_error("page 2 corrected (maximum 8..8 bit(s) per ECC step)")
expect_error("1 clean, 2 corrected (1 threshold/max-strength), 0 uncorrectable")

run_read(gv-corrected 0 "${TEST_ROOT}/gv.bin" --serial gv-corrected --size 8)
expect_hex("${TEST_ROOT}/gv.bin" "0001020304050607")
expect_error("maximum 1..1 bit(s)")
run_read(kv-corrected 0 "${TEST_ROOT}/kv.bin" --serial kv-corrected --size 8)
expect_error("maximum 3..3 bit(s)")
expect_error("0 clean, 1 corrected (0 threshold/max-strength)")
run_read(kv-threshold 0 "${TEST_ROOT}/kv-threshold.bin" --serial kv-threshold --size 8)
expect_error("0 clean, 1 corrected (1 threshold/max-strength)")
run_read(gm9-corrected 0 "${TEST_ROOT}/gm9.bin" --serial gm9-corrected --size 8)
expect_error("maximum 6..6 bit(s)")

# The recovery option must not hide invalid status, lost ECC, or transport errors.
run_read(gv-invalid 1 "${TEST_ROOT}/gv-invalid.bin" --serial gv-invalid --size 8 --fill-on-ecc-error)
expect_error("Invalid W25N01GV ECC status")
expect_size("${TEST_ROOT}/gv-invalid.bin" 0)
run_read(kv-invalid 1 "${TEST_ROOT}/kv-invalid.bin" --serial kv-invalid --size 8 --fill-on-ecc-error)
expect_error("Inconsistent W25N02KV ECC status")
expect_size("${TEST_ROOT}/kv-invalid.bin" 0)
run_read(extended-error 1 "${TEST_ROOT}/extended-error.bin" --serial extended-error --size 8 --fill-on-ecc-error)
expect_error("injected ECC detail read error")
expect_size("${TEST_ROOT}/extended-error.bin" 0)
run_read(config-lost 1 "${TEST_ROOT}/config-lost.bin" --serial config-lost --size 4096 --fill-on-ecc-error)
expect_error("ECC/read configuration changed")
expect_size("${TEST_ROOT}/config-lost.bin" 2048)

run_read(config-disabled 0 "${TEST_ROOT}/enabled.bin" --serial config-disabled --size 8 --trace-i2c)
expect_hex("${TEST_ROOT}/enabled.bin" "0001020304050607")
expect_error("I2C W 0x49: 10 1F B0 11 -> OK")
expect_error("I2C W 0x49: 10 1F B0 01 -> OK")
expect_error("I2C W 0x49: 24 -> OK")

# Setup failure must leave an existing output untouched and still clean up ISP.
run_read(config-ignored 1 "${TEST_ROOT}/preserved.bin" --serial config-ignored --size 8 --trace-i2c)
expect_error("Failed to enable/verify")
expect_error("I2C W 0x49: 24 -> OK")
expect_hex("${TEST_ROOT}/preserved.bin" "6b656570206d65")
run_read(config-write-error 1 "${TEST_ROOT}/preserved.bin" --serial config-write-error --size 8 --trace-i2c)
expect_error("injected configuration write error")
expect_error("I2C W 0x49: 10 1F B0 01 -> OK")
expect_error("I2C W 0x49: 24 -> OK")
expect_hex("${TEST_ROOT}/preserved.bin" "6b656570206d65")

# Restoring the flash is attempted on output-open failure, even with stay-in-isp.
run_read(config-open-error 1 "${TEST_ROOT}/directory" --serial config-disabled --size 8 --trace-i2c --stay-in-isp)
expect_error("Failed to open output file")
expect_error("I2C W 0x49: 10 1F B0 01 -> OK")
if(stderr MATCHES "I2C W 0x49: 24")
    message(FATAL_ERROR "--stay-in-isp unexpectedly released the target")
endif()

foreach(scenario restore-error restore-mismatch)
    run_read(${scenario} 1 "${TEST_ROOT}/${scenario}.bin" --serial ${scenario} --size 8 --trace-i2c)
    expect_hex("${TEST_ROOT}/${scenario}.bin" "0001020304050607")
    expect_error("Failed to restore flash read configuration")
    expect_error("I2C W 0x49: 24 -> OK")
endforeach()
