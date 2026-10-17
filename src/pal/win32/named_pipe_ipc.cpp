// TODO: Implement overlapped Named Pipe control IPC. Owner: @kerelosgayed
//
// Until then this is a stub so the daemon links on Windows: connect() and
// listen() fail with "not supported". Replace the whole file with the real
// pipe (src/pal/interfaces/ipc_channel.hpp, issue #12).

#include "ipc_channel.hpp"

#include <utility>

namespace am::pal {

struct IpcChannel::Impl {};
struct IpcListener::Impl {};

namespace {
std::unexpected<std::error_code> not_implemented() {
    return std::unexpected(std::make_error_code(std::errc::function_not_supported));
}
} // namespace

std::expected<IpcChannel, std::error_code> IpcChannel::connect(Reactor&, std::string_view) {
    return not_implemented();
}

IpcChannel::IpcChannel(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
IpcChannel::~IpcChannel() = default;
IpcChannel::IpcChannel(IpcChannel&&) noexcept = default;
IpcChannel& IpcChannel::operator=(IpcChannel&&) noexcept = default;

std::expected<IoStatus, std::error_code> IpcChannel::send(std::span<const std::byte>) {
    return not_implemented();
}
std::expected<ReceiveResult, std::error_code> IpcChannel::receive(std::span<std::byte>) {
    return not_implemented();
}
void IpcChannel::set_readable_callback(Callback) {}
void IpcChannel::set_writable_callback(Callback) {}

std::expected<IpcListener, std::error_code> IpcListener::listen(Reactor&, std::string_view) {
    return not_implemented();
}

IpcListener::IpcListener(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
IpcListener::~IpcListener() = default;
IpcListener::IpcListener(IpcListener&&) noexcept = default;
IpcListener& IpcListener::operator=(IpcListener&&) noexcept = default;

std::expected<std::optional<IpcChannel>, std::error_code> IpcListener::accept() {
    return not_implemented();
}
void IpcListener::set_accept_callback(Callback) {}

} // namespace am::pal
