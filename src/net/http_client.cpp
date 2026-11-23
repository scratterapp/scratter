#include "net/http_client.hpp"

#include "common/app_name.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace am::net {

namespace {

// How long the HTTP thread sleeps in curl_multi_poll() when nothing happens.
// get(), cancel() and the destructor wake it early with curl_multi_wakeup(),
// and curl wakes it for its own timeouts, so this is only a backstop.
constexpr int kIdlePollMillis = 1000;

// curl_global_init() has to run once, before any other curl call.
bool curl_ready() {
    static const bool ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    return ready;
}

// Whether this libcurl was built with HTTP/2 (the Linux system one is; the
// Windows one, without nghttp2, isn't yet).
bool curl_has_http2() {
    static const bool has =
        (curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_HTTP2) != 0;
    return has;
}

bool same_text_ignoring_case(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        const auto lower = [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
        };
        return lower(x) == lower(y);
    });
}

struct UrlDeleter {
    void operator()(CURLU* url) const noexcept { curl_url_cleanup(url); }
};
using UrlHandle = std::unique_ptr<CURLU, UrlDeleter>;

struct CurlText {
    char* text = nullptr;
    CurlText() = default;
    CurlText(const CurlText&) = delete;
    CurlText& operator=(const CurlText&) = delete;
    ~CurlText() { curl_free(text); }
    [[nodiscard]] std::string_view view() const { return text != nullptr ? text : ""; }
};

// Parses `url` with curl's own parser, and keeps it only if it's allowed (see
// the header). The transfer then uses this parsed URL (CURLOPT_CURLU), so the
// check and the request can't read the same text two ways.
UrlHandle parse_allowed(std::string_view url) {
    if (url.find('\0') != std::string_view::npos) {
        return {};
    }
    UrlHandle parsed(curl_url());
    const std::string text(url);
    if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, text.c_str(), 0) != CURLUE_OK) {
        return {};
    }
    CurlText scheme;
    CurlText host;
    if (curl_url_get(parsed.get(), CURLUPART_SCHEME, &scheme.text, 0) != CURLUE_OK ||
        curl_url_get(parsed.get(), CURLUPART_HOST, &host.text, 0) != CURLUE_OK) {
        return {};
    }
    // curl gives the scheme in lower case. The host as written, an IPv6
    // address in its brackets.
    const bool tls = scheme.view() == "https" && !host.view().empty();
    const bool this_machine =
        scheme.view() == "http" && (host.view() == "127.0.0.1" || host.view() == "[::1]" ||
                                    same_text_ignoring_case(host.view(), "localhost"));
    if (!tls && !this_machine) {
        return {};
    }
    return parsed;
}

bool url_is_https(CURLU* url) {
    CurlText scheme;
    return curl_url_get(url, CURLUPART_SCHEME, &scheme.text, 0) == CURLUE_OK &&
           scheme.view() == "https";
}

// "Name: value" for curl's header list. Nothing with a line break, which
// would start a header of its own, and no colon in the name.
bool header_line(const HttpHeader& header, std::string& line) {
    const auto breaks = [](std::string_view s) {
        return s.find_first_of("\r\n") != std::string_view::npos ||
               s.find('\0') != std::string_view::npos;
    };
    if (header.name.empty() || breaks(header.name) || breaks(header.value) ||
        header.name.find(':') != std::string::npos) {
        return false;
    }
    // "Name;" is curl's way of sending a header with an empty value ("Name:"
    // would remove it instead).
    line = header.value.empty() ? header.name + ";" : header.name + ": " + header.value;
    return true;
}

struct SlistDeleter {
    void operator()(curl_slist* list) const noexcept { curl_slist_free_all(list); }
};
using HeaderList = std::unique_ptr<curl_slist, SlistDeleter>;

} // namespace

std::string_view to_string(HttpError error) noexcept {
    switch (error) {
    case HttpError::None:
        return "ok";
    case HttpError::BadRequest:
        return "request not allowed";
    case HttpError::Network:
        return "network error";
    case HttpError::TimedOut:
        return "timed out";
    case HttpError::TooLarge:
        return "answer too large";
    case HttpError::Cancelled:
        return "cancelled";
    }
    return "unknown error";
}

bool HttpClient::allowed_url(std::string_view url) noexcept {
    return curl_ready() && parse_allowed(url) != nullptr;
}

// Everything below runs on the HTTP thread, except get(), cancel() and the
// parts of the destructor marked otherwise, which only touch `queue_`.
struct HttpClient::Impl {
    struct Transfer {
        RequestId id = 0;
        CURL* easy = nullptr;
        UrlHandle url;
        HeaderList headers;
        std::vector<std::string> want_headers;
        std::vector<std::byte> body;
        bool too_large = false;
        std::size_t max_body = 0;
        char error_text[CURL_ERROR_SIZE] = {};
    };

    Impl(Callback done, Options opts, CURLM* m, CURLSH* s)
        : on_done(std::move(done)), options(opts), multi(m), share(s) {}

