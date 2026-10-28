# Development setup

Today the build needs a compiler, CMake, Ninja and git, plus the PipeWire and
libsecret development files on Linux, and the X11 and Wayland ones for the app
window. The first configure downloads Catch2 (the test framework), SDL3 and Dear
ImGui, so it needs internet, and building SDL3 adds about two minutes to the first
build. The libraries listed under
"Later" aren't wired into CMake yet; they're what the spec calls for, so install them
when you start on the matching module. How third-party dependencies get pulled in
(system packages, vcpkg, `FetchContent`) is still an open decision.

## Arch Linux

Needed now:

```sh
sudo pacman -S --needed base-devel git cmake ninja clang pipewire libsecret systemd-libs
sudo pacman -S --needed libx11 libxext libxrandr libxcursor libxi libxss libxtst libxkbcommon wayland wayland-protocols mesa libdecor
```

The second line is for the app window (`am-ui`): SDL3 is built from source with
the rest of the project and looks for these to build its X11 and Wayland drivers.
Arch packages include their headers, so there are no separate `-dev` packages.
`libdecor` draws the title bar on Wayland desktops that leave that to the app
(GNOME); without it the window has no title bar there. On Ubuntu the equivalent
is `libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev
libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev
libgl-dev libdecor-0-dev`.

`pipewire` ships the headers and the `libpipewire-0.3` pkg-config file the audio sink
builds against, `libsecret` does the same for the login-keyring vault, and
`systemd-libs` (already installed on any systemd Arch) for sd-bus, which the
media keys use (`pkgconf` comes with `base-devel`). On Ubuntu, which CI uses, the
equivalent is `pkg-config libpipewire-0.3-dev libsecret-1-dev libsystemd-dev`. The
vault tests also need a keyring daemon running (GNOME Keyring or KWallet, which
your desktop normally starts); without one they skip. The media keys tests need a
D-Bus session bus, which every desktop login has; without one they skip.

Later, by module:

| Module | Packages |
| --- | --- |
| ALSA fallback sink | `alsa-lib` |
| Real-time thread priority | `rtkit` |
| Network and catalog (`src/net/`) | `curl simdjson` |
| Tooling | `lldb` (debugger), `playerctl` (testing MPRIS) |

SDL3, Dear ImGui and CEF aren't installed from pacman: CMake fetches SDL3 and
ImGui itself (`src/client/CMakeLists.txt`). CEF is built from source with AAC turned on (the prebuilt
downloads can't decode Apple Music's audio) and unpacked into `third_party/cef/`
(git-ignored). See spec section 3.1.1; the build script lives on the
`experiment/cef-widevine` branch for now. Once it's there, the `linux-cef` preset
builds the daemon with it, and CEF needs a Widevine CDM that a browser installed
(`chromium` plus `chromium-widevine` from the AUR, or `google-chrome`). Keep it
updated: Google revokes old Widevine versions every few months, and a revoked
one gets no licenses (4.10.2934.0 was refused in October 2026; 4.10.3050.0
works). See [`learning/cef-host.md`](learning/cef-host.md) and
[`learning/license-request.md`](learning/license-request.md).

Build:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

Swap `linux-debug` for `linux-asan` (AddressSanitizer + UBSan) or `linux-tsan`
(ThreadSanitizer) to run the same tests under a sanitizer. `linux-fuzz` also
builds the libFuzzer targets; it needs `clang` and its sanitizer runtimes
(`compiler-rt` on Arch). The preset uses plain `clang++`. On Ubuntu 24.04 that's
clang 18, which can't compile `std::expected`, so add
`-DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_C_COMPILER=clang-20` to the configure step
there (CI does). Set the C compiler too whenever you pick a C++ one: SDL3 is C, and
under a sanitizer its code and ours must come from the same compiler.

## Windows 10/11

The spec targets Windows 10 build 19041 or later, x86_64.

Needed now, from an admin PowerShell:

```powershell
winget install --id Git.Git
winget install --id Kitware.CMake
winget install --id Ninja-build.Ninja
winget install --id Microsoft.VisualStudio.2022.Community --override "--add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended"
```

The "Desktop development with C++" workload brings MSVC and a Windows 10/11 SDK
(10.0.19041 or newer). The VS Build Tools edition works too
(`Microsoft.VisualStudio.2022.BuildTools`, workload `Microsoft.VisualStudio.Workload.VCTools`).

WASAPI, DPAPI, Named Pipes, IOCP, DirectX 11 and C++/WinRT (SMTC) all ship with the
Windows SDK. libcurl, simdjson and ImGui will come from vcpkg or `FetchContent` once
that's decided. CEF needs its own Windows build with AAC turned on, unpacked into
`third_party/cef/` (spec section 3.1.1).

Build, from **Developer PowerShell for VS 2022** (so `cl.exe` is on the path):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```
