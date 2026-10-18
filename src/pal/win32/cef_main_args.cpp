// Windows implementation of src/pal/interfaces/cef_main_args.hpp.
//
// GetModuleHandleW(nullptr) is the running .exe: the daemon when it calls
// CefInitialize(), the helper program when it calls CefExecuteProcess().

#include "cef_main_args.hpp"

#include <windows.h>

#include "include/cef_base.h" // CefMainArgs

namespace am::pal {

CefMainArgs cef_main_args(int /*argc*/, char* /*argv*/[]) {
    return CefMainArgs(::GetModuleHandleW(nullptr));
}

} // namespace am::pal
