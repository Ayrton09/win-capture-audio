cmake_minimum_required(VERSION 3.24)

project(win-capture-audio VERSION 2.3.0 LANGUAGES CXX)

set(PLUGIN_AUTHOR "bozbez")
set(PLUGIN_URL "https://github.com/Ayrton09/win-capture-audio")

if(NOT WIN32)
    message(FATAL_ERROR "win-capture-audio is a Windows-only plugin.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Only 64-bit builds are supported. OBS Studio has not shipped a 32-bit "
                        "Windows build since version 27.")
endif()

set(ARCH_NAME "64bit")
set(RELEASE_DIR "${CMAKE_BINARY_DIR}/release")

# ---------------------------------------------------------------------------------
# libobs
#
# OBS Studio's Windows installer ships no development files, so `find_package(libobs)`
# only works for people who have built OBS themselves and point CMAKE_PREFIX_PATH at
# it. Everyone else gets a minimal SDK assembled from the OBS build already installed
# on this machine (see cmake/bootstrap-obs-sdk.ps1).
# ---------------------------------------------------------------------------------

find_package(libobs CONFIG QUIET)

if(TARGET OBS::libobs)
    message(STATUS "libobs: using the CMake package found at ${libobs_DIR}")
else()
    set(OBS_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/libobs-sdk"
        CACHE PATH "Directory holding the generated libobs SDK (headers + import library)")
    set(OBS_INSTALL_DIR "" CACHE PATH "Root of the OBS Studio installation (auto-detected when empty)")
    set(OBS_VERSION "" CACHE STRING "obs-studio tag to take headers from (auto-detected when empty)")

    set(_bootstrap_args
        -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bootstrap-obs-sdk.ps1"
        -OutDir "${OBS_SDK_DIR}")

    if(OBS_INSTALL_DIR)
        list(APPEND _bootstrap_args -ObsDir "${OBS_INSTALL_DIR}")
    endif()
    if(OBS_VERSION)
        list(APPEND _bootstrap_args -ObsVersion "${OBS_VERSION}")
    endif()

    message(STATUS "libobs: assembling SDK from the installed OBS Studio build")
    execute_process(
        COMMAND powershell ${_bootstrap_args}
        RESULT_VARIABLE _bootstrap_result
        OUTPUT_VARIABLE _bootstrap_output
        ERROR_VARIABLE _bootstrap_output)

    if(NOT _bootstrap_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to assemble the libobs SDK.\n${_bootstrap_output}\n"
            "Either install OBS Studio, pass -DOBS_INSTALL_DIR=<path>, or point "
            "-DCMAKE_PREFIX_PATH at a libobs build of your own.")
    endif()

    string(STRIP "${_bootstrap_output}" _bootstrap_output)
    message(STATUS "${_bootstrap_output}")

    if(NOT EXISTS "${OBS_SDK_DIR}/lib/obs.lib" OR NOT EXISTS "${OBS_SDK_DIR}/include/obs.h")
        message(FATAL_ERROR "The libobs SDK at ${OBS_SDK_DIR} is incomplete.")
    endif()

    add_library(OBS::libobs SHARED IMPORTED)
    set_target_properties(OBS::libobs PROPERTIES
        IMPORTED_IMPLIB "${OBS_SDK_DIR}/lib/obs.lib"
        IMPORTED_LOCATION "${OBS_SDK_DIR}/lib/obs.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${OBS_SDK_DIR}/include"
        INTERFACE_COMPILE_DEFINITIONS "HAVE_OBSCONFIG_H")
endif()

# ---------------------------------------------------------------------------------
# Windows Implementation Library (header-only)
# ---------------------------------------------------------------------------------

set(WIL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/wil/include"
    CACHE PATH "Include directory of microsoft/wil")

