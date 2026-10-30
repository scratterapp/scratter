# Finds our CEF build (built with AAC turned on, see spec 3.1.1) and sets up
# what the daemon needs to use it. Only the daemon and its CEF helper link CEF;
# am-ui never does.
#
#   find_package(CEF REQUIRED)
#
# Looks in CEF_ROOT, which defaults to third_party/cef/linux64 (windows64 on
# Windows). The "Building CEF with AAC" section of docs/CEF.md
# says how to build it and link it there. Any CEF "minimal" binary
# distribution of the same version works for building; only one built with
# AAC plays Apple Music.
#
# Defines:
#   CEF::CEF            an INTERFACE target: CEF's headers (as system headers,
#                       so our warning flags don't apply to them), libcef and
#                       libcef_dll_wrapper, CEF's C++ layer over its C API,
#                       which comes as source and is built here
#   am_cef_copy_runtime(target)
#                       copies libcef and its resource files next to `target`,
#                       where CEF looks for them at run time
#   CEF_FOUND, CEF_VERSION_STRING

if(WIN32)
    set(_am_cef_platform windows64)
else()
    set(_am_cef_platform linux64)
endif()
set(CEF_ROOT "${PROJECT_SOURCE_DIR}/third_party/cef/${_am_cef_platform}"
    CACHE PATH "Our CEF build (a CEF binary distribution built with AAC)")

if(NOT EXISTS "${CEF_ROOT}/cmake/cef_variables.cmake")
    set(CEF_FOUND FALSE)
    if(CEF_FIND_REQUIRED)
        message(FATAL_ERROR
            "CEF not found in ${CEF_ROOT}. Build it as the \"Building CEF with AAC\" "
            "section of docs/CEF.md describes, "
            "or point CEF_ROOT at a CEF binary distribution.")
    endif()
    return()
endif()

# CEF's own CMake helpers: the compiler flags its wrapper needs, the lists of
# files to copy, and macros like COPY_FILES. Its own FindCEF.cmake does the
# same two includes; ours replaces it because this file has the same name.
set(_CEF_ROOT "${CEF_ROOT}")
set(_CEF_ROOT_EXPLICIT 1)
if(WIN32)
    # CEF builds its wrapper with the static C++ runtime (/MT, /MTd) unless
    # told otherwise, and our code uses CMake's default, the DLL runtime (/MD,
    # /MDd). MSVC refuses to link the two together (LNK2038), so the wrapper
    # follows ours. libcef.dll itself is unaffected: its interface is plain C.
    set(CEF_RUNTIME_LIBRARY_FLAG /MD)
endif()
enable_language(C) # cef_variables checks C compiler flags too
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
include(cef_variables)
include(cef_macros)

# CEF picks its library folder from the build type, but our build only has
# Release (CEF's Debug build is many GB and hours more). A Debug daemon links
# the Release libcef just fine, since the interface between them is plain C.
if(NOT EXISTS "${CEF_BINARY_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}cef${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(CEF_BINARY_DIR "${CEF_BINARY_DIR_RELEASE}")
endif()
if(WIN32)
    set(_am_cef_lib "${CEF_BINARY_DIR}/libcef.lib")
else()
    set(_am_cef_lib "${CEF_BINARY_DIR}/libcef.so")
endif()
if(NOT EXISTS "${_am_cef_lib}")
    message(FATAL_ERROR "${CEF_ROOT} has no ${_am_cef_lib}")
endif()

# The wrapper is built with CEF's flags, not ours. CEF makes warnings errors
# there too; a newer compiler than CEF was tested with can add warnings in code
# we don't own, so drop that.
list(REMOVE_ITEM CEF_COMPILER_FLAGS -Werror /WX)
list(REMOVE_ITEM CEF_CXX_COMPILER_FLAGS -Werror /WX)
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" "${CMAKE_BINARY_DIR}/libcef_dll_wrapper"
                 EXCLUDE_FROM_ALL)

add_library(CEF::CEF INTERFACE IMPORTED)
target_include_directories(CEF::CEF SYSTEM INTERFACE "${CEF_ROOT}")
target_link_libraries(CEF::CEF INTERFACE libcef_dll_wrapper "${_am_cef_lib}" ${CEF_STANDARD_LIBS})

file(STRINGS "${CEF_ROOT}/include/cef_version.h" _am_cef_version_line
     REGEX "^#define CEF_VERSION \"")
string(REGEX REPLACE "^#define CEF_VERSION \"([^\"]*)\".*" "\\1" CEF_VERSION_STRING
       "${_am_cef_version_line}")
message(STATUS "Found CEF ${CEF_VERSION_STRING} in ${CEF_ROOT}")
set(CEF_FOUND TRUE)

# libcef, its helper libraries and its resource files (.pak files, ICU data,
# locales) have to sit next to the program. Copies only what changed.
function(am_cef_copy_runtime target)
    COPY_FILES(${target} "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "$<TARGET_FILE_DIR:${target}>")
    COPY_FILES(${target} "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "$<TARGET_FILE_DIR:${target}>")
    if(NOT WIN32)
        # Find libcef.so next to the program, wherever the build folder is.
        set_target_properties(${target} PROPERTIES BUILD_RPATH "$ORIGIN")
    endif()
endfunction()
