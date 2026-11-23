// Tests for the HTTP client (src/net/http_client.hpp, issue #104), against a
// small HTTP/1.1 server this file starts on 127.0.0.1. It answers each path
// the way a test told it to: a status, a body, a delay, extra headers.
//
// The server is plain sockets. Winsock and BSD sockets differ in a few names,
// so it has the only platform #ifdefs in the tests; src/ has none.

#include "net/http_client.hpp"

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using am::net::HttpClient;
using am::net::HttpError;
using am::net::HttpResponse;
using am::net::kBodyPadding;
using am::net::RequestId;

// ---- Sockets, both ways ------------------------------------------------------

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void close_socket(Socket s) { ::closesocket(s); }
bool readable_within(Socket s, int millis) {
    WSAPOLLFD p{.fd = s, .events = POLLRDNORM, .revents = 0};
    return ::WSAPoll(&p, 1, millis) > 0;
}
void set_receive_timeout(Socket s, std::chrono::milliseconds timeout) {
    const auto millis = static_cast<DWORD>(timeout.count());
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&millis),
                 sizeof(millis));
}
constexpr int kSendFlags = 0;
using IoLength = int; // send() and recv() take an int length here
struct SocketLibrary {
    SocketLibrary() {
        WSADATA data{};
        ok = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~SocketLibrary() {
        if (ok) {
            ::WSACleanup();
        }
    }
    SocketLibrary(const SocketLibrary&) = delete;
    SocketLibrary& operator=(const SocketLibrary&) = delete;
    bool ok = false;
};
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void close_socket(Socket s) { ::close(s); }
bool readable_within(Socket s, int millis) {
    pollfd p{.fd = s, .events = POLLIN, .revents = 0};
    return ::poll(&p, 1, millis) > 0;
}
void set_receive_timeout(Socket s, std::chrono::milliseconds timeout) {
    const timeval tv{.tv_sec = static_cast<time_t>(timeout.count() / 1000),
                     .tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000)};
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
constexpr int kSendFlags = MSG_NOSIGNAL; // a client that hung up is an error, not SIGPIPE
using IoLength = std::size_t;
struct SocketLibrary {
    bool ok = true;
};
#endif

bool send_all(Socket s, std::string_view data) {
    while (!data.empty()) {
        const auto chunk = std::min<std::size_t>(data.size(), 64 * 1024);
        const auto sent = ::send(s, data.data(), static_cast<IoLength>(chunk), kSendFlags);
        if (sent <= 0) {
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

// ---- The server --------------------------------------------------------------

struct Answer {
    int status = 200;
    std::string body{};
    std::chrono::milliseconds delay{0}; // before answering
    std::vector<std::pair<std::string, std::string>> headers{};
    bool send_length = true; // false: no Content-Length, the end of the body is the hang-up
};

class TestServer {
  public:
    TestServer() {
        if (!library_.ok) {
            return;
        }
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == kNoSocket) {
            return;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0; // any free port
        socklen_t length = sizeof(address);
        if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 128) != 0 ||
            ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            return;
        }
        port_ = ntohs(address.sin_port);
        accepting_ = std::thread([this] { accept_loop(); });
    }

    ~TestServer() {
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        woken_.notify_all();
        if (accepting_.joinable()) {
            accepting_.join();
        }
        for (std::thread& connection : connections_) {
            connection.join();
        }
        if (listener_ != kNoSocket) {
            close_socket(listener_);
        }
    }
    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    [[nodiscard]] bool ok() const { return port_ != 0; }

    [[nodiscard]] std::string url(std::string_view path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }

    void answer(const std::string& path, Answer how) {
        const std::lock_guard lock(mutex_);
        answers_[path] = std::move(how);
    }

    // How many requests have arrived, and the newest one's header block.
    [[nodiscard]] int requests() const {
        const std::lock_guard lock(mutex_);
        return requests_;
    }
    [[nodiscard]] std::string last_request() const {
        const std::lock_guard lock(mutex_);
        return last_request_;
    }

    // Waits until `count` requests have arrived.
    [[nodiscard]] bool wait_for_requests(int count, std::chrono::milliseconds timeout) const {
        std::unique_lock lock(mutex_);
        return arrived_.wait_for(lock, timeout, [&] { return requests_ >= count; });
    }

  private:
    void accept_loop() {
        for (;;) {
            {
                const std::lock_guard lock(mutex_);
                if (stopping_) {
                    return;
                }
            }
            if (!readable_within(listener_, 20)) {
                continue;
            }
            const Socket connection = ::accept(listener_, nullptr, nullptr);
            if (connection == kNoSocket) {
                continue;
            }
            connections_.emplace_back([this, connection] {
                serve(connection);
                close_socket(connection);
            });
        }
    }

    void serve(Socket connection) {
        set_receive_timeout(connection, 5s);
        std::string request;
        std::array<char, 4096> chunk{};
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 64 * 1024) {
            const auto got =
                ::recv(connection, chunk.data(), static_cast<IoLength>(chunk.size()), 0);
            if (got <= 0) {
                return;
            }
            request.append(chunk.data(), static_cast<std::size_t>(got));
        }
        // "GET /path HTTP/1.1"
        const std::size_t path_start = request.find(' ') + 1;
        const std::string path =
            request.substr(path_start, request.find(' ', path_start) - path_start);

        Answer how{.status = 404, .body = "not found"};
        {
            const std::lock_guard lock(mutex_);
            ++requests_;
            last_request_ = request;
            if (const auto found = answers_.find(path); found != answers_.end()) {
                how = found->second;
            }
        }
        arrived_.notify_all();

        if (how.delay > 0ms) {
            std::unique_lock lock(mutex_);
            if (woken_.wait_for(lock, how.delay, [&] { return stopping_; })) {
                return; // shutting down
            }
        }
        std::string head = "HTTP/1.1 " + std::to_string(how.status) + " Test\r\n";
        if (how.send_length) {
            head += "Content-Length: " + std::to_string(how.body.size()) + "\r\n";
        }
        for (const auto& [name, value] : how.headers) {
            head.append(name).append(": ").append(value).append("\r\n");
        }
        head += "Connection: close\r\n\r\n";
        if (send_all(connection, head)) {
            (void)send_all(connection, how.body);
        }
    }

    SocketLibrary library_;
    Socket listener_ = kNoSocket;
    std::uint16_t port_ = 0;
    mutable std::mutex mutex_;
    mutable std::condition_variable arrived_;
    std::condition_variable woken_;
    bool stopping_ = false;
    std::map<std::string, Answer> answers_;
    int requests_ = 0;
    std::string last_request_;
    std::thread accepting_;
    std::vector<std::thread> connections_; // only the accept thread adds to it
};

// ---- What the client's callback saw -------------------------------------------

struct Seen {
    RequestId id = 0;
    HttpError error = HttpError::None;
    long status = 0;
    std::string body;
    bool padded = false; // kBodyPadding zero bytes after the body
    std::vector<am::net::HttpHeader> headers;
    std::string detail;
};

class Recorder {
  public:
    HttpClient::Callback callback() {
        return [this](const HttpResponse& r) {
            Seen seen{.id = r.id,
                      .error = r.error,
                      .status = r.status,
                      .body =
                          std::string(reinterpret_cast<const char*>(r.body.data()), r.body.size()),
                      .padded = true,
                      .headers = r.headers,
                      .detail = r.detail};
            if (r.body.data() != nullptr) {
                const std::byte* after = r.body.data() + r.body.size();
                seen.padded = std::all_of(after, after + kBodyPadding,
                                          [](std::byte b) { return b == std::byte{0}; });
            }
            {
                const std::lock_guard lock(mutex_);
                seen_.push_back(std::move(seen));
            }
            changed_.notify_all();
        };
    }

