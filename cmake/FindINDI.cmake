# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

find_path(INDI_INCLUDE_DIR
    NAMES indidevapi.h inditelescope.h
    HINTS ${INDI_ROOT} ENV INDI_ROOT
    PATH_SUFFIXES include/libindi libindi
)

find_library(INDI_DRIVER_LIBRARY
    NAMES indidriver
    HINTS ${INDI_ROOT} ENV INDI_ROOT
    PATH_SUFFIXES lib lib64
)

set(INDI_VERSION "")
if(INDI_INCLUDE_DIR AND EXISTS "${INDI_INCLUDE_DIR}/indiversion.h")
    file(READ "${INDI_INCLUDE_DIR}/indiversion.h" _INDI_VERSION_HEADER)
    if(_INDI_VERSION_HEADER MATCHES "INDI_VERSION[ \t]+\"?([0-9]+\\.[0-9]+\\.[0-9]+)")
        set(INDI_VERSION "${CMAKE_MATCH_1}")
    else()
        # Current INDI releases expose the library version as three numeric
        # preprocessor macros rather than one INDI_VERSION string.
        string(REGEX MATCH "#[ \t]*define[ \t]+INDI_VERSION_MAJOR[ \t]+([0-9]+)"
               _INDI_MAJOR_MATCH "${_INDI_VERSION_HEADER}")
        set(_INDI_VERSION_MAJOR "${CMAKE_MATCH_1}")
        string(REGEX MATCH "#[ \t]*define[ \t]+INDI_VERSION_MINOR[ \t]+([0-9]+)"
               _INDI_MINOR_MATCH "${_INDI_VERSION_HEADER}")
        set(_INDI_VERSION_MINOR "${CMAKE_MATCH_1}")
        string(REGEX MATCH "#[ \t]*define[ \t]+INDI_VERSION_RELEASE[ \t]+([0-9]+)"
               _INDI_RELEASE_MATCH "${_INDI_VERSION_HEADER}")
        set(_INDI_VERSION_RELEASE "${CMAKE_MATCH_1}")
        if(_INDI_MAJOR_MATCH AND _INDI_MINOR_MATCH AND _INDI_RELEASE_MATCH)
            set(INDI_VERSION
                "${_INDI_VERSION_MAJOR}.${_INDI_VERSION_MINOR}.${_INDI_VERSION_RELEASE}")
        endif()
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(INDI
    REQUIRED_VARS INDI_INCLUDE_DIR INDI_DRIVER_LIBRARY
    VERSION_VAR INDI_VERSION
)

if(INDI_FOUND)
    set(INDI_LIBRARIES "${INDI_DRIVER_LIBRARY}")
endif()

mark_as_advanced(INDI_INCLUDE_DIR INDI_DRIVER_LIBRARY)
