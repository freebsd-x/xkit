#include "xkit/clipboard.h"
#include <cstring>
#include <span>

namespace fbx::xkit {

void Clipboard::setString(XBusClient& bus, const std::string& text) noexcept {
    // We send the string including the null terminator
    std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(text.c_str()), text.length() + 1);
    bus.Call("com.freebsdx.Clipboard", 0x01, payload, nullptr);
}

void Clipboard::getString(XBusClient& bus, std::function<void(const std::string&)> callback) noexcept {
    bus.Call("com.freebsdx.Clipboard", 0x02, {}, [callback = std::move(callback)](const shared::XBusMessage& msg) {
        if (callback) {
            std::string text(reinterpret_cast<const char*>(msg.payload_));
            callback(text);
        }
    });
}

} // namespace fbx::xkit