    // Everything seen so far, once there are at least `count`; nothing if
    // they don't arrive in time.
    std::optional<std::vector<Seen>> wait_for(std::size_t count,
                                              std::chrono::milliseconds timeout = 10s) {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, timeout, [&] { return seen_.size() >= count; })) {
            return std::nullopt;
        }
        return seen_;
    }

    [[nodiscard]] std::size_t count() {
        const std::lock_guard lock(mutex_);
        return seen_.size();
    }

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Seen> seen_;
};

} // namespace

TEST_CASE("only https://, or plain http:// to this machine", "[http]") {
    CHECK(HttpClient::allowed_url("https://api.music.apple.com/v1/catalog/us/songs/1"));
    CHECK(HttpClient::allowed_url("HTTPS://Example.com/"));
    CHECK(HttpClient::allowed_url("http://127.0.0.1:8080/x"));
    CHECK(HttpClient::allowed_url("http://localhost/"));
    CHECK(HttpClient::allowed_url("http://[::1]:80/"));

    CHECK_FALSE(HttpClient::allowed_url("http://api.music.apple.com/"));
    CHECK_FALSE(
        HttpClient::allowed_url("http://127.0.0.1@example.com/")); // the host is example.com
    CHECK_FALSE(HttpClient::allowed_url("http://localhost.example.com/"));
    CHECK_FALSE(HttpClient::allowed_url("ftp://127.0.0.1/"));
    CHECK_FALSE(HttpClient::allowed_url("file:///etc/passwd"));
    CHECK_FALSE(HttpClient::allowed_url("https://"));
    CHECK_FALSE(HttpClient::allowed_url("api.music.apple.com/v1"));
    CHECK_FALSE(HttpClient::allowed_url(""));
    CHECK_FALSE(HttpClient::allowed_url(std::string_view("https://a.example\0.com/", 23)));
}

