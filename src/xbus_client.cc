#include "xkit/xbus_client.h"
#include "xserver/ipc_socket.h"
#include <cstring>
#include <iostream>

namespace fbx::xkit {

XBusClient::XBusClient(std::shared_ptr<xserver::IpcSocket> socket) noexcept
    : socket_(std::move(socket)) {}

void XBusClient::Call(const std::string& service, uint32_t opcode, std::span<const uint8_t> payload, ResponseCallback callback, std::optional<xserver::UniqueFd> fd_opt) noexcept {
    if (!socket_) return;

    shared::XBusMessage msg{};
    msg.transaction_id_ = next_transaction_id_++;
    std::strncpy(msg.service_name_, service.c_str(), sizeof(msg.service_name_) - 1);
    msg.opcode_ = opcode;
    
    if (!payload.empty()) {
        std::memcpy(msg.payload_, payload.data(), std::min(payload.size(), sizeof(msg.payload_)));
    }

    if (callback) {
        pending_requests_[msg.transaction_id_] = std::move(callback);
    }

    std::span<const uint8_t> msg_span(reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
    (void)socket_->sendMessage(msg_span, fd_opt ? fd_opt->get() : -1);
}

bool XBusClient::HandleIncoming(const std::span<const uint8_t>& data) noexcept {
    // The live transport is SOCK_SEQPACKET, so each recvmsg delivers exactly one
    // datagram and `data` normally holds a single XBusMessage. This loop is kept
    // as defence-in-depth: XBusMessage is fixed-size, so a buffer that ever does
    // contain several back-to-back messages (a synthetic feed in tests, or a
    // future transport change back toward byte-stream semantics) is still framed
    // implicitly by size. The historical bug was dispatching only the message at
    // data.data() and dropping any trailing frames; walk the buffer in
    // message-sized strides and dispatch every whole XBusMessage whose magic
    // matches, stopping at the first non-XBus frame so raw InputMessages fall
    // through to the caller's input path.
    constexpr size_t kMsgSize = sizeof(shared::XBusMessage);
    size_t offset = 0;
    bool consumed_any = false;
    while (offset + kMsgSize <= data.size()) {
        const auto* msg =
            reinterpret_cast<const shared::XBusMessage*>(data.data() + offset);
        // Stop at the first non-XBus frame: the same socket also carries raw
        // InputMessages (handled by the caller). A leading run of XBus messages
        // is consumed here; anything after a non-matching frame falls through.
        if (msg->magic_ != shared::XBUS_MAGIC) break;

        auto it = pending_requests_.find(msg->transaction_id_);
        if (it != pending_requests_.end()) {
            // Move the callback out before invoking: a reply handler may itself
            // issue new Calls (mutating pending_requests_), so we must not hold
            // an iterator into the map across the call.
            auto cb = std::move(it->second);
            pending_requests_.erase(it);
            cb(*msg);
        } else if (broadcast_callback_) {
            broadcast_callback_(*msg);
        }
        consumed_any = true;
        offset += kMsgSize;
    }
    if (consumed_any) return true;

    // NOTE: a former "Phase 78 prototype" branch here consumed any read the
    // size of an InputMessage and fed it to the broadcast callback as a fake
    // XBusMessage, returning true. That silently swallowed all raw input (mouse
    // AND keyboard) for any client with a broadcast callback registered — e.g.
    // the login greeter, whose clock uses OnBroadcast, could neither focus nor
    // type. Raw InputMessages must fall through (return false) so the caller's
    // input dispatch handles them; real broadcasts carry XBUS_MAGIC and are
    // handled above.
    return false;
}

} // namespace fbx::xkit
