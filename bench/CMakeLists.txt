# Benchmarks: plain executables, not ctest tests. Their numbers depend on the
# machine and build type, so they'd only make CI flaky. Run them by hand from a
# linux-release build.

# The IPC channel only has a Linux implementation so far (#15). Drop the
# if(LINUX) once the named pipe one (#12) lands; the benchmark itself has no
# platform code.
if(LINUX)
    add_executable(ipc_ping_pong ipc_ping_pong.cpp)
    target_link_libraries(ipc_ping_pong PRIVATE am_pal am_common)
    am_enable_warnings(ipc_ping_pong)
endif()
