# Cheap guards the compiler and linker can add to every program we build, so
# that many memory bugs crash the program instead of letting someone take it
# over. Like Sanitizers.cmake, the flags go on every target in the build,
# Catch2 and CEF's wrapper included. This file is included before any target
# is created. AM_HARDENING (on by default) turns it off, for comparing.
#
# Linux (GCC and Clang):
#
#   _FORTIFY_SOURCE=3          memcpy, strcpy, snprintf and friends check the
#                              size of the buffer they write to, when the
#                              compiler can work it out. Needs optimization,
#                              so only in the optimized build types (Release,
#                              RelWithDebInfo, MinSizeRel), never in Debug or a
#                              build with no type: Arch's glibc warns about it
#                              without -O, and -Werror would stop there.
#   _GLIBCXX_ASSERTIONS        std::vector, std::span, std::array and others
#                              check the index in operator[] and abort if it's
#                              out of range, instead of reading past the end.
#   -fstack-protector-strong   a secret value (the "canary") next to each
#                              array on the stack, checked before the function
#                              returns. The CEF helper's main() opts out
#                              (cef_helper_main.cpp says why).
#   -fstack-clash-protection   a huge stack allocation can't jump over the
#                              guard page into other memory.
#   -fcf-protection            marks the program ready for Intel CET (checked
#                              jumps and a shadow stack of return addresses).
#                              x86-64 only. On Linux today it's mostly for
#                              later: the kernel doesn't check jumps (IBT) for
#                              programs, and glibc only turns the shadow stack
#                              on when told to.
#   PIE                        the program loads at a random address each run
#                              (ASLR), so an attacker can't know where code is.
#   -z relro -z now            the table of library function addresses is
#                              filled in at startup and then made read-only.
#   -z noexecstack             the stack can't hold runnable code.
#
# Windows (MSVC) already defaults to /DYNAMICBASE, /HIGHENTROPYVA, /NXCOMPAT
# and /GS, the equivalents of PIE, a non-executable stack and the stack
# protector. On top of those:
#
#   /guard:cf                  Control Flow Guard: indirect calls must go to
#                              the start of a real function.
#   /CETCOMPAT                 marks the program as working with the CPU's
#                              shadow stack, so Windows can turn it on. x64
#                              only, like -fcf-protection. The CEF helper
#                              opts out (root CMakeLists.txt says why).

option(AM_HARDENING "Build with compiler and linker hardening flags" ON)

if(NOT AM_HARDENING)
    return()
endif()

include(CheckPIESupported)
check_pie_supported()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Intel CET (-fcf-protection, /CETCOMPAT) only exists on x86-64.
set(_am_x86_64 FALSE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(_am_x86_64 TRUE)
endif()

if(MSVC)
    add_compile_options(/guard:cf)
    add_link_options(/guard:cf)
    if(_am_x86_64)
        add_link_options(/CETCOMPAT)
    endif()
    return()
endif()

add_compile_options(
    -fstack-protector-strong
    -fstack-clash-protection
    "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=3>"
)
add_compile_definitions(_GLIBCXX_ASSERTIONS)
if(_am_x86_64)
    add_compile_options(-fcf-protection=full)
endif()
add_link_options(-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