TEST_CASE("a 200 comes back with its body, padded, and the headers asked for", "[http]") {
    TestServer server;
    REQUIRE(server.ok());
    server.answer("/album", {.status = 200,
                             .body = R"({"data":[{"id":"1440857781"}]})",
                             .headers = {{"Content-Type", "application/json"}, {"X-Test", "yes"}}});
    Recorder seen;
    auto http = HttpClient::create(seen.callback());
    REQUIRE(http.has_value());

    const RequestId id = (*http)->get(
        {.url = server.url("/album"),
         .headers = {{"Authorization", "Bearer test-token"}, {"Music-User-Token", "user-token"}},
         .want_headers = {"content-type", "X-Missing"}});
    const auto done = seen.wait_for(1);
    REQUIRE(done.has_value());
    const Seen& r = done->front();
    CHECK(r.id == id);
    CHECK(r.error == HttpError::None);
    CHECK(r.status == 200);
    CHECK(r.body == R"({"data":[{"id":"1440857781"}]})");
    CHECK(r.padded);
    REQUIRE(r.headers.size() == 1); // X-Missing wasn't in the answer
    CHECK(r.headers[0].name == "content-type");
    CHECK(r.headers[0].value == "application/json");

    // The headers went out as given.
    const std::string request = server.last_request();
    CHECK(request.starts_with("GET /album HTTP/1.1\r\n"));
    CHECK(request.find("\r\nAuthorization: Bearer test-token\r\n") != std::string::npos);
    CHECK(request.find("\r\nMusic-User-Token: user-token\r\n") != std::string::npos);
}

TEST_CASE("a 404 is an answer, not an error", "[http]") {
    TestServer server;
    REQUIRE(server.ok());
    Recorder seen;
    auto http = HttpClient::create(seen.callback());
    REQUIRE(http.has_value());

    (void)(*http)->get({.url = server.url("/nothing-here")});
    const auto done = seen.wait_for(1);
    REQUIRE(done.has_value());
    CHECK(done->front().error == HttpError::None);
    CHECK(done->front().status == 404);
    CHECK(done->front().body == "not found");
}

