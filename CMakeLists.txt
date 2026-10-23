# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

cmake_minimum_required(VERSION 3.16)
project(indi_vixen_wireless VERSION 0.1.138 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(vixen_wireless_protocol
    src/client.cpp
    src/discovery.cpp
    src/protocol.cpp
    src/simulator_transport.cpp
    src/transport.cpp
)
target_include_directories(vixen_wireless_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(vixen_wireless_protocol PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

option(VIXEN_BUILD_INDI_DRIVER "Require INDI and build the INDI telescope driver" ON)
set(VIXEN_EXPECTED_INDI_PACKAGE_VERSION "2.2.3.1" CACHE STRING
    "Target INDI package release (the INDI public header exposes only 2.2.3)")

if(VIXEN_BUILD_INDI_DRIVER)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    find_package(INDI 2.2.3 REQUIRED)
    add_executable(indi_vixen_wirelessunit
        src/indi_vixen_wireless.cpp
    )
    target_include_directories(indi_vixen_wirelessunit PRIVATE
        ${INDI_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(indi_vixen_wirelessunit PRIVATE
        vixen_wireless_protocol
        ${INDI_LIBRARIES}
    )
    target_compile_options(indi_vixen_wirelessunit PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wformat=2
        -Werror=format-security
    )
    target_compile_definitions(indi_vixen_wirelessunit PRIVATE VIXEN_DRIVER_VERSION="${PROJECT_VERSION}")

    configure_file(indi_vixen_wirelessunit.xml.in indi_vixen_wirelessunit.xml @ONLY)
    install(TARGETS indi_vixen_wirelessunit RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/indi_vixen_wirelessunit.xml"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/indi")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/COPYING"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/indi-vixen-wirelessunit")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/indi_vixen_wirelessunit.1"
            DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)

    function(vixen_register_cpp_test name)
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES TIMEOUT 20)
    endfunction()

    add_executable(protocol_test tests/protocol_test.cpp)
    target_link_libraries(protocol_test PRIVATE vixen_wireless_protocol)
    vixen_register_cpp_test(protocol_test)

    add_executable(client_test tests/client_test.cpp)
    target_link_libraries(client_test PRIVATE vixen_wireless_protocol)
    vixen_register_cpp_test(client_test)

    add_executable(transport_test tests/transport_test.cpp)
    target_link_libraries(transport_test PRIVATE vixen_wireless_protocol Threads::Threads)
    vixen_register_cpp_test(transport_test)

    add_executable(discovery_test tests/discovery_test.cpp)
    target_link_libraries(discovery_test PRIVATE vixen_wireless_protocol Threads::Threads)
    vixen_register_cpp_test(discovery_test)

    add_executable(wifi_ap_update_test tests/wifi_ap_update_test.cpp)
    target_link_libraries(wifi_ap_update_test PRIVATE vixen_wireless_protocol)
    vixen_register_cpp_test(wifi_ap_update_test)

    add_executable(simulator_test tests/simulator_test.cpp)
    target_link_libraries(simulator_test PRIVATE vixen_wireless_protocol)
    vixen_register_cpp_test(simulator_test)

    add_executable(site_set_test tests/site_set_test.cpp)
    target_link_libraries(site_set_test PRIVATE vixen_wireless_protocol)
    vixen_register_cpp_test(site_set_test)

    file(GLOB python_test_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.py")
    if(NOT python_test_files)
        message(FATAL_ERROR "BUILD_TESTING is ON but no Python tests were found")
    endif()
    foreach(python_test_file IN LISTS python_test_files)
        get_filename_component(python_test_name "${python_test_file}" NAME_WE)
        add_test(NAME python_${python_test_name}
                 COMMAND ${Python3_EXECUTABLE}
                         "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_python_tests.py"
                         "${python_test_file}")
        set_tests_properties(python_${python_test_name} PROPERTIES TIMEOUT 20)
    endforeach()

    if(VIXEN_BUILD_INDI_DRIVER)
        find_program(INDISERVER_EXECUTABLE indiserver)
        if(NOT INDISERVER_EXECUTABLE)
            message(FATAL_ERROR "Driver XML tests require indiserver")
        endif()
        add_test(NAME driver_xml_simulation_test
                 COMMAND ${Python3_EXECUTABLE}
                         "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_xml_simulation_test.py"
                         --driver "$<TARGET_FILE:indi_vixen_wirelessunit>"
                         --indiserver "${INDISERVER_EXECUTABLE}"
                         --expected-version "${PROJECT_VERSION}")
        set_tests_properties(driver_xml_simulation_test PROPERTIES TIMEOUT 40)
    endif()
endif()
