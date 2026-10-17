// CefHost for daemons built without CEF (the AM_WITH_CEF CMake option off,
// the default). start() always fails and says how to turn CEF on; the rest
// is never reached. The real one is cef_host.cpp.

#include "daemon/cef_host/cef_host.hpp"

namespace am::daemon {

struct CefHost::Impl {};

std::expected<CefHost, std::string> CefHost::start(const CefHostOptions&, audio::AudioPath&) {
    return std::unexpected("this daemon was built without CEF. Build it with the linux-cef "
                           "preset (or -DAM_WITH_CEF=ON), see docs/learning/cef-host.md");
}

CefHost::CefHost(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
CefHost::CefHost(CefHost&&) noexcept = default;
CefHost& CefHost::operator=(CefHost&&) noexcept = default;
CefHost::~CefHost() = default;

void CefHost::set_state(common::PlaybackState) {}

} // namespace am::daemon
