#pragma once

#include "shared/protocol.h"
#include <string>
#include <functional>
#include <unordered_map>
#include <memory>
#include <span>

namespace fbx::xserver {
    class IpcSocket;
}

#include "xserver/unique_fd.h"
#include <optional>

namespace fbx::xkit {

class XBusClient {
public:
    using ResponseCallback = std::function<void(const shared::XBusMessage&)>;
    using BroadcastCallback = std::function<void(const shared::XBusMessage&)>;

    explicit XBusClient(std::shared_ptr<xserver::IpcSocket> socket) noexcept;

    // Dispatch an XBus request asynchronously
    void Call(const std::string& service, uint32_t opcode, std::span<const uint8_t> payload, ResponseCallback callback, std::optional<xserver::UniqueFd> fd_opt = std::nullopt) noexcept;

    void OnBroadcast(BroadcastCallback callback) noexcept { broadcast_callback_ = std::move(callback); }
    // Hand a message to the broadcast callback directly (XApplication uses it
    // for compositor actions that arrive as raw input).
    void deliverBroadcast(const fbx::shared::XBusMessage& msg) noexcept {
        if (broadcast_callback_) broadcast_callback_(msg);
    }

    // Handles incoming messages on the socket. Returns true if it was an XBus message and consumed it.
    // `fd`: the descriptor that came with this read (SCM_RIGHTS), if any. A
    // reply callback can claim it with takeIncomingFd(); unclaimed, it closes.
    bool HandleIncoming(const std::span<const uint8_t>& data, xserver::UniqueFd* fd = nullptr) noexcept;

    // Inside a reply/broadcast callback: the fd attached to that message.
    [[nodiscard]] xserver::UniqueFd takeIncomingFd() noexcept {
        return incoming_fd_ ? std::move(*incoming_fd_) : xserver::UniqueFd{};
    }

private:
    std::shared_ptr<xserver::IpcSocket> socket_;
    uint32_t next_transaction_id_{1};
    std::unordered_map<uint32_t, ResponseCallback> pending_requests_;
    BroadcastCallback broadcast_callback_;
    xserver::UniqueFd* incoming_fd_ = nullptr;   // valid only during HandleIncoming
};

} // namespace fbx::xkit