TEST_CASE("a slow server runs into the timeout", "[http]") {
    TestServer server;
    REQUIRE(server.ok());
    server.answer("/slow", {.body = "late", .delay = 5s});
    Recorder seen;
    auto http = HttpClient::create(seen.callback());
    REQUIRE(http.has_value());

    const auto started = std::chrono::steady_clock::now();
    (void)(*http)->get({.url = server.url("/slow"), .timeout = 200ms});
    const auto done = seen.wait_for(1);
    REQUIRE(done.has_value());
    CHECK(done->front().error == HttpError::TimedOut);
    CHECK(done->front().status == 0);
    CHECK_FALSE(done->front().detail.empty());
    CHECK(std::chrono::steady_clock::now() - started < 3s);
}

TEST_CASE("a body over the limit stops, with or without a length up front", "[http]") {
    TestServer server;
    REQUIRE(server.ok());
    server.answer("/big", {.body = std::string(5000, 'x')});
    server.answer("/big-unannounced", {.body = std::string(5000, 'x'), .send_length = false});
    server.answer("/fits", {.body = std::string(1000, 'y')});
    Recorder seen;
    auto http = HttpClient::create(seen.callback(), {.max_body_bytes = 1000});
    REQUIRE(http.has_value());

    const RequestId big = (*http)->get({.url = server.url("/big")});
    const RequestId unannounced = (*http)->get({.url = server.url("/big-unannounced")});
    const RequestId fits = (*http)->get({.url = server.url("/fits")});
    const auto done = seen.wait_for(3);
    REQUIRE(done.has_value());
    for (const Seen& r : *done) {
        if (r.id == big || r.id == unannounced) {
            INFO("request " << r.id);
            CHECK(r.error == HttpError::TooLarge);
            CHECK(r.body.empty());
        } else {
            CHECK(r.id == fits);
            CHECK(r.error == HttpError::None);
            CHECK(r.body.size() == 1000); // exactly the limit is fine
        }
    }
}

TEST_CASE("a request can be cancelled while it's in flight", "[http]") {
    TestServer server;
    REQUIRE(server.ok());
    server.answer("/slow", {.body = "late", .delay = 5s});
    Recorder seen;
    auto http = HttpClient::create(seen.callback());
    REQUIRE(http.has_value());

    const RequestId id = (*http)->get({.url = server.url("/slow")});
    REQUIRE(server.wait_for_requests(1, 5s)); // it's on the server now
    const auto cancelled_at = std::chrono::steady_clock::now();
    (*http)->cancel(id);
    const auto done = seen.wait_for(1);
    REQUIRE(done.has_value());
    CHECK(done->front().id == id);
    CHECK(done->front().error == HttpError::Cancelled);
    CHECK(std::chrono::steady_clock::now() - cancelled_at < 2s);

    // Cancelling again, or an id that's long finished, does nothing.
    (*http)->cancel(id);
    (*http)->cancel(id + 100);
    std::this_thread::sleep_for(100ms);
    CHECK(seen.count() == 1);
}

TEST_CASE("50 requests at once all come back, each with its own body", "[http]") {
    TestServer server;
    REQUIRE(server.ok());
    constexpr int kRequests = 50;
    for (int i = 0; i < kRequests; ++i) {
        server.answer("/track/" + std::to_string(i), {.body = "track " + std::to_string(i)});
    }
    Recorder seen;
    auto http = HttpClient::create(seen.callback());
    REQUIRE(http.has_value());

    std::map<RequestId, std::string> expected;
    for (int i = 0; i < kRequests; ++i) {
        const RequestId id = (*http)->get({.url = server.url("/track/" + std::to_string(i))});
        expected[id] = "track " + std::to_string(i);
    }
    const auto done = seen.wait_for(kRequests, 20s);
    REQUIRE(done.has_value());
    CHECK(done->size() == kRequests);
    for (const Seen& r : *done) {
        INFO("request " << r.id);
        CHECK(r.error == HttpError::None);
        CHECK(r.status == 200);
        CHECK(r.padded);
        CHECK(r.body == expected[r.id]);
    }
}

