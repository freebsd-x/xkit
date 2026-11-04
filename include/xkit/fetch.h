#pragma once

#include "shared/protocol.h"
#include "xserver/unique_fd.h"
#include <cstdint>
#include <chrono>
#include <functional>
#include <string>

// HTTP(S) for apps, through com.freebsdx.Fetch: a sandboxed app can't open
// sockets, so the compositor relays the request (if the app's host allowlist
// permits it — xserver/fetch_policy.h) to the xfetch daemon.
namespace fbx::xkit {

struct FetchResult {
    shared::FetchStatus status = shared::FetchStatus::Unavailable;
    uint32_t            http_status = 0;
    bool                from_cache = false;
    std::string         body;
    [[nodiscard]] bool ok() const noexcept { return status == shared::FetchStatus::Ok; }
};

// Asynchronous; call it on the UI thread. `done` runs on the UI thread.
void fetch(const std::string& url, std::function<void(FetchResult)> done) noexcept;

// Blocking, for worker threads only (on the UI thread it would wait on itself
// and returns Unavailable instead): hands the request to the UI thread and
// waits for the answer, up to `timeout`.
FetchResult fetchBlocking(const std::string& url,
                          std::chrono::milliseconds timeout = std::chrono::seconds(30)) noexcept;

// A TCP connection, for what fetch can't carry (Mail's IMAP and SMTP). The app's
// sandbox can't open sockets, so the compositor connects, if policy allows that
// port for this app, and hands over the socket. Blocking, for worker threads only.
struct ConnectResult {
    shared::FetchStatus status = shared::FetchStatus::Unavailable;
    xserver::UniqueFd   socket;
    std::string         error;
    [[nodiscard]] bool ok() const noexcept { return status == shared::FetchStatus::Ok && socket; }
};
ConnectResult connectBlocking(const std::string& host, uint16_t port,
                              std::chrono::milliseconds timeout = std::chrono::seconds(20)) noexcept;

}  // namespace fbx::xkit
