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
