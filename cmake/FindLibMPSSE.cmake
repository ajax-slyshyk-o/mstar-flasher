#[=======================================================================[.rst:
FindLibMPSSE
------------

Locates FTDI's LibMPSSE-I2C library, needed by the FTDI backend
(MSTAR_ENABLE_FTDI). Not required for mstar-core or mstar-tests.

Honors the LIBMPSSE_ROOT variable/environment variable. If LIBMPSSE_ROOT is
not set and LibMPSSE is not found on the system, it is extracted from a zip
in ${CMAKE_SOURCE_DIR}/archives/ (see FetchFTDIPackage.cmake) - get the
package from
https://ftdichip.com/software-examples/mpsse-projects/libmpsse-i2c-examples/
(the "libmpsse-windows-*.zip" release package).

Expected layout (as shipped by FTDI's libmpsse-windows zip):
    release/include/libmpsse_i2c.h
    release/build/x64/DLL/libmpsse.lib,   release/build/x64/DLL/libmpsse.dll
    release/build/Win32/DLL/libmpsse.lib, release/build/Win32/DLL/libmpsse.dll

Defines the imported target ``LibMPSSE::LibMPSSE`` when found. It is a
SHARED imported target (import lib + runtime DLL) so its DLL is picked up
by generator expression $<TARGET_RUNTIME_DLLS:...> for post-build copying.
#]=======================================================================]

set(LIBMPSSE_DOWNLOAD_URL "" CACHE STRING
    "Direct .zip URL for FTDI's LibMPSSE-I2C package, used when LIBMPSSE_ROOT is not set and no archive is found locally")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_libmpsse_arch_suffixes release/build/x64/DLL release/build/x64/LIB amd64)
else()
    set(_libmpsse_arch_suffixes release/build/Win32/DLL release/build/Win32/LIB i386)
endif()

find_path(LIBMPSSE_INCLUDE_DIR
    NAMES libMPSSE_i2c.h libmpsse_i2c.h
    HINTS "${LIBMPSSE_ROOT}" ENV LIBMPSSE_ROOT
    PATH_SUFFIXES release/include include
)
find_library(LIBMPSSE_LIBRARY
    NAMES MPSSE libMPSSE libmpsse
    HINTS "${LIBMPSSE_ROOT}" ENV LIBMPSSE_ROOT
    PATH_SUFFIXES ${_libmpsse_arch_suffixes} lib lib64
)

if((NOT LIBMPSSE_INCLUDE_DIR OR NOT LIBMPSSE_LIBRARY) AND NOT LIBMPSSE_ROOT)
    include("${CMAKE_CURRENT_LIST_DIR}/FetchFTDIPackage.cmake")
    mstar_fetch_ftdi_package(LIBMPSSE "libmpsse*.zip" "${LIBMPSSE_DOWNLOAD_URL}" LIBMPSSE_ROOT)

    find_path(LIBMPSSE_INCLUDE_DIR
        NAMES libMPSSE_i2c.h libmpsse_i2c.h
        HINTS "${LIBMPSSE_ROOT}"
        PATH_SUFFIXES release/include include
    )
    find_library(LIBMPSSE_LIBRARY
        NAMES MPSSE libMPSSE libmpsse
        HINTS "${LIBMPSSE_ROOT}"
        PATH_SUFFIXES ${_libmpsse_arch_suffixes} lib lib64
    )
endif()

find_file(LIBMPSSE_DLL
    NAMES libmpsse.dll MPSSE.dll
    HINTS "${LIBMPSSE_ROOT}" ENV LIBMPSSE_ROOT
    PATH_SUFFIXES ${_libmpsse_arch_suffixes} bin
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibMPSSE
    REQUIRED_VARS LIBMPSSE_LIBRARY LIBMPSSE_INCLUDE_DIR
)

if(LibMPSSE_FOUND AND NOT TARGET LibMPSSE::LibMPSSE)
    if(LIBMPSSE_DLL)
        add_library(LibMPSSE::LibMPSSE SHARED IMPORTED)
        set_target_properties(LibMPSSE::LibMPSSE PROPERTIES
            IMPORTED_LOCATION "${LIBMPSSE_DLL}"
            IMPORTED_IMPLIB "${LIBMPSSE_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBMPSSE_INCLUDE_DIR}"
        )
    else()
        add_library(LibMPSSE::LibMPSSE UNKNOWN IMPORTED)
        set_target_properties(LibMPSSE::LibMPSSE PROPERTIES
            IMPORTED_LOCATION "${LIBMPSSE_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBMPSSE_INCLUDE_DIR}"
        )
    endif()
endif()

mark_as_advanced(LIBMPSSE_INCLUDE_DIR LIBMPSSE_LIBRARY LIBMPSSE_DLL)
