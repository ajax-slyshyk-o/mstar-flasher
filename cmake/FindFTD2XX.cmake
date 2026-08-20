#[=======================================================================[.rst:
FindFTD2XX
----------

Locates the FTDI D2XX driver library, needed by the FTDI backend
(MSTAR_ENABLE_FTDI). Not required for mstar-core or mstar-tests.

Honors the FTD2XX_ROOT variable/environment variable.

Defines the imported target ``FTD2XX::FTD2XX`` when found.
#]=======================================================================]

find_path(FTD2XX_INCLUDE_DIR
    NAMES ftd2xx.h
    HINTS "${FTD2XX_ROOT}"
    PATH_SUFFIXES include
)

find_library(FTD2XX_LIBRARY
    NAMES ftd2xx FTD2XX
    HINTS "${FTD2XX_ROOT}"
    PATH_SUFFIXES lib lib64
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FTD2XX
    REQUIRED_VARS FTD2XX_LIBRARY FTD2XX_INCLUDE_DIR
)

if(FTD2XX_FOUND AND NOT TARGET FTD2XX::FTD2XX)
    add_library(FTD2XX::FTD2XX UNKNOWN IMPORTED)
    set_target_properties(FTD2XX::FTD2XX PROPERTIES
        IMPORTED_LOCATION "${FTD2XX_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FTD2XX_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(FTD2XX_INCLUDE_DIR FTD2XX_LIBRARY)
