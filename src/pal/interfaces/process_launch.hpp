#pragma once

// Starting the app window from the daemon (issue #41).
//
// When someone clicks the player in the desktop's media widget, MPRIS asks the
// daemon to Raise() it. If no window is connected, the daemon starts am-ui:
//
//   auto ui = am::pal::program_next_to_this_one(AM_UI_NAME);  // "am-ui"
//   if (ui) {
//       if (auto started = am::pal::launch_ui(reactor, *ui); !started) {
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
//             collects it when it exits. /proc/self/exe finds this program.
//   Windows:  not written yet (function_not_supported). CreateProcessW and
//             GetModuleFileNameW, for kero; Windows has no zombies, so it
//             won't need the reactor.
//
// Errors are std::error_code, like the other interfaces:
//
//   std::errc::function_not_supported  this platform's version isn't written yet
//   std::errc::no_such_file_or_directory
//                                      `program` isn't there (a build that
//                                      didn't make am-ui, say)
//   anything else                      what the OS said
//
// Both functions run on the reactor thread, like everything else the daemon
// does. Each platform defines them once: src/pal/linux/process_launch.cpp and
// src/pal/win32/process_launch.cpp. Nothing here may include a platform header.

#include <expected>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace am::pal {

class Reactor;

// The full path of `file_name` in the folder this program's own executable is
// in. The build puts am-ui next to the daemon, and so does `cmake --install`,
// so this finds the window that belongs to this daemon, not whichever one is
// first on $PATH. Doesn't check that the file exists.
[[nodiscard]] std::expected<std::filesystem::path, std::error_code>
program_next_to_this_one(std::string_view file_name);

// Starts `program`, with no arguments, as a process of its own, and returns
// once it has started. Its standard input reads nothing (/dev/null on Linux);
// its output goes where the daemon's goes, so its error messages end up next
// to the daemon's (the terminal, or the journal under systemd).
[[nodiscard]] std::expected<void, std::error_code> launch_ui(Reactor& reactor,
                                                             const std::filesystem::path& program);

} // namespace am::pal
