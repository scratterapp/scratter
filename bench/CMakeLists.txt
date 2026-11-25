# Benchmarks: plain executables, not ctest tests. Their numbers depend on the
# machine and build type, so they'd only make CI flaky. Run them by hand from a
# linux-release build.

# The IPC round trip, over the Unix socket on Linux and the named pipe on
# Windows; the benchmark itself has no platform code.
add_executable(ipc_ping_pong ipc_ping_pong.cpp)
target_link_libraries(ipc_ping_pong PRIVATE am_pal am_common)
am_enable_warnings(ipc_ping_pong)

# What our own audio code costs per second of audio; no platform code.
add_executable(audio_render audio_render.cpp)
target_link_libraries(audio_render PRIVATE am_common)
am_enable_warnings(audio_render)

# What one update of the window's spectrum costs the daemon; no platform code.
add_executable(spectrum_fft spectrum_fft.cpp)
target_link_libraries(spectrum_fft PRIVATE am_common)
am_enable_warnings(spectrum_fft)

# The catalog parser on a 500-track playlist (spec section 6, phase 4 gate):
# how long one parse takes, and that it allocates nothing outside the arena.
# It counts allocations by replacing the global operator new, which the
# sanitizers replace too (TSan's can't be overridden, so it won't link), and a
# sanitizer build's timings mean nothing anyway, so it's left out of those.
if(NOT (AM_ENABLE_ASAN OR AM_ENABLE_TSAN))
    add_executable(catalog_parse catalog_parse.cpp)
    target_link_libraries(catalog_parse PRIVATE am_net)
    target_include_directories(catalog_parse PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_compile_definitions(catalog_parse PRIVATE
        AM_CATALOG_SAMPLES="${PROJECT_SOURCE_DIR}/tests/data/catalog")
    am_enable_warnings(catalog_parse)
endif()
