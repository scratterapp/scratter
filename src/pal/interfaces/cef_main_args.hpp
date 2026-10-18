#pragma once

// The startup arguments CEF takes from the OS.
//
// CefInitialize() in the daemon and CefExecuteProcess() in its helper program
// both take a CefMainArgs, and CEF defines it differently on each OS:
//
//   Linux    argc and argv; Chromium reads its switches from argv
//   Windows  the program's module handle (HINSTANCE); Chromium reads its
//            switches from the program's own command line, GetCommandLineW(),
//            so it sees the daemon's arguments too and ignores the ones it
//            doesn't know
//
// cef_main_args() builds the right one, which keeps src/daemon/cef_host/ free
// of OS #ifdefs.
//
// Unlike the rest of src/pal, the two .cpp files need CEF's headers, so they
// aren't part of am_pal, which am-ui and amctl link too. The root
// CMakeLists.txt builds them into am_pal_cef, and only with AM_WITH_CEF.

class CefMainArgs;

namespace am::pal {

// CEF's arguments for the running program. Linux keeps the argc and argv it's
// given without copying them, so `argv` has to outlive every CEF call the
// result is passed to. Windows ignores both and uses GetModuleHandleW(nullptr).
[[nodiscard]] CefMainArgs cef_main_args(int argc, char* argv[]);

} // namespace am::pal