    ~Impl() {
        for (auto& [id, transfer] : active) {
            curl_multi_remove_handle(multi, transfer->easy);
            curl_easy_cleanup(transfer->easy);
        }
        for (CURL* easy : spare_easy) {
            curl_easy_cleanup(easy);
        }
        curl_multi_cleanup(multi);
        curl_share_cleanup(share);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    static std::size_t on_body(char* data, std::size_t size, std::size_t count, void* user) {
        auto& t = *static_cast<Transfer*>(user);
        const std::size_t bytes = size * count;
        if (bytes > t.max_body - std::min(t.max_body, t.body.size())) {
            t.too_large = true;
            return 0; // anything but `bytes` stops the transfer (CURLE_WRITE_ERROR)
        }
        const auto* first = reinterpret_cast<const std::byte*>(data);
        t.body.insert(t.body.end(), first, first + bytes);
        return bytes;
    }

    void respond(HttpResponse& response) { on_done(response); }

    void refuse(RequestId id) {
        HttpResponse response;
        response.id = id;
        response.error = HttpError::BadRequest;
        response.detail =
            "only https://, or http:// to this machine, and no line breaks in headers";
        respond(response);
    }

    void start(RequestId id, HttpRequest request) {
        UrlHandle url = parse_allowed(request.url);
        if (!url) {
            refuse(id);
            return;
        }
        HeaderList headers;
        for (const HttpHeader& header : request.headers) {
            std::string line;
            if (!header_line(header, line)) {
                refuse(id);
                return;
            }
            curl_slist* longer = curl_slist_append(headers.get(), line.c_str());
            if (longer == nullptr) {
                refuse(id); // out of memory; rare enough not to have its own error
                return;
            }
            (void)headers.release();
            headers.reset(longer);
        }

        auto transfer = std::make_unique<Transfer>();
        transfer->id = id;
        transfer->easy = take_easy();
        if (transfer->easy == nullptr) {
            refuse(id);
            return;
        }
        transfer->url = std::move(url);
        transfer->headers = std::move(headers);
        transfer->want_headers = std::move(request.want_headers);
        transfer->body = take_buffer();
        transfer->max_body = options.max_body_bytes;

        CURL* e = transfer->easy;
        curl_easy_setopt(e, CURLOPT_CURLU, transfer->url.get());
        curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);
        // Verification stays on. There's deliberately no option to turn it off.
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(e, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_2TLS));
        if (curl_has_http2() && url_is_https(transfer->url.get())) {
            // Wait for a connection being set up to say whether it can be
            // shared, rather than opening a second one. Only worth it where
            // HTTP/2 is possible: on HTTP/1.1 it would make every request to
            // a host wait behind the first one's answer.
            curl_easy_setopt(e, CURLOPT_PIPEWAIT, 1L);
        }
        curl_easy_setopt(e, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(e, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
        curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS,
                         static_cast<long>(options.connect_timeout.count()));
        curl_easy_setopt(e, CURLOPT_MAXFILESIZE_LARGE,
                         static_cast<curl_off_t>(options.max_body_bytes));
        curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, ""); // whatever this curl can decode
        curl_easy_setopt(e, CURLOPT_USERAGENT, common::kAppName.data());
        curl_easy_setopt(e, CURLOPT_HTTPHEADER, transfer->headers.get());
        curl_easy_setopt(e, CURLOPT_SHARE, share);
        curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, &Impl::on_body);
        curl_easy_setopt(e, CURLOPT_WRITEDATA, transfer.get());
        curl_easy_setopt(e, CURLOPT_PRIVATE, transfer.get());
        curl_easy_setopt(e, CURLOPT_ERRORBUFFER, transfer->error_text);

        if (curl_multi_add_handle(multi, e) != CURLM_OK) {
            give_back(*transfer);
            refuse(id);
            return;
        }
        active.emplace(id, std::move(transfer));
    }

    void finish(std::unique_ptr<Transfer> transfer, HttpError error, CURLcode code) {
        curl_multi_remove_handle(multi, transfer->easy);

        HttpResponse response;
        response.id = transfer->id;
        response.error = error;
        if (error == HttpError::None) {
            curl_easy_getinfo(transfer->easy, CURLINFO_RESPONSE_CODE, &response.status);
            for (const std::string& name : transfer->want_headers) {
                curl_header* found = nullptr;
                if (curl_easy_header(transfer->easy, name.c_str(), 0, CURLH_HEADER, -1, &found) !=
                    CURLHE_OK) {
                    continue;
                }
                if (found->amount > 1) {
                    (void)curl_easy_header(transfer->easy, name.c_str(), found->amount - 1,
                                           CURLH_HEADER, -1, &found);
                }
                response.headers.push_back({.name = name, .value = found->value});
            }
            const std::size_t size = transfer->body.size();
            transfer->body.resize(size + kBodyPadding); // zeros: resize value-initializes
            response.body = std::span<const std::byte>(transfer->body).first(size);
        } else if (error != HttpError::Cancelled) {
            response.detail = curl_easy_strerror(code);
            if (transfer->error_text[0] != '\0') {
                response.detail += ": ";
                response.detail += transfer->error_text;
            }
        }
        respond(response);
        give_back(*transfer);
    }

    static HttpError error_of(const Transfer& t, CURLcode code) {
        switch (code) {
        case CURLE_OK:
            return HttpError::None;
        case CURLE_OPERATION_TIMEDOUT:
            return HttpError::TimedOut;
        case CURLE_FILESIZE_EXCEEDED:
            return HttpError::TooLarge;
        case CURLE_WRITE_ERROR:
            return t.too_large ? HttpError::TooLarge : HttpError::Network;
        case CURLE_URL_MALFORMAT:
        case CURLE_UNSUPPORTED_PROTOCOL:
            return HttpError::BadRequest;
        default:
            return HttpError::Network;
        }
    }

    // Spare easy handles and body buffers, so a steady stream of requests
    // reuses the same memory. curl_easy_reset() keeps the handle's
    // connections and caches; it only forgets the options.
    CURL* take_easy() {
        if (spare_easy.empty()) {
            return curl_easy_init();
        }
        CURL* easy = spare_easy.back();
        spare_easy.pop_back();
        return easy;
    }
    std::vector<std::byte> take_buffer() {
        if (spare_buffers.empty()) {
            return {};
        }
        std::vector<std::byte> buffer = std::move(spare_buffers.back());
        spare_buffers.pop_back();
        return buffer;
    }
    void give_back(Transfer& t) {
        curl_easy_reset(t.easy);
        spare_easy.push_back(t.easy);
        t.easy = nullptr;
        t.body.clear(); // keeps its capacity
        spare_buffers.push_back(std::move(t.body));
    }

    void run() {
        for (;;) {
            std::vector<std::pair<RequestId, HttpRequest>> starting;
            std::vector<RequestId> cancelling;
            {
                const std::lock_guard lock(mutex);
                if (stopping) {
                    return;
                }
                starting.swap(queued);
                cancelling.swap(cancels);
            }
            for (auto& [id, request] : starting) {
                start(id, std::move(request));
            }
            for (const RequestId id : cancelling) {
                if (auto found = active.find(id); found != active.end()) {
                    std::unique_ptr<Transfer> transfer = std::move(found->second);
                    active.erase(found);
                    finish(std::move(transfer), HttpError::Cancelled, CURLE_OK);
                }
            }

            int running = 0;
            curl_multi_perform(multi, &running);
            int left = 0;
            while (CURLMsg* message = curl_multi_info_read(multi, &left)) {
                if (message->msg != CURLMSG_DONE) {
                    continue;
                }
                Transfer* raw = nullptr;
                curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &raw);
                const CURLcode code = message->data.result;
                if (auto found = active.find(raw->id); found != active.end()) {
                    std::unique_ptr<Transfer> transfer = std::move(found->second);
                    active.erase(found);
                    const HttpError error = error_of(*transfer, code);
                    finish(std::move(transfer), error, code);
                }
            }
            curl_multi_poll(multi, nullptr, 0, kIdlePollMillis, nullptr);
        }
    }

    Callback on_done;
    const Options options;
    CURLM* const multi;
    CURLSH* const share;
    std::unordered_map<RequestId, std::unique_ptr<Transfer>> active;
    std::vector<CURL*> spare_easy;
    std::vector<std::vector<std::byte>> spare_buffers;

    // Shared with the threads calling get() and cancel().
    std::mutex mutex;
    std::vector<std::pair<RequestId, HttpRequest>> queued;
    std::vector<RequestId> cancels;
    bool stopping = false;
    std::atomic<RequestId> next_id{1};

    std::thread thread; // last, so it starts after everything above exists
};