TEST_CASE("requests from many threads at once", "[http]") {
    TestServer server;
    REQUIRE(server.ok());
    server.answer("/ok", {.body = "ok"});
    Recorder seen;
    auto http = HttpClient::create(seen.callback());
    REQUIRE(http.has_value());

    constexpr int kAskers = 4;
    std::vector<std::thread> askers;
    askers.reserve(kAskers);
    for (int t = 0; t < kAskers; ++t) {
        askers.emplace_back([&] {
            for (int i = 0; i < 5; ++i) {
                (void)(*http)->get({.url = server.url("/ok")});
            }
        });
    }
    for (std::thread& asker : askers) {
        asker.join();
    }
    const auto done = seen.wait_for(20, 20s);
    REQUIRE(done.has_value());
    CHECK(std::ranges::all_of(*done, [](const Seen& r) { return r.body == "ok"; }));
}

TEST_CASE("shutting down with requests in flight is quick and calls nothing back", "[http]") {
    TestServer server;
    REQUIRE(server.ok());
    server.answer("/slow", {.body = "late", .delay = 10s});
    Recorder seen;
    auto http = HttpClient::create(seen.callback());
    REQUIRE(http.has_value());

    for (int i = 0; i < 3; ++i) {
        (void)(*http)->get({.url = server.url("/slow")});
    }
    REQUIRE(server.wait_for_requests(3, 5s));
    const auto stopping = std::chrono::steady_clock::now();
    http->reset();
    CHECK(std::chrono::steady_clock::now() - stopping < 2s);
    CHECK(seen.count() == 0);
}

TEST_CASE("refused requests never reach the network", "[http]") {
    TestServer server;
    REQUIRE(server.ok());
    Recorder seen;
    auto http = HttpClient::create(seen.callback());
    REQUIRE(http.has_value());

    (void)(*http)->get({.url = "http://example.com/"});
    (void)(*http)->get({.url = server.url("/x"), .headers = {{"X-Evil", "a\r\nHost: elsewhere"}}});
    (void)(*http)->get({.url = server.url("/x"), .headers = {{"Bad:Name", "v"}}});
    const auto done = seen.wait_for(3);
    REQUIRE(done.has_value());
    for (const Seen& r : *done) {
        CHECK(r.error == HttpError::BadRequest);
        CHECK(r.status == 0);
    }
    CHECK(server.requests() == 0);
}

TEST_CASE("nobody listening is a network error", "[http]") {
    std::string url;
    {
        TestServer gone;
        REQUIRE(gone.ok());
        url = gone.url("/"); // a port that was free a moment ago, and is again
    }
    Recorder seen;
    auto http = HttpClient::create(seen.callback());
    REQUIRE(http.has_value());

    (void)(*http)->get({.url = url});
    const auto done = seen.wait_for(1, 15s);
    REQUIRE(done.has_value());
    CHECK(done->front().error == HttpError::Network);
    CHECK_FALSE(done->front().detail.empty());
}

// Real TLS, so it needs the internet: hidden from ctest ([.]), run it by hand
// with `test_http_client [network]`. badssl.com serves broken certificates on
// purpose; each of them has to be refused.
TEST_CASE("certificates are checked (needs the internet)", "[.][network]") {
    Recorder seen;
    auto http = HttpClient::create(seen.callback());
    REQUIRE(http.has_value());

    const RequestId good = (*http)->get({.url = "https://badssl.com/"});
    const RequestId expired = (*http)->get({.url = "https://expired.badssl.com/"});
    const RequestId wrong_host = (*http)->get({.url = "https://wrong.host.badssl.com/"});
    const RequestId self_signed = (*http)->get({.url = "https://self-signed.badssl.com/"});
    const auto done = seen.wait_for(4, 30s);
    REQUIRE(done.has_value());
    for (const Seen& r : *done) {
        INFO("request " << r.id << ": " << r.detail);
        if (r.id == good) {
            CHECK(r.error == HttpError::None);
            CHECK(r.status == 200);
        } else {
            CHECK((r.id == expired || r.id == wrong_host || r.id == self_signed));
            CHECK(r.error == HttpError::Network);
        }
    }
}
