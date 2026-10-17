set(AM_PAL_SOURCES)

if(LINUX)
    list(APPEND AM_PAL_SOURCES
        linux/alsa_sink.cpp
        linux/browser_files.cpp
        linux/epoll_reactor.cpp
        linux/pipewire_sink.cpp
        linux/rtkit_priority.cpp
        linux/secret_vault.cpp
        linux/signalfd_shutdown.cpp
        linux/unix_seqpacket_ipc.cpp
        linux/mpris_controls.cpp
        linux/posix_shm.cpp
    )
elseif(WIN32)
    list(APPEND AM_PAL_SOURCES
        win32/wasapi_sink.cpp
        win32/browser_files.cpp
        win32/dpapi_vault.cpp
        win32/iocp_reactor.cpp
        win32/mmcss_priority.cpp
        win32/named_pipe_ipc.cpp
        win32/smtc_controls.cpp
        win32/win32_shm.cpp
        win32/console_shutdown.cpp
    )
else()
    message(FATAL_ERROR "Unsupported platform: only Linux and Windows are supported")
endif()

add_library(am_pal STATIC ${AM_PAL_SOURCES})
target_include_directories(am_pal PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/interfaces"
)
target_link_libraries(am_pal PUBLIC am_common)

if(WIN32)
    # One set of Windows header settings for every target that links am_pal, so
    # <windows.h> is lean, doesn't define min/max macros, and resolves the -W/-A
    # API names to their wide forms. _WIN32_WINNT/WINVER pin the SDK to Windows
    # 10, which is what the IOCP and WASAPI code targets.
    target_compile_definitions(am_pal PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
    )
endif()

if(LINUX)
    # PipeWire comes from the system (Arch: pipewire, Ubuntu: libpipewire-0.3-dev).
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    target_link_libraries(am_pal PRIVATE PkgConfig::PIPEWIRE)

    # libsecret talks to the desktop keyring for the credential vault
    # (Arch: libsecret, Ubuntu: libsecret-1-dev).
    pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
    target_link_libraries(am_pal PRIVATE PkgConfig::LIBSECRET)

    # sd-bus, part of libsystemd, publishes the MPRIS media controls on D-Bus
    # (Arch: systemd-libs, Ubuntu: libsystemd-dev). GLOBAL so the MPRIS
    # tests can link it too.
    pkg_check_modules(LIBSYSTEMD REQUIRED IMPORTED_TARGET GLOBAL libsystemd)
    target_link_libraries(am_pal PRIVATE PkgConfig::LIBSYSTEMD)
endif()
am_enable_warnings(am_pal)
