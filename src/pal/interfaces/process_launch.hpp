#pragma once

// Starting the app window from the daemon (issue #41).
//
// When someone clicks the player in the desktop's media widget, MPRIS asks the
// daemon to Raise() it. If no window is connected, the daemon starts am-ui:
//
//   // The am-ui next to this daemon (browser_files.hpp's executable_dir()),
//   // so it's the matching build and not whichever one is first on $PATH.
//   // The build puts the two side by side, and so does `cmake --install`.
//   // The full file name, with .exe on Windows: CreateProcessW doesn't add
//   // it to a full path. The daemon gets it from CMake (AM_UI_NAME).
//   auto dir = am::pal::executable_dir();
//   if (dir) {
//       if (auto started = am::pal::launch_ui(reactor, *dir / AM_UI_NAME, {}); !started) {
//           log started.error().message();
//       }
//   }
//
// The window is a process of its own. The daemon doesn't wait for it, and
// closing or killing the window doesn't touch the daemon, the same as when
// it's started from a terminal or the app launcher. The window isn't stopped
// when the daemon quits either; it shows its "waiting for the daemon" screen,
// as it always does when the daemon goes away.
//
//   Linux:    posix_spawn. A process that exits stays in the process table
//             (a "zombie") until its parent collects its exit status, so the
//             Linux version watches a pidfd for the window on `reactor` and
//             collects it when it exits.
//   Windows:  CreateProcessW. Windows has no zombies, so it doesn't need the
//             reactor: it closes both handles CreateProcessW hands back
//             (hProcess and hThread) once the window has started, and keeps
//             nothing. Arguments go from UTF-8 to UTF-16 and are quoted so
//             the window's C runtime splits them back unchanged.
//
// Errors are std::error_code, like the other interfaces:
//
//   std::errc::function_not_supported  this platform's version isn't written yet
//   std::errc::no_such_file_or_directory
//                                      `program` isn't there (a build that
//                                      didn't make am-ui, say)
//   std::errc::invalid_argument        Windows: an argument isn't valid UTF-8,
//                                      so it can't become UTF-16 unchanged
//   anything else                      what the OS said
//
// It runs on the reactor thread, like everything else the daemon does. Each
// platform defines it once: src/pal/linux/process_launch.cpp and
// src/pal/win32/process_launch.cpp. Nothing here may include a platform header.

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

namespace am::pal {

class Reactor;

// Starts `program` (a full path, file name and all, .exe included on
// Windows) with `args` (not counting the program's own name, which
// goes first as usual) as a process of its own, and returns once it has
// started. The daemon passes `--name NAME` when it runs under a name other
// than the default, so the window connects to this daemon and not another.
// Its standard input reads nothing (/dev/null on Linux); its output goes
// where the daemon's goes, so its error messages end up next to the daemon's
// (the terminal, or the journal under systemd).
[[nodiscard]] std::expected<void, std::error_code> launch_ui(Reactor& reactor,
                                                             const std::filesystem::path& program,
                                                             std::span<const std::string> args);

} // namespace am::pal
