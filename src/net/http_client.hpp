#pragma once

// HTTP for the Apple Music API (spec section 3.3.1, issue #104).
//
// One HttpClient runs one libcurl multi handle on a thread of its own. Any
// thread can ask for a GET; the answer comes back through the callback, on
// the client's thread:
//
//   auto http = HttpClient::create([&](const HttpResponse& r) {
//       // On the HTTP thread. r.body is only valid until this returns: parse
//       // it here (the catalog parser, #105) or copy it, then wake the
//       // window the way DaemonLink does.
//   });
//   if (!http) { log http.error().message(); }
//   const RequestId id = (*http)->get({
//       .url = "https://api.music.apple.com/v1/catalog/us/albums/1440857781",
//       .headers = {{"Authorization", "Bearer " + developer_token}},
//   });
//   (*http)->cancel(id); // changed our mind: the callback says Cancelled
//
// Why a thread and not the reactor: the spec's plan has curl's sockets
// watched by pal::Reactor (curl_multi_socket_action). The Windows reactor is
// IOCP, which reports finished operations, not readiness, and am-ui's window
// thread has no reactor, so that would be a new readiness API in src/pal/
// first. curl_multi_poll() on a thread does the same job with no platform
// code. Callers only see get(), cancel() and the callback, so this can move
// onto a reactor later without them changing. (Decided on the roadmap, #6.)
//
// What every request gets, with no way to turn it off:
//   - TLS verification of the certificate and the host name. https:// only,
//     except plain http:// to this machine (127.0.0.1, ::1, localhost), which
//     the tests use. Anything else is refused with HttpError::BadRequest.
//   - A total timeout (HttpRequest::timeout) and a connect timeout.
//   - A maximum body size (Options::max_body_bytes): a bigger answer stops
//     downloading and ends with HttpError::TooLarge.
//   - No redirects, and nothing logged: request headers carry the bearer
//     token and the Music-User-Token, so none of them are ever printed.
//
// HTTP/2 is asked for and used where curl was built with it (the Linux system
// libcurl). The Windows build has no nghttp2 yet, so there it's HTTP/1.1 with
// keep-alive. Connections, DNS answers and TLS sessions are kept and reused
// between requests either way.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace am::net {

// Bytes of zeros after every body, so simdjson can read past the end safely
// (SIMDJSON_PADDING, 64 in every simdjson release; the parser, #105, checks
// it with a static_assert).
inline constexpr std::size_t kBodyPadding = 64;

using RequestId = std::uint64_t;

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequest {
    std::string url{};
    std::vector<HttpHeader> headers{};
    // Response headers to hand back, by name, case ignored (HTTP/2 sends them
    // in lower case). Empty for none.
    std::vector<std::string> want_headers{};
    std::chrono::milliseconds timeout{std::chrono::seconds{15}};
};

enum class HttpError : std::uint8_t {
    None,       // an answer came; check `status`, a 404 is still an answer
    BadRequest, // the URL isn't allowed (see the top), or a header has a line break
    Network,    // no connection: DNS, refused, reset, TLS handshake or certificate
    TimedOut,   // HttpRequest::timeout passed first
    TooLarge,   // the body went over Options::max_body_bytes
    Cancelled,  // cancel() was called before it finished
};

[[nodiscard]] std::string_view to_string(HttpError error) noexcept;

struct HttpResponse {
    RequestId id = 0;
    HttpError error = HttpError::None;
    long status = 0; // the HTTP status, or 0 when no answer came
    // The headers asked for in want_headers that the answer had, in the order
    // asked. A header sent twice appears once, with its last value.
    std::vector<HttpHeader> headers;
    // The body, followed in memory by kBodyPadding zero bytes that aren't
    // part of the span. Only valid during the callback: the buffer is reused
    // for the next request.
    std::span<const std::byte> body;
    // curl's own words when error isn't None, for a log line.
    std::string detail;
};

class HttpClient {
  public:
    // Runs on the HTTP thread, once per request, finished or not. It must not
    // call back into this client's destructor; get() and cancel() are fine.
    using Callback = std::move_only_function<void(const HttpResponse&)>;

    struct Options {
        std::size_t max_body_bytes = std::size_t{16} * 1024 * 1024; // a big playlist is ~2 MB
        std::chrono::milliseconds connect_timeout{std::chrono::seconds{10}};
    };

    // Starts the HTTP thread. Fails with not_enough_memory if curl can't be
    // set up.
    [[nodiscard]] static std::expected<std::unique_ptr<HttpClient>, std::error_code>
    create(Callback on_done, Options options);
    [[nodiscard]] static std::expected<std::unique_ptr<HttpClient>, std::error_code>
    create(Callback on_done) {
        return create(std::move(on_done), Options{});
    }

    // Stops the thread. Requests still in flight are dropped without a
    // callback, so nothing runs after this returns.
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) = delete;
    HttpClient& operator=(HttpClient&&) = delete;

    // Starts a GET and returns at once. Any thread. A request that isn't
    // allowed still gets an id, and its callback says BadRequest.
    RequestId get(HttpRequest request);

    // Ends a request early: its callback says Cancelled, unless it already
    // finished (then this does nothing). Any thread.
    void cancel(RequestId id);

    // True for the URLs get() accepts (see the top of this file).
    [[nodiscard]] static bool allowed_url(std::string_view url) noexcept;

  private:
    struct Impl;
    explicit HttpClient(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace am::net
