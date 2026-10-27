cmake_minimum_required(VERSION 3.25)

# The project's name. It can't contain "Apple" or "Apple Music". The daemon
# binary and the runtime names in src/common/app_name.hpp follow it.
set(AM_APP_NAME "scratter")

project(${AM_APP_NAME} VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(AM_ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(AM_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(AM_BUILD_FUZZERS "Build the libFuzzer targets in tests/ (Clang only)" OFF)
# CEF only exists on machines that built it (it takes hours), so it's off by
# default and CI builds without it. The linux-cef preset turns it on.
option(AM_WITH_CEF "Build the daemon with CEF from third_party/cef/ (see cmake/FindCEF.cmake)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
include(Hardening)

add_library(am_common INTERFACE)
target_include_directories(am_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(am_common INTERFACE AM_APP_NAME="${AM_APP_NAME}")

add_subdirectory(src/pal)
add_subdirectory(src/net)

set(AM_DAEMON_TARGET "${AM_APP_NAME}d")
add_executable(${AM_DAEMON_TARGET}
    src/daemon/main_daemon.cpp
    src/daemon/control_server.cpp
)
target_link_libraries(${AM_DAEMON_TARGET} PRIVATE am_pal am_common)
am_enable_warnings(${AM_DAEMON_TARGET})

if(AM_WITH_CEF)
    if(AM_ENABLE_ASAN OR AM_ENABLE_TSAN)
        message(FATAL_ERROR "AM_WITH_CEF doesn't mix with the sanitizer presets: libcef isn't "
                            "built with them. Use linux-cef.")
    endif()
    find_package(CEF REQUIRED)

    # How CefMainArgs is built differs by OS (src/pal/interfaces/cef_main_args.hpp).
    # Its .cpp files need CEF's headers, so they're built here and not in
    # am_pal, which never links CEF.
    if(WIN32)
        add_library(am_pal_cef STATIC src/pal/win32/cef_main_args.cpp)
    else()
        add_library(am_pal_cef STATIC src/pal/linux/cef_main_args.cpp)
    endif()
    target_include_directories(am_pal_cef PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/pal/interfaces")
    target_link_libraries(am_pal_cef PUBLIC CEF::CEF)
    if(WIN32)
        # The Windows header settings am_pal uses. The helper doesn't link
        # am_pal, and CEF's headers need NOMINMAX at least.
        target_link_libraries(am_pal_cef PUBLIC am_win32_headers)
    endif()
    am_enable_warnings(am_pal_cef)

    # The real CEF host, and the small program CEF runs for its subprocesses.
    set(AM_CEF_HELPER_TARGET "${AM_DAEMON_TARGET}-cef-helper")
    target_sources(${AM_DAEMON_TARGET} PRIVATE src/daemon/cef_host/cef_host.cpp)
    target_link_libraries(${AM_DAEMON_TARGET} PRIVATE am_pal_cef CEF::CEF)
    # The helper's file name as it is on disk, so with .exe on Windows.
    target_compile_definitions(${AM_DAEMON_TARGET} PRIVATE
        AM_CEF_HELPER_NAME="$<TARGET_FILE_NAME:${AM_CEF_HELPER_TARGET}>")

    add_executable(${AM_CEF_HELPER_TARGET} src/daemon/cef_host/cef_helper_main.cpp)
    target_link_libraries(${AM_CEF_HELPER_TARGET} PRIVATE am_pal_cef CEF::CEF)
    am_enable_warnings(${AM_CEF_HELPER_TARGET})
    if(MSVC AND AM_HARDENING)
        # The helper runs CEF's renderers, and V8 isn't shadow-stack safe: with
        # /CETCOMPAT from Hardening.cmake the renderer dies with fast fail 0x39
        # (a shadow-stack violation) about 40 s into the test film. The daemon
        # keeps it.
        target_link_options(${AM_CEF_HELPER_TARGET} PRIVATE /CETCOMPAT:NO)
    endif()
    add_dependencies(${AM_DAEMON_TARGET} ${AM_CEF_HELPER_TARGET})

    am_cef_copy_runtime(${AM_DAEMON_TARGET})
    set_target_properties(${AM_CEF_HELPER_TARGET} PROPERTIES BUILD_RPATH "$ORIGIN")
else()
    # Stands in for the CEF host and says how to turn CEF on.
    target_sources(${AM_DAEMON_TARGET} PRIVATE src/daemon/cef_host/cef_host_off.cpp)
endif()

# A command-line remote for the daemon (play, pause, volume, status), for
# testing by hand and from tests before am-ui exists.
add_executable(amctl tools/amctl/main_amctl.cpp)
target_include_directories(amctl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tools")
target_link_libraries(amctl PRIVATE am_pal am_common)
am_enable_warnings(amctl)

# The app window, am-ui. Its CMakeLists.txt also fetches SDL3 and Dear ImGui.
add_subdirectory(src/client)

enable_testing()
add_subdirectory(tests)
add_subdirectory(bench)
