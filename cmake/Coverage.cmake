# Line and branch coverage (linux-coverage preset). Built with GCC's gcov
# format, which both GCC and Clang can write; read it with gcovr (see CLAUDE.md).
# Like the sanitizers, it's added to every target before any is created.
#
#   AM_ENABLE_COVERAGE  instrument everything for coverage

if(NOT AM_ENABLE_COVERAGE)
    return()
endif()

if(MSVC)
    message(FATAL_ERROR "Coverage is currently supported only with GCC/Clang")
endif()

if(AM_ENABLE_ASAN OR AM_ENABLE_TSAN)
    message(FATAL_ERROR "AM_ENABLE_COVERAGE can't be combined with a sanitizer")
endif()

add_compile_options(--coverage)
add_link_options(--coverage)
