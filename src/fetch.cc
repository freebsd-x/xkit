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

void fetch(const std::string& url, std::function<void(FetchResult)> done) noexcept {
    XBusClient* bus = XApplication::getBus();
    if (!bus) { if (done) done({}); return; }
    shared::FetchRequest req{};
    std::strncpy(req.url_, url.c_str(), sizeof(req.url_) - 1);
    if (url.size() >= sizeof(req.url_)) {   // would be truncated: never fetch a different URL
        FetchResult r; r.status = shared::FetchStatus::BadRequest;
        if (done) done(std::move(r));
        return;
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
              });
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

}  // namespace fbx::xkit
