cmake_minimum_required(VERSION 3.28)

project(mstar-flasher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/EHsc)
endif()

option(MSTAR_BUILD_TESTS "Build unit tests" ON)
option(MSTAR_ENABLE_FTDI "Enable FTDI programmer backend" ON)
option(MSTAR_ENABLE_CH341 "Enable CH341 backend" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(CLI11 CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(toml11 CONFIG REQUIRED)
find_package(serdepp CONFIG REQUIRED)

# mstar-core: hardware-independent MStar ISP / SPI bus / flash logic.
# Must never link against D2XX, LibMPSSE or libusb.
add_library(mstar-core
    src/isp/mstar_isp.cpp
    src/flash/flash_table.cpp
    src/flash/spi_nand.cpp
    src/flash/spi_nand_write.cpp
    src/transport/tracing_i2c.cpp
)
target_include_directories(mstar-core PUBLIC include)
target_compile_features(mstar-core PUBLIC cxx_std_23)
target_link_libraries(mstar-core PUBLIC 
    fmt::fmt 
    toml11::toml11 
    nlohmann_json::nlohmann_json 
    serdepp::serdepp)

add_executable(mstar-cli src/app/main.cpp src/app/write_commands.cpp src/app/write_plan.cpp)
target_link_libraries(mstar-cli PRIVATE mstar-core CLI11::CLI11)

if(MSTAR_ENABLE_FTDI)
    add_library(mstar-ftdi)
    target_include_directories(mstar-ftdi PUBLIC include)
    target_link_libraries(mstar-ftdi PUBLIC mstar-core)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LIBFTDI REQUIRED IMPORTED_TARGET libftdi1>=1.5 libusb-1.0)
        target_sources(mstar-ftdi PRIVATE src/transport/ftdi/linux/ftdi_i2c.cpp)
        target_link_libraries(mstar-ftdi PRIVATE PkgConfig::LIBFTDI)
    else()
        find_package(FTD2XX REQUIRED)
        find_package(LibMPSSE REQUIRED)
        target_sources(mstar-ftdi PRIVATE
            src/transport/ftdi/ftdi_device.cpp
            src/transport/ftdi/ftdi_i2c.cpp
            src/transport/ftdi/ftdi_console.cpp)
        target_link_libraries(mstar-ftdi PRIVATE FTD2XX::FTD2XX LibMPSSE::LibMPSSE)
    endif()
    target_link_libraries(mstar-cli PRIVATE mstar-ftdi)
    target_compile_definitions(mstar-cli PRIVATE MSTAR_ENABLE_FTDI)

    if(WIN32)
        # $<TARGET_RUNTIME_DLLS:...> is only meaningful on DLL platforms;
        # ELF platforms resolve shared libs via RPATH instead.
        add_custom_command(TARGET mstar-cli POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_RUNTIME_DLLS:mstar-cli>"
                "$<TARGET_FILE_DIR:mstar-cli>"
            COMMAND_EXPAND_LISTS
        )
    endif()
endif()

if(MSTAR_ENABLE_CH341)
    message(FATAL_ERROR "MSTAR_ENABLE_CH341 is not implemented yet")
endif()

if(MSTAR_BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 CONFIG REQUIRED)

    add_executable(mstar-tests
        tests/test_mstar_isp.cpp
        tests/test_spi_nand.cpp
        tests/test_spi_nand_write.cpp
        tests/test_tracing_i2c.cpp
    )
    target_link_libraries(mstar-tests PRIVATE mstar-core Catch2::Catch2WithMain)
    target_include_directories(mstar-tests PRIVATE tests)

    add_test(NAME mstar-tests COMMAND mstar-tests)

    # Exercise D2XX/LibMPSSE ownership on any host without vendor SDKs or USB.
    add_executable(mstar-d2xx-tests tests/test_ftdi_d2xx.cpp
        src/transport/ftdi/ftdi_i2c.cpp)
    target_include_directories(mstar-d2xx-tests PRIVATE
        tests/fake_ftdi_sdk src/transport/ftdi)
    target_link_libraries(mstar-d2xx-tests PRIVATE mstar-core Catch2::Catch2WithMain)
    add_test(NAME mstar-d2xx-tests COMMAND mstar-d2xx-tests)

    # Exercise real CLI parsing, exit codes and file I/O with a fake programmer.
    # This target never links the hardware backend, even in FTDI-enabled builds.
    add_executable(mstar-cli-read-tests src/app/main.cpp src/app/write_commands.cpp src/app/write_plan.cpp tests/fake_ftdi_read.cpp)
    target_compile_definitions(mstar-cli-read-tests PRIVATE MSTAR_ENABLE_FTDI)
    target_link_libraries(mstar-cli-read-tests PRIVATE mstar-core CLI11::CLI11)
    add_test(NAME mstar-read-cli
        COMMAND "${CMAKE_COMMAND}"
            "-DEXE=$<TARGET_FILE:mstar-cli-read-tests>"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/read-cli-tests"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_read_cli.cmake")
    add_test(NAME mstar-write-cli
        COMMAND "${CMAKE_COMMAND}"
            "-DEXE=$<TARGET_FILE:mstar-cli-read-tests>"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/write-cli-tests"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_write_cli.cmake")

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME mstar-linux-setup
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_setup_linux.sh")
    endif()

    if(MSTAR_ENABLE_FTDI AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Fake C APIs exercise the real backend without linking any USB library.
        add_executable(mstar-linux-tests tests/test_ftdi_linux.cpp
            src/transport/ftdi/linux/ftdi_i2c.cpp)
        target_include_directories(mstar-linux-tests SYSTEM PRIVATE ${LIBFTDI_INCLUDE_DIRS})
        target_link_libraries(mstar-linux-tests PRIVATE mstar-core Catch2::Catch2WithMain)
        add_test(NAME mstar-linux-tests COMMAND mstar-linux-tests)
    endif()
endif()
