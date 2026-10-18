# Tests use Catch2 v3, fetched and built with the rest of the project so it gets
# the same sanitizer flags. Exceptions are off project-wide, so a failed REQUIRE
# aborts the test case instead of throwing.
include(FetchContent)

set(CATCH_CONFIG_DISABLE_EXCEPTIONS ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.9.1
    GIT_SHALLOW TRUE
    SYSTEM
)
FetchContent_MakeAvailable(Catch2)

# Build Catch2 itself without exceptions too, so it matches the code it tests.
foreach(target Catch2 Catch2WithMain)
    if(MSVC)
        target_compile_options(${target} PRIVATE /EHs-c-)
        target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
    else()
        target_compile_options(${target} PRIVATE -fno-exceptions)
    endif()
endforeach()

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

# One executable per file. Add a file's name here once it has at least one
# TEST_CASE (ctest fails on a test binary with none).
set(AM_TESTS
    test_sanity
    test_crc32c
    test_ipc_framing
    test_ipc_messages
    test_spsc_concurrency
    test_telemetry_seqlock
    test_resampler_drift
    test_audio_path
    test_amctl_args
    test_credential_vault
    test_media_controls
    test_reactor
)

# These need a platform implementation, and only Linux has one so far.
# Move test_realtime into the list above once the Windows side of #18 (#14)
# lands; it tests behavior the platforms share. test_epoll_reactor,
# test_pipewire_sink and test_unix_seqpacket_ipc stay Linux-only, and so does
# test_mpris_controls, which talks D-Bus to the MPRIS code.
if(LINUX)
    list(APPEND AM_TESTS test_epoll_reactor test_realtime test_pipewire_sink
         test_unix_seqpacket_ipc test_mpris_controls)
endif()

# The Windows counterpart of test_epoll_reactor: watching handles with the IOCP
# reactor, plus the Windows specifics of its wakeup and timer.
if(WIN32)
    list(APPEND AM_TESTS test_iocp_reactor)
endif()


foreach(test ${AM_TESTS})
    add_executable(${test} ${test}.cpp)
    target_link_libraries(${test} PRIVATE am_pal am_common Catch2::Catch2WithMain)
    am_enable_warnings(${test})
    # Registers each TEST_CASE as its own ctest test. PRE_TEST lists them when
    # ctest runs instead of at build time.
    catch_discover_tests(${test} DISCOVERY_MODE PRE_TEST)
endforeach()

if(LINUX)
    # Plays the desktop's side of D-Bus with sd-bus directly.
    target_link_libraries(test_mpris_controls PRIVATE PkgConfig::LIBSYSTEMD)
endif()

# amctl's argument parser lives with amctl, in tools/.
target_include_directories(test_amctl_args PRIVATE "${PROJECT_SOURCE_DIR}/tools")

# ShmMapping has a Linux (posix_shm.cpp) and a Windows (win32_shm.cpp)
# implementation, so its two-process test runs on both platforms.

# The second process for the two-process SeqLock test.
add_executable(shm_reader_child shm_reader_child.cpp)
target_link_libraries(shm_reader_child PRIVATE am_pal am_common)
am_enable_warnings(shm_reader_child)

add_executable(test_shm_mapping test_shm_mapping.cpp)
target_link_libraries(test_shm_mapping PRIVATE am_pal am_common Catch2::Catch2WithMain)
target_compile_definitions(test_shm_mapping PRIVATE
    AM_SHM_READER_CHILD="$<TARGET_FILE:shm_reader_child>")
add_dependencies(test_shm_mapping shm_reader_child)
am_enable_warnings(test_shm_mapping)
catch_discover_tests(test_shm_mapping DISCOVERY_MODE PRE_TEST)

