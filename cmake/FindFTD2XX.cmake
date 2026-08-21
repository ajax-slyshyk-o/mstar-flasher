#[=======================================================================[.rst:
FindFTD2XX
----------

Locates the FTDI D2XX driver library, needed by the FTDI backend
(MSTAR_ENABLE_FTDI). Not required for mstar-core or mstar-tests.

Honors the FTD2XX_ROOT variable/environment variable. If FTD2XX_ROOT is not
set and FTD2XX is not found on the system, it is extracted from a zip in
${CMAKE_SOURCE_DIR}/archives/ (see FetchFTDIPackage.cmake) - get the
package from https://ftdichip.com/drivers/d2xx-drivers/ (the plain D2XX
driver zip, e.g. "CDM-*-WHQL-Certified.zip" - NOT a "Universal Driver"
package, which ships no import library).

Expected layout (as shipped by FTDI's D2XX zip):
    ftd2xx.h
    amd64/ftd2xx.lib, amd64/FTD2XX64.dll
    i386/ftd2xx.lib,  i386/ftd2xx.dll

Defines the imported target ``FTD2XX::FTD2XX`` when found. It is a SHARED
imported target (import lib + runtime DLL) so its DLL is picked up by
generator expression $<TARGET_RUNTIME_DLLS:...> for post-build copying.
#]=======================================================================]

set(FTD2XX_DOWNLOAD_URL "" CACHE STRING
    "Direct .zip URL for FTDI's D2XX driver package, used when FTD2XX_ROOT is not set and no archive is found locally")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_ftd2xx_arch_suffixes amd64 lib/amd64 x64)
else()
    set(_ftd2xx_arch_suffixes i386 lib/i386 x86)
endif()

find_path(FTD2XX_INCLUDE_DIR
    NAMES ftd2xx.h
    HINTS "${FTD2XX_ROOT}" ENV FTD2XX_ROOT
    PATH_SUFFIXES "" include
)
find_library(FTD2XX_LIBRARY
    NAMES ftd2xx FTD2XX
    HINTS "${FTD2XX_ROOT}" ENV FTD2XX_ROOT
    PATH_SUFFIXES ${_ftd2xx_arch_suffixes} lib lib64
)

if((NOT FTD2XX_INCLUDE_DIR OR NOT FTD2XX_LIBRARY) AND NOT FTD2XX_ROOT)
    include("${CMAKE_CURRENT_LIST_DIR}/FetchFTDIPackage.cmake")
    mstar_fetch_ftdi_package(FTD2XX "CDM*.zip" "${FTD2XX_DOWNLOAD_URL}" FTD2XX_ROOT)

    find_path(FTD2XX_INCLUDE_DIR
        NAMES ftd2xx.h
        HINTS "${FTD2XX_ROOT}"
        PATH_SUFFIXES "" include
    )
    find_library(FTD2XX_LIBRARY
        NAMES ftd2xx FTD2XX
        HINTS "${FTD2XX_ROOT}"
        PATH_SUFFIXES ${_ftd2xx_arch_suffixes} lib lib64
    )
endif()

find_file(FTD2XX_DLL
    NAMES FTD2XX64.dll ftd2xx.dll
    HINTS "${FTD2XX_ROOT}" ENV FTD2XX_ROOT
    PATH_SUFFIXES ${_ftd2xx_arch_suffixes} bin
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FTD2XX
    REQUIRED_VARS FTD2XX_LIBRARY FTD2XX_INCLUDE_DIR
)

if(FTD2XX_FOUND AND NOT TARGET FTD2XX::FTD2XX)
    if(FTD2XX_DLL)
        add_library(FTD2XX::FTD2XX SHARED IMPORTED)
        set_target_properties(FTD2XX::FTD2XX PROPERTIES
            IMPORTED_LOCATION "${FTD2XX_DLL}"
            IMPORTED_IMPLIB "${FTD2XX_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${FTD2XX_INCLUDE_DIR}"
        )
    else()
        add_library(FTD2XX::FTD2XX UNKNOWN IMPORTED)
        set_target_properties(FTD2XX::FTD2XX PROPERTIES
            IMPORTED_LOCATION "${FTD2XX_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${FTD2XX_INCLUDE_DIR}"
        )
    endif()
endif()

mark_as_advanced(FTD2XX_INCLUDE_DIR FTD2XX_LIBRARY FTD2XX_DLL)