std::expected<std::unique_ptr<HttpClient>, std::error_code> HttpClient::create(Callback on_done,
                                                                               Options options) {
    if (!curl_ready()) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
    CURLM* multi = curl_multi_init();
    CURLSH* share = curl_share_init();
    if (multi == nullptr || share == nullptr) {
        curl_multi_cleanup(multi);
        curl_share_cleanup(share);
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
    // HTTP/2 requests to the same host share one connection where possible.
    curl_multi_setopt(multi, CURLMOPT_PIPELINING, static_cast<long>(CURLPIPE_MULTIPLEX));
    // DNS answers and TLS sessions are reused by every request. Only the
    // HTTP thread touches the share, so it needs no lock functions.
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);

    auto impl = std::make_unique<Impl>(std::move(on_done), options, multi, share);
    impl->thread = std::thread([raw = impl.get()] { raw->run(); });
    return std::unique_ptr<HttpClient>(new HttpClient(std::move(impl)));
}

HttpClient::HttpClient(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

HttpClient::~HttpClient() {
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    curl_multi_wakeup(impl_->multi);
    impl_->thread.join();
    // ~Impl drops whatever was still in flight, without callbacks.
}

RequestId HttpClient::get(HttpRequest request) {
    const RequestId id = impl_->next_id.fetch_add(1, std::memory_order_relaxed);
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->queued.emplace_back(id, std::move(request));
    }
    curl_multi_wakeup(impl_->multi);
    return id;
}

void HttpClient::cancel(RequestId id) {
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->cancels.push_back(id);
    }
    curl_multi_wakeup(impl_->multi);
}

} // namespace am::net
