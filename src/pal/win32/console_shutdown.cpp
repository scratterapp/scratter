// TODO: Implement Ctrl-C / console close handling with SetConsoleCtrlHandler. Owner: @kerelosgayed
//
// The handler runs on a thread Windows creates for it, so it should only call
// Reactor::wake() (or post a packet to the completion port) and let the
// reactor thread run the callback, the same promise the Linux signalfd
// version keeps (src/pal/interfaces/shutdown_signal.hpp).
//
// Until then this is a stub so the daemon links: watch() fails with "not
// supported".

#include "shutdown_signal.hpp"

#include <utility>

namespace am::pal {

struct ShutdownSignal::Impl {};

std::expected<ShutdownSignal, std::error_code> ShutdownSignal::watch(Reactor&, Callback) {
    return std::unexpected(std::make_error_code(std::errc::function_not_supported));
}

ShutdownSignal::ShutdownSignal(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ShutdownSignal::~ShutdownSignal() = default;
ShutdownSignal::ShutdownSignal(ShutdownSignal&&) noexcept = default;
ShutdownSignal& ShutdownSignal::operator=(ShutdownSignal&&) noexcept = default;

} // namespace am::pal
