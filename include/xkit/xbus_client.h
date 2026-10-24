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

    // Handles incoming messages on the socket. Returns true if it was an XBus message and consumed it.
    bool HandleIncoming(const std::span<const uint8_t>& data) noexcept;

private:
    std::shared_ptr<xserver::IpcSocket> socket_;
    uint32_t next_transaction_id_{1};
    std::unordered_map<uint32_t, ResponseCallback> pending_requests_;
    BroadcastCallback broadcast_callback_;
};

} // namespace fbx::xkit