if(NOT EXISTS "${WIL_INCLUDE_DIR}/wil/resource.h")
    message(FATAL_ERROR
        "microsoft/wil not found at ${WIL_INCLUDE_DIR}.\n"
        "Run: git submodule update --init --recursive\n"
        "  or: git clone --depth 1 https://github.com/microsoft/wil.git deps/wil")
endif()

# ---------------------------------------------------------------------------------
# Version header
# ---------------------------------------------------------------------------------

set(GIT_HASH "unknown")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" log -1 --format=%h
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT GIT_HASH)
        set(GIT_HASH "unknown")
    endif()
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin-macros.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/plugin-macros.generated.hpp"
    @ONLY)

# ---------------------------------------------------------------------------------
# The plugin
# ---------------------------------------------------------------------------------

add_library(win-capture-audio MODULE
    src/plugin.cpp
    src/audio-capture.cpp
    src/audio-capture-helper.cpp
    src/session-monitor.cpp
    src/mixer.cpp)

target_include_directories(win-capture-audio PRIVATE
    "${WIL_INCLUDE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_link_libraries(win-capture-audio PRIVATE
    OBS::libobs
    user32      # PostThreadMessage / GetMessage / window queries
    ole32       # COM
    mmdevapi    # ActivateAudioInterfaceAsync
    ksuser      # KSDATAFORMAT_SUBTYPE_* GUIDs
    psapi)      # GetProcessImageFileNameW

target_compile_features(win-capture-audio PRIVATE cxx_std_20)

target_compile_definitions(win-capture-audio PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE)

if(MSVC)
    target_compile_options(win-capture-audio PRIVATE /utf-8 /W3 /permissive-)
endif()

set_target_properties(win-capture-audio PROPERTIES PREFIX "")

# ---------------------------------------------------------------------------------
# Staging + install
#
# OBS 32 on Windows discovers third-party plugins under
#   %ProgramData%/obs-studio/plugins/<name>/bin/64bit/<name>.dll
#   %ProgramData%/obs-studio/plugins/<name>/data/...
# (frontend/widgets/OBSBasic.cpp -> AddExtraModulePaths -> GetProgramDataPath).
#
# The legacy layout inside the OBS installation directory
#   <obs>/obs-plugins/64bit/<name>.dll  +  <obs>/data/obs-plugins/<name>/...
# still works and is what the Inno Setup installer produces.
# ---------------------------------------------------------------------------------

# Staged modern plugin layout: consumed by the Inno Setup installer and zipped
# as the portable release artifact.
add_custom_command(TARGET win-capture-audio POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${RELEASE_DIR}/bin/${ARCH_NAME}"
        "${RELEASE_DIR}/data"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/data"
        "${RELEASE_DIR}/data"
    COMMAND "${CMAKE_COMMAND}" -E copy
        "$<TARGET_FILE:win-capture-audio>"
        "${RELEASE_DIR}/bin/${ARCH_NAME}"
    VERBATIM)

if(NOT DEFINED CMAKE_INSTALL_PREFIX OR CMAKE_INSTALL_PREFIX STREQUAL "")
    set(CMAKE_INSTALL_PREFIX "$ENV{ProgramData}/obs-studio/plugins/${PROJECT_NAME}" CACHE PATH "" FORCE)
elseif(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{ProgramData}/obs-studio/plugins/${PROJECT_NAME}" CACHE PATH "" FORCE)
endif()

install(TARGETS win-capture-audio LIBRARY DESTINATION "bin/${ARCH_NAME}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/" DESTINATION "data")

if(MSVC)
    install(FILES "$<TARGET_PDB_FILE:win-capture-audio>"
        DESTINATION "bin/${ARCH_NAME}" OPTIONAL)
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/installer/installer.iss.in"
    "${CMAKE_CURRENT_BINARY_DIR}/installer.generated.iss"
    @ONLY)

message(STATUS "win-capture-audio ${PROJECT_VERSION} (${GIT_HASH})")
message(STATUS "  install prefix: ${CMAKE_INSTALL_PREFIX}")
