// CEF's helper program. Chromium runs each web page, the GPU work, the audio
// service and the Widevine CDM in processes of their own, and starts every one
// of them by running this program with a --type=... switch. The daemon points
// CEF at it with CefSettings::browser_subprocess_path, so the daemon itself
// never has to tell "am I a CEF subprocess?" apart from its own arguments.
//
// Why NO_STACK_PROTECTOR: on Linux most of these processes are forked from
// Chromium's "zygote" process instead of started fresh, and Chromium gives
// each forked child a new stack canary, the secret value the compiler's
// stack protector checks when a function returns. main() was already running
// before the fork, so it still holds the zygote's old canary; when the child
// finishes and main() returns, the check fails and glibc kills the process
// with "*** stack smashing detected ***". The #5 experiment hit exactly this
// when playback stopped. CEF's own samples mark main() this way for the same
// reason. With MSVC the macro is empty.

#include "cef_main_args.hpp"

#include "include/base/cef_compiler_specific.h"
#include "include/cef_app.h"

NO_STACK_PROTECTOR
int main(int argc, char* argv[]) {
    // argc and argv on Linux, the helper's module handle on Windows.
    const CefMainArgs main_args = am::pal::cef_main_args(argc, argv);
    // Runs the subprocess until it's done and returns its exit code. There's
    // no CefApp: the built-in page needs nothing custom in the renderer.
    return CefExecuteProcess(main_args, nullptr, nullptr);
}