# Windows-only: the win32_shm.cpp specifics (owner/DACL, page-rounded size,
# invalid UTF-8 names) that the cross-platform test above doesn't cover.
if(WIN32)
    add_executable(test_win32_shm test_win32_shm.cpp)
    target_link_libraries(test_win32_shm PRIVATE am_pal am_common Catch2::Catch2WithMain)
    am_enable_warnings(test_win32_shm)
    # A test that can't run in this shell (the foreign-owner one needs an elevated
    # token) prints AM_TEST_SKIPPED instead of calling Catch2's SKIP(), which
    # aborts in a no-exceptions build. ctest reports such a test as Skipped.
    catch_discover_tests(test_win32_shm DISCOVERY_MODE PRE_TEST
                         PROPERTIES SKIP_REGULAR_EXPRESSION "AM_TEST_SKIPPED")
endif()

if(LINUX)
    # Starts the daemon binary and talks to it, directly and through amctl.
    add_executable(test_daemon test_daemon.cpp)
    target_link_libraries(test_daemon PRIVATE am_pal am_common Catch2::Catch2WithMain
                                              PkgConfig::LIBSYSTEMD)
    target_compile_definitions(test_daemon PRIVATE
        AM_DAEMON_PATH="$<TARGET_FILE:${AM_DAEMON_TARGET}>"
        AM_AMCTL_PATH="$<TARGET_FILE:amctl>")
    add_dependencies(test_daemon ${AM_DAEMON_TARGET} amctl)
    target_include_directories(test_daemon PRIVATE "${PROJECT_SOURCE_DIR}/tools")
    am_enable_warnings(test_daemon)
    catch_discover_tests(test_daemon DISCOVERY_MODE PRE_TEST)

    # A few seconds of the soak test (tools/soak/soak.py) against the tone with
    # no sound card, so the script keeps working with the daemon it measures.
    # The real run is hours long and by hand (docs/learning/soak-test.md).
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        set(soak_dir "${CMAKE_CURRENT_BINARY_DIR}/soak_smoke")
        file(MAKE_DIRECTORY "${soak_dir}/runtime")
        add_test(NAME soak_smoke
                 COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/soak/soak.py"
                         --build "$<TARGET_FILE_DIR:${AM_DAEMON_TARGET}>" --hours 0.002
                         --interval 1 --name soak_smoke --out "${soak_dir}/results"
                         -- --no-audio)
        # A private $XDG_RUNTIME_DIR, like test_daemon, so the daemon's socket
        # can't clash with a real one.
        set_tests_properties(soak_smoke PROPERTIES
                             ENVIRONMENT "XDG_RUNTIME_DIR=${soak_dir}/runtime")
    endif()

    # Not a ctest test: plays a sine through the speakers (see the file).
    add_executable(play_sine manual/play_sine.cpp)
    target_link_libraries(play_sine PRIVATE am_pal am_common)
    am_enable_warnings(play_sine)
    set_target_properties(play_sine PROPERTIES RUNTIME_OUTPUT_DIRECTORY manual)

    # Not a ctest test: saves a dummy secret so you can see it in Seahorse.
    add_executable(vault_demo manual/vault_demo.cpp)
    target_link_libraries(vault_demo PRIVATE am_pal am_common)
    am_enable_warnings(vault_demo)
    set_target_properties(vault_demo PROPERTIES RUNTIME_OUTPUT_DIRECTORY manual)
endif()

# libFuzzer targets. libFuzzer ships with Clang and supplies its own main(), so
# these can't be Catch2 tests. Use the linux-fuzz preset, which turns this on
# together with ASan/UBSan. ctest only runs a short smoke run of each; the long
# run is a separate command (see the comment at the top of each fuzz_*.cpp).
if(AM_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "AM_BUILD_FUZZERS needs Clang (libFuzzer is part of it)")
    endif()

    foreach(fuzzer fuzz_ipc_framing)
        add_executable(${fuzzer} ${fuzzer}.cpp)
        target_link_libraries(${fuzzer} PRIVATE am_common)
        target_compile_options(${fuzzer} PRIVATE -fsanitize=fuzzer)
        target_link_options(${fuzzer} PRIVATE -fsanitize=fuzzer)
        am_enable_warnings(${fuzzer})
        add_test(NAME ${fuzzer} COMMAND ${fuzzer} -runs=100000)
    endforeach()
endif()
