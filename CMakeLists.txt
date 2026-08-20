cmake_minimum_required(VERSION 3.25)

project(mstar-flasher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MSTAR_BUILD_TESTS "Build unit tests" ON)
option(MSTAR_ENABLE_FTDI "Enable FTDI programmer backend" OFF)
option(MSTAR_ENABLE_CH341 "Enable CH341 backend" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(CLI11 CONFIG REQUIRED)

# mstar-core: hardware-independent MStar ISP / SPI bus / flash logic.
# Must never link against D2XX, LibMPSSE or libusb.
add_library(mstar-core
    src/isp/mstar_isp.cpp
)
target_include_directories(mstar-core PUBLIC include)
target_compile_features(mstar-core PUBLIC cxx_std_23)

add_executable(mstar-cli src/app/main.cpp)
target_link_libraries(mstar-cli PRIVATE mstar-core CLI11::CLI11)

if(MSTAR_ENABLE_FTDI)
    find_package(FTD2XX REQUIRED)
    find_package(LibMPSSE REQUIRED)

    add_library(mstar-ftdi
        src/transport/ftdi/ftdi_device.cpp
        src/transport/ftdi/ftdi_i2c.cpp
        src/transport/ftdi/ftdi_console.cpp
    )
    target_include_directories(mstar-ftdi PUBLIC include)
    target_link_libraries(mstar-ftdi
        PUBLIC mstar-core
        PRIVATE FTD2XX::FTD2XX LibMPSSE::LibMPSSE
    )
    target_link_libraries(mstar-cli PRIVATE mstar-ftdi)
endif()

if(MSTAR_ENABLE_CH341)
    message(FATAL_ERROR "MSTAR_ENABLE_CH341 is not implemented yet")
endif()

if(MSTAR_BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 CONFIG REQUIRED)

    add_executable(mstar-tests
        tests/test_mstar_isp.cpp
    )
    target_link_libraries(mstar-tests PRIVATE mstar-core Catch2::Catch2WithMain)
    target_include_directories(mstar-tests PRIVATE tests)

    add_test(NAME mstar-tests COMMAND mstar-tests)
endif()
