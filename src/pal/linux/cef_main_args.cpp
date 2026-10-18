// Linux implementation of src/pal/interfaces/cef_main_args.hpp.

#include "cef_main_args.hpp"

#include "include/cef_base.h" // CefMainArgs

namespace am::pal {

CefMainArgs cef_main_args(int argc, char* argv[]) { return CefMainArgs(argc, argv); }

} // namespace am::pal
