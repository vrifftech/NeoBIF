cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime library" )
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cmake/NeoSharedSource.cmake")

if(NOT EXISTS "${NEOSHARED_ROOT}/include/neoshared/ResourceDocument.hpp" OR
   NOT EXISTS "${NEOSHARED_ROOT}/wx/NeoModulePanel.hpp")
    message(FATAL_ERROR "NeoBIF 1.0.0 requires the matched NeoShared resource/panel API update. Publish the complete current NeoShared source; CI selects main automatically.")
endif()

include("${NEOSHARED_ROOT}/cmake/NeoVersion.cmake")
include("${NEOSHARED_ROOT}/cmake/NeoMacOSBundle.cmake")
include("${NEOSHARED_ROOT}/cmake/NeoWasm.cmake")
neo_read_version_header(NEOBIF_VERSION
    "${CMAKE_CURRENT_LIST_DIR}/src/core/Version.hpp"
    NEOBIF_VERSION_STRING)
project(NeoBIFCpp VERSION ${NEOBIF_VERSION} DESCRIPTION "C++17/wxWidgets KotOR game archive browser and extractor" LANGUAGES CXX)

if(MSVC AND NOT NEO_HOSTED_BUILD)
    foreach(_neo_runtime_flag_var IN ITEMS
        CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_MINSIZEREL
        CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
        if(DEFINED ${_neo_runtime_flag_var})
            string(REGEX REPLACE "(^|[ /-])MDd?($|[ ]|/)" "\\1MT\\2" ${_neo_runtime_flag_var} "${${_neo_runtime_flag_var}}")
        endif()
    endforeach()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(neo_force_static_msvc_runtime target_name)
    if(MSVC AND TARGET ${target_name} AND NOT NEO_HOSTED_BUILD)
        set_property(TARGET ${target_name} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Debug>:/MTd>
            $<$<NOT:$<CONFIG:Debug>>:/MT>
        )
    endif()
endfunction()

function(neo_force_static_msvc_runtime_for_directory_targets)
    if(MSVC)
        get_property(_neo_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_neo_target IN LISTS _neo_targets)
            get_target_property(_neo_type ${_neo_target} TYPE)
            if(NOT _neo_type STREQUAL "INTERFACE_LIBRARY" AND NOT _neo_type STREQUAL "UTILITY")
                neo_force_static_msvc_runtime(${_neo_target})
            endif()
        endforeach()
    endif()
endfunction()

include(GNUInstallDirs)

message(STATUS "Using neoshared: ${NEOSHARED_ROOT}")
include("${NEOSHARED_ROOT}/cmake/NeoMinimalRelease.cmake")
set(NEOSHARED_ENABLE_ERF ON CACHE BOOL
    "Build the NeoShared ERF/MOD/SAV/HAK/NWM/RIM archive core" FORCE)
if(NOT TARGET neoshared::wx)
    add_subdirectory("${NEOSHARED_ROOT}"
        "${CMAKE_CURRENT_BINARY_DIR}/neoshared" EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET neoshared::erf)
    message(FATAL_ERROR
        "NeoBIF requires the neoshared::erf target. Configure NeoShared with "
        "NEOSHARED_ENABLE_ERF=ON before adding NeoBIF.")
endif()

file(READ "${NEOSHARED_ROOT}/include/neoshared/erf/Utils.hpp" _neobif_shared_utils)
if(NOT _neobif_shared_utils MATCHES "same_regular_file_revision")
    message(FATAL_ERROR "NeoBIF requires the matched NeoShared archive-revision safety update. Publish the complete current NeoShared source; CI selects main automatically.")
endif()

option(NEOBIF_BUILD_CLI "Build the NeoBIF command-line utility" ON)

add_library(neobif_core
    src/core/SafeOutput.cpp
    src/core/ArchiveExport.cpp
    src/core/ResourceQuery.cpp
    src/core/ResourceDocument.cpp
    src/core/KeyBifArchive.cpp
    src/core/LooseArchiveCatalog.cpp
)
target_include_directories(neobif_core PUBLIC src)
target_compile_features(neobif_core PUBLIC cxx_std_17)
target_link_libraries(neobif_core PUBLIC neoshared::erf)
if(EMSCRIPTEN)
    # Exceptions can originate inside the archive core (for example allocation
    # failures) and are caught by the wx/browser transaction boundary.  The
    # library itself must therefore be compiled with Emscripten exception
    # support, not only linked into a target that enables it.
    target_compile_options(neobif_core PRIVATE -fexceptions)
endif()
if(MSVC)
    target_compile_options(neobif_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(neobif_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NEOBIF_BUILD_CLI)
    add_executable(neobif-cli src/cli/main.cpp)
    target_link_libraries(neobif-cli PRIVATE neobif_core)
    if(MSVC)
        target_compile_options(neobif-cli PRIVATE /W4 /permissive-)
    else()
        target_compile_options(neobif-cli PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    install(TARGETS neobif-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()


option(NEOBIF_BUILD_WX_GUI "Build the wxWidgets desktop GUI when wxWidgets is available" ON)
option(NEOBIF_REQUIRE_WX_GUI "Fail configure if wxWidgets is unavailable while NEOBIF_BUILD_WX_GUI is ON" OFF)

function(neobif_link_wx target_name)
    target_compile_definitions(${target_name} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)

    set(_wx_targets)
    foreach(_wx_target IN ITEMS wx::core wx::base wx::adv wx::aui wxWidgets::wxWidgets)
        if(TARGET ${_wx_target})
            list(APPEND _wx_targets ${_wx_target})
        endif()
    endforeach()

    if(_wx_targets)
        target_link_libraries(${target_name} PUBLIC ${_wx_targets})
    else()
        if(wxWidgets_USE_FILE)
            include(${wxWidgets_USE_FILE})
        endif()
        target_include_directories(${target_name} SYSTEM PUBLIC ${wxWidgets_INCLUDE_DIRS})
        target_compile_definitions(${target_name} PUBLIC ${wxWidgets_DEFINITIONS})
        target_link_libraries(${target_name} PUBLIC ${wxWidgets_LIBRARIES})
    endif()

    neo_force_static_msvc_runtime(${target_name})
endfunction()

option(NEOBIF_BUILD_WX_MODULE "Build the reusable wxWidgets panel" OFF)

if(NEOBIF_BUILD_WX_GUI OR NEOBIF_BUILD_WX_MODULE)
    set(NEOBIF_WX_COMPONENTS core base adv aui)
    # vcpkg provides CMake package targets such as wx::core and wx::base.
    # Prefer CONFIG mode on Windows/vcpkg, but keep FindwxWidgets/wx-config as
    # the first choice on Unix-like systems with traditional wxWidgets installs.
    set(_neo_prefer_wx_config_package OFF)
    if(WIN32 OR DEFINED VCPKG_TARGET_TRIPLET OR CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg.cmake")
        set(_neo_prefer_wx_config_package ON)
    endif()

    if(_neo_prefer_wx_config_package)
        find_package(wxWidgets CONFIG QUIET COMPONENTS ${NEOBIF_WX_COMPONENTS})
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets CONFIG QUIET)
        endif()
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets QUIET MODULE COMPONENTS ${NEOBIF_WX_COMPONENTS})
        endif()
    else()
        find_package(wxWidgets QUIET MODULE COMPONENTS ${NEOBIF_WX_COMPONENTS})
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets CONFIG QUIET COMPONENTS ${NEOBIF_WX_COMPONENTS})
        endif()
        if(NOT wxWidgets_FOUND AND NOT TARGET wx::core AND NOT TARGET wxWidgets::wxWidgets)
            find_package(wxWidgets CONFIG QUIET)
        endif()
    endif()

    if(wxWidgets_FOUND OR TARGET wx::core OR TARGET wxWidgets::wxWidgets)
        add_library(neobif_wx STATIC src/wx/BrowserPanel.cpp)
        add_library(neobif::wx ALIAS neobif_wx)
        target_include_directories(neobif_wx PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(neobif_wx PUBLIC neobif_core neoshared::wx)
        neobif_link_wx(neobif_wx)
        if(NOT EMSCRIPTEN)
            find_package(Threads REQUIRED)
            target_link_libraries(neobif_wx PUBLIC Threads::Threads)
        endif()
        if(MSVC)
            target_compile_options(neobif_wx PRIVATE /W4 /permissive-)
        else()
            target_compile_options(neobif_wx PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        if(NEOBIF_BUILD_WX_GUI)
        set(NEOBIF_WX_SOURCES src/wx/main.cpp)
        if(WIN32)
            neo_configure_windows_version_resource(
                NEOBIF_WX_SOURCES_RC
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/neobif.rc.in"
                "neobif.rc"
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/neobif.ico")
            list(APPEND NEOBIF_WX_SOURCES "${NEOBIF_WX_SOURCES_RC}")
        endif()

        if(WIN32)
            add_executable(NeoBIF WIN32 ${NEOBIF_WX_SOURCES})
        elseif(APPLE)
            add_executable(NeoBIF MACOSX_BUNDLE ${NEOBIF_WX_SOURCES})
        else()
            add_executable(NeoBIF ${NEOBIF_WX_SOURCES})
        endif()
        neo_configure_macos_bundle(NeoBIF
            NAME "NeoBIF"
            IDENTIFIER "com.vrifftech.neobif"
            ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/neobif.icns"
        )
        neo_configure_wasm_target(NeoBIF
            NAME "NeoBIF"
            SLUG "neobif"
            ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/neobif.svg"
        )
        target_link_libraries(NeoBIF PRIVATE neobif_wx)
        if(NOT EMSCRIPTEN)
            find_package(Threads REQUIRED)
            target_link_libraries(NeoBIF PRIVATE Threads::Threads)
        endif()
        target_include_directories(NeoBIF PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources)
        target_link_libraries(NeoBIF PRIVATE neoshared::wx)
        neobif_link_wx(NeoBIF)
        target_compile_features(NeoBIF PRIVATE cxx_std_17)
        if(MSVC)
            target_compile_options(NeoBIF PRIVATE /W4 /permissive-)
        else()
            target_compile_options(NeoBIF PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        install(TARGETS NeoBIF
            BUNDLE DESTINATION .
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )

        if(UNIX AND NOT APPLE)
            foreach(NEO_ICON_SIZE 16 24 32 48 64 128 256)
                install(FILES resources/icons/${NEO_ICON_SIZE}x${NEO_ICON_SIZE}/neobif.png
                    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${NEO_ICON_SIZE}x${NEO_ICON_SIZE}/apps
                    RENAME neobif.png
                )
            endforeach()
            configure_file(resources/neobif.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/neobif.desktop @ONLY)
            install(FILES ${CMAKE_CURRENT_BINARY_DIR}/neobif.desktop
                DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
            )
        endif()
        endif() # standalone application wrapper
    elseif(NEOBIF_REQUIRE_WX_GUI OR NEOBIF_BUILD_WX_MODULE)
        message(FATAL_ERROR "wxWidgets was not found, but NEOBIF_REQUIRE_WX_GUI is ON. On Windows, install the pinned wxWidgets 3.3.3 overlay with ../neoshared/scripts/install-wxwidgets.ps1 and configure with the vcpkg toolchain. On Debian or Ubuntu, install libwxgtk3.2-dev.")
    else()
        message(WARNING "wxWidgets was not found. Core and CLI targets will still build; install wxWidgets to build NeoBIF.")
    endif()
endif()

neo_force_static_msvc_runtime_for_directory_targets()
neo_apply_minimal_release_to_directory_targets()
