#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <optional>
#include "xkit/fetch.h"
#include "xkit/application.h"
#include "xkit/xbus_client.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <future>
#include <memory>

namespace fbx::xkit {

namespace {

// The body xfetch attached to the reply (read-only shm; at most its stated size).
std::string readBody(int fd, uint64_t size) {
    std::string body;
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st) != 0 || st.st_size < 0) return body;
    size_t want = std::min<uint64_t>(size, static_cast<uint64_t>(st.st_size));
    body.resize(want);
    size_t got = 0;
    while (got < want) {
        ssize_t r = ::pread(fd, body.data() + got, want - got, static_cast<off_t>(got));
        if (r <= 0) break;
        got += static_cast<size_t>(r);
    }
    body.resize(got);
    return body;
}

}  // namespace

namespace {

// `post`: the whole request for a kFetchPost, in a descriptor.
void fetchWithFlags(const std::string& url, uint32_t flags, std::function<void(FetchResult)> done,
                    const std::string* post = nullptr) noexcept {
    XBusClient* bus = XApplication::getBus();
    if (!bus) { if (done) done({}); return; }
    shared::FetchRequest req{};
    req.flags_ = flags;
    std::optional<xserver::UniqueFd> long_url;
    if (post) {
        xserver::UniqueFd fd(shm_open(SHM_ANON, O_RDWR | O_CLOEXEC, 0600));
        void* map = MAP_FAILED;
        if (fd && !post->empty() && ::ftruncate(fd.get(), static_cast<off_t>(post->size())) == 0)
            map = ::mmap(nullptr, post->size(), PROT_WRITE, MAP_SHARED, fd.get(), 0);
        if (map == MAP_FAILED) {
            FetchResult r; r.status = shared::FetchStatus::BadRequest;
            if (done) done(std::move(r));
            return;
        }
        std::memcpy(map, post->data(), post->size());
        ::munmap(map, post->size());
        if (!(flags & shared::kFetchScript)) req.flags_ |= shared::kFetchPost;   // a script's request says so itself
        long_url = std::move(fd);
    } else if (url.size() < sizeof(req.url_)) {
        std::strncpy(req.url_, url.c_str(), sizeof(req.url_) - 1);
    } else {
        // Too long for the message. Browsing can send it in a descriptor; anything else
        // would be truncated, and we never fetch a different URL.
        xserver::UniqueFd fd((flags & shared::kFetchWeb) ? shm_open(SHM_ANON, O_RDWR | O_CLOEXEC, 0600) : -1);
        void* map = MAP_FAILED;
        if (fd && ::ftruncate(fd.get(), static_cast<off_t>(url.size())) == 0)
            map = ::mmap(nullptr, url.size(), PROT_WRITE, MAP_SHARED, fd.get(), 0);
        if (map == MAP_FAILED) {
            FetchResult r; r.status = shared::FetchStatus::BadRequest;
            if (done) done(std::move(r));
            return;
        }
        std::memcpy(map, url.data(), url.size());
        ::munmap(map, url.size());
        req.flags_ |= shared::kFetchUrlInFd;
        long_url = std::move(fd);
    }
    bus->Call("com.freebsdx.Fetch", static_cast<uint32_t>(shared::FetchOpcode::Get),
              {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
              [bus, done = std::move(done)](const shared::XBusMessage& m) {
                  shared::FetchReply rep{};
                  std::memcpy(&rep, m.payload_, sizeof(rep));
                  FetchResult r;
                  r.status = static_cast<shared::FetchStatus>(rep.status_);
                  r.http_status = rep.http_status_;
                  r.from_cache = rep.from_cache_ != 0;
                  auto fd = bus->takeIncomingFd();
                  if (r.ok()) {
                      if (fd) r.body = readBody(fd.get(), rep.size_);
                      else r.status = shared::FetchStatus::NetworkError;   // ok without a body
                  }
                  if (done) done(std::move(r));
              },
              std::move(long_url));
}

}  // namespace

void fetch(const std::string& url, std::function<void(FetchResult)> done) noexcept {
    fetchWithFlags(url, 0, std::move(done));
}

namespace {

WebFetchResult webBlocking(const std::string& url, std::optional<std::string> post, std::chrono::milliseconds timeout, bool cookies = false,
                           bool script = false) noexcept {
    WebFetchResult out;
    if (XApplication::onUiThread()) return out;
    auto promise = std::make_shared<std::promise<FetchResult>>();
    auto future = promise->get_future();
    XApplication::post([url, promise, post = std::move(post), cookies, script] {
        fetchWithFlags(url, shared::kFetchWeb | (cookies ? shared::kFetchCookies : 0u) | (script ? shared::kFetchScript : 0u),
                       [promise](FetchResult r) { promise->set_value(std::move(r)); }, post ? &*post : nullptr);
    });
    if (future.wait_for(timeout) != std::future_status::ready) return out;
    FetchResult r = future.get();
    out.status = r.status;
    out.http_status = r.http_status;
    if (!r.ok()) return out;
    // "<final url>\n<content type>\n<body>"
    const size_t first = r.body.find('\n');
    const size_t second = first == std::string::npos ? std::string::npos : r.body.find('\n', first + 1);
    if (second == std::string::npos) {
        out.status = shared::FetchStatus::NetworkError;
        return out;
    }
    out.final_url = r.body.substr(0, first);
    out.content_type = r.body.substr(first + 1, second - first - 1);
    out.body = r.body.substr(second + 1);
    if (script) {
        // "<n>\n" and n bytes of "name: value\r\n" headers, then the body.
        const size_t eol = out.body.find('\n');
        const size_t n = eol == std::string::npos ? std::string::npos : static_cast<size_t>(std::strtoull(out.body.c_str(), nullptr, 10));
        if (eol == std::string::npos || n > out.body.size() - eol - 1) {
            out.status = shared::FetchStatus::NetworkError;
            return out;
        }
        const std::string block = out.body.substr(eol + 1, n);
        out.body.erase(0, eol + 1 + n);
        size_t at = 0;
        while (at < block.size()) {
            size_t end = block.find("\r\n", at);
            if (end == std::string::npos) end = block.size();
            const std::string line = block.substr(at, end - at);
            const size_t colon = line.find(':');
            if (colon != std::string::npos) out.headers.emplace_back(line.substr(0, colon), line.substr(colon + 2 <= line.size() ? colon + 2 : line.size()));
            at = end + 2;
        }
    }
    return out;
}

}  // namespace

WebFetchResult fetchWebBlocking(const std::string& url, std::chrono::milliseconds timeout) noexcept {
    return webBlocking(url, std::nullopt, timeout);
}

WebFetchResult fetchWebBlocking(const std::string& url, bool cookies, std::chrono::milliseconds timeout) noexcept {
    return webBlocking(url, std::nullopt, timeout, cookies);
}

WebFetchResult postWebBlocking(const std::string& url, const std::string& content_type, const std::string& body,
                               std::chrono::milliseconds timeout) noexcept {
    if (url.find('\n') != std::string::npos || content_type.find_first_of("\r\n") != std::string::npos) return {};
    return webBlocking(url, url + "\n" + content_type + "\n" + body, timeout, true);
}

WebFetchResult postWebBlocking(const std::string& url, const std::string& content_type, const std::string& body, bool cookies,
                               std::chrono::milliseconds timeout) noexcept {
    if (url.find('\n') != std::string::npos || content_type.find_first_of("\r\n") != std::string::npos) return {};
    return webBlocking(url, url + "\n" + content_type + "\n" + body, timeout, cookies);
}

WebFetchResult scriptWebBlocking(const std::string& method, const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                                 const std::string& body, bool cookies, std::chrono::milliseconds timeout) noexcept {
    if (method.find('\n') != std::string::npos || url.find('\n') != std::string::npos) return {};
    std::string request = method + "\n" + url + "\n";
    for (const auto& [name, value] : headers) {
        if (name.find_first_of("\r\n:") != std::string::npos || value.find_first_of("\r\n") != std::string::npos) continue;
        request += name + ": " + value + "\n";
    }
    request += "\n" + body;
    return webBlocking(url, std::move(request), timeout, cookies, true);
}

FetchResult fetchBlocking(const std::string& url, std::chrono::milliseconds timeout) noexcept {
    if (XApplication::onUiThread()) return {};
    auto promise = std::make_shared<std::promise<FetchResult>>();
    auto future = promise->get_future();
    XApplication::post([url, promise] {
        fetch(url, [promise](FetchResult r) { promise->set_value(std::move(r)); });
    });
    if (future.wait_for(timeout) != std::future_status::ready) return {};
    return future.get();
}

ConnectResult connectBlocking(const std::string& host, uint16_t port, std::chrono::milliseconds timeout) noexcept {
    ConnectResult failed;
    shared::NetConnectRequest req{};
    if (XApplication::onUiThread() || host.size() >= sizeof(req.host_)) {
        failed.error = "can't connect from here";
        return failed;
    }
    req.port_ = port;
    std::strncpy(req.host_, host.c_str(), sizeof(req.host_) - 1);
    auto promise = std::make_shared<std::promise<ConnectResult>>();
    auto future = promise->get_future();
    XApplication::post([req, promise] {
        XBusClient* bus = XApplication::getBus();
        if (!bus) { ConnectResult none; none.error = "not connected"; promise->set_value(std::move(none)); return; }
        bus->Call("com.freebsdx.Fetch", static_cast<uint32_t>(shared::FetchOpcode::Connect),
                  {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                  [bus, promise](const shared::XBusMessage& m) {
                      shared::NetConnectReply rep{};
                      std::memcpy(&rep, m.payload_, sizeof(rep));
                      rep.error_[sizeof(rep.error_) - 1] = '\0';
                      ConnectResult r;
                      r.status = static_cast<shared::FetchStatus>(rep.status_);
                      r.error = rep.error_;
                      r.socket = bus->takeIncomingFd();
                      promise->set_value(std::move(r));
                  });
    });
    if (future.wait_for(timeout) != std::future_status::ready) {
        failed.error = "the connection timed out";
        return failed;
    }
    return future.get();
}

}  // namespace fbx::xkit
