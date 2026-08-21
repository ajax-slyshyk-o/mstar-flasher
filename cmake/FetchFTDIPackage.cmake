#[=======================================================================[.rst:
FetchFTDIPackage
-----------------

Helper to make an FTDI SDK zip (D2XX or LibMPSSE) available as an extracted
directory, for use by FindFTD2XX.cmake / FindLibMPSSE.cmake when the
matching *_ROOT variable is not already set.

FTDI's site sits behind a Cloudflare bot check, so a plain HTTP client
(including CMake's file(DOWNLOAD)) generally gets an HTML challenge page
instead of the zip - automated download from ftdichip.com is NOT reliable.
The supported workflow is therefore:

  1. Download the zip yourself, in a real browser, from FTDI's site:
       D2XX:      https://ftdichip.com/drivers/d2xx-drivers/
       LibMPSSE:  https://ftdichip.com/software-examples/mpsse-projects/libmpsse-i2c-examples/
     (Grab the plain driver/library zip, NOT a "Universal Driver" CDM
     installer package - that one ships headers/DLL only, no import lib.)
  2. Drop it into ${CMAKE_SOURCE_DIR}/archives/
  3. Re-run CMake configure - it will be found and extracted automatically.

If nothing matching is found in archives/, this falls back to downloading
FTD2XX_DOWNLOAD_URL / LIBMPSSE_DOWNLOAD_URL if one was given, on the chance
it isn't Cloudflare-gated (e.g. a mirror you host yourself).

mstar_fetch_ftdi_package(<name> <archive_glob> <url> <out_root_var>)

Extracts into ${CMAKE_BINARY_DIR}/_deps/<name>-src (skipped on repeat
configures once extracted) and sets <out_root_var> in the parent scope to
that directory.
#]=======================================================================]

function(mstar_fetch_ftdi_package name archive_glob url out_root_var)
    set(_src_dir "${CMAKE_BINARY_DIR}/_deps/${name}-src")
    set(_stamp_file "${_src_dir}/.mstar-fetched")

    if(EXISTS "${_stamp_file}")
        set("${out_root_var}" "${_src_dir}" PARENT_SCOPE)
        return()
    endif()

    set(_archives_dir "${CMAKE_SOURCE_DIR}/archives")
    file(GLOB _local_archives "${_archives_dir}/${archive_glob}")

    if(_local_archives)
        list(GET _local_archives 0 _archive)
        message(STATUS "${name}: using local archive ${_archive}")
    elseif(url)
        set(_download_dir "${CMAKE_BINARY_DIR}/_deps/${name}-download")
        file(MAKE_DIRECTORY "${_download_dir}")
        get_filename_component(_zip_name "${url}" NAME)
        if(NOT _zip_name MATCHES "\\.zip$")
            set(_zip_name "${name}.zip")
        endif()
        set(_archive "${_download_dir}/${_zip_name}")

        message(STATUS "${name}: no local archive matching '${archive_glob}' in "
            "${_archives_dir}, attempting download from ${url} (may fail if the "
            "host is bot-protected)")
        file(DOWNLOAD "${url}" "${_archive}" STATUS _dl_status SHOW_PROGRESS TLS_VERIFY ON)
        list(GET _dl_status 0 _dl_code)
        if(NOT _dl_code EQUAL 0)
            list(GET _dl_status 1 _dl_msg)
            file(REMOVE "${_archive}")
            message(FATAL_ERROR "${name}: download failed: ${_dl_msg}")
        endif()
    else()
        message(FATAL_ERROR
            "${name}: could not find an SDK.\n"
            "Either:\n"
            "  - download the package yourself from FTDI's site and place it in\n"
            "    ${_archives_dir}/ (matching pattern '${archive_glob}'), or\n"
            "  - set -D${name}_ROOT=<path to an already-extracted ${name} SDK>, or\n"
            "  - set -D${name}_DOWNLOAD_URL=<direct .zip URL> (only works if it isn't behind a bot check)\n"
            "FTDI D2XX drivers:    https://ftdichip.com/drivers/d2xx-drivers/\n"
            "FTDI LibMPSSE-I2C:    https://ftdichip.com/software-examples/mpsse-projects/libmpsse-i2c-examples/\n")
    endif()

    message(STATUS "${name}: extracting to ${_src_dir}")
    file(REMOVE_RECURSE "${_src_dir}")
    file(MAKE_DIRECTORY "${_src_dir}")
    file(ARCHIVE_EXTRACT
        INPUT "${_archive}"
        DESTINATION "${_src_dir}"
    )
    file(WRITE "${_stamp_file}" "")

    set("${out_root_var}" "${_src_dir}" PARENT_SCOPE)
endfunction()
