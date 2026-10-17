# Sanitizer flags are added to every target in the build, including the test
# framework, so instrumented and uninstrumented code never mix. This file is
# included before any target is created.
#
#   AM_ENABLE_ASAN  AddressSanitizer + UndefinedBehaviorSanitizer (linux-asan preset)
#   AM_ENABLE_TSAN  ThreadSanitizer (linux-tsan preset)

if(AM_ENABLE_ASAN AND AM_ENABLE_TSAN)
    message(FATAL_ERROR "AM_ENABLE_ASAN and AM_ENABLE_TSAN cannot both be enabled")
endif()

if(NOT (AM_ENABLE_ASAN OR AM_ENABLE_TSAN))
    return()
endif()

if(MSVC)
    message(FATAL_ERROR "Sanitizers are currently supported only with Clang/GCC")
endif()

if(AM_ENABLE_ASAN)
    # vptr checks need RTTI, which the project builds without.
    set(AM_SANITIZER_FLAGS
        -fsanitize=address,undefined
        -fno-sanitize=vptr
        -fno-sanitize-recover=undefined
    )
else()
    set(AM_SANITIZER_FLAGS -fsanitize=thread)
endif()

add_compile_options(${AM_SANITIZER_FLAGS} -fno-omit-frame-pointer)
add_link_options(${AM_SANITIZER_FLAGS})
