#[=======================================================================[.rst:
FindLibMPSSE
------------

Locates FTDI's LibMPSSE-I2C library, needed by the FTDI backend
(MSTAR_ENABLE_FTDI). Not required for mstar-core or mstar-tests.

Honors the LIBMPSSE_ROOT variable/environment variable.

Defines the imported target ``LibMPSSE::LibMPSSE`` when found.
#]=======================================================================]

find_path(LIBMPSSE_INCLUDE_DIR
    NAMES libMPSSE_i2c.h
    HINTS "${LIBMPSSE_ROOT}"
    PATH_SUFFIXES include
)

find_library(LIBMPSSE_LIBRARY
    NAMES MPSSE libMPSSE
    HINTS "${LIBMPSSE_ROOT}"
    PATH_SUFFIXES lib lib64
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibMPSSE
    REQUIRED_VARS LIBMPSSE_LIBRARY LIBMPSSE_INCLUDE_DIR
)

if(LibMPSSE_FOUND AND NOT TARGET LibMPSSE::LibMPSSE)
    add_library(LibMPSSE::LibMPSSE UNKNOWN IMPORTED)
    set_target_properties(LibMPSSE::LibMPSSE PROPERTIES
        IMPORTED_LOCATION "${LIBMPSSE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBMPSSE_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(LIBMPSSE_INCLUDE_DIR LIBMPSSE_LIBRARY)
