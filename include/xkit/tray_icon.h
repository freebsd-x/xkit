#pragma once
// XTrayIcon — convenience wrapper for com.freebsdx.Tray XBus service.
//
// Usage:
//   auto tray = std::make_shared<XTrayIcon>(&xbus, "com.myapp", "🔔", "My App");
//   tray->setOnClick([](){ /* toggle app window */ });
//   // Later:
//   tray->setStatus(TrayStatus::Attention);
//   tray->setTooltip("3 new messages");

#include "xkit/xbus_client.h"
#include "shared/protocol.h"
#include <functional>
#include <string>
#include <cstring>

namespace fbx::xkit {

class XTrayIcon {
public:
    XTrayIcon(XBusClient* xbus,
              const std::string& app_id,
              const std::string& icon_text,
              const std::string& tooltip,
              shared::TrayStatus status = shared::TrayStatus::Active) noexcept
        : xbus_(xbus), id_(0)
    {
        if (!xbus_) return;
        shared::TrayRegisterRequest req{};
        std::strncpy(req.app_id_,    app_id.c_str(),    sizeof(req.app_id_)-1);
        std::strncpy(req.icon_text_, icon_text.c_str(), sizeof(req.icon_text_)-1);
        std::strncpy(req.tooltip_,   tooltip.c_str(),   sizeof(req.tooltip_)-1);
        req.status_ = static_cast<uint32_t>(status);

        xbus_->Call("com.freebsdx.Tray",
            static_cast<uint32_t>(shared::TrayOpcode::Register),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [this](const shared::XBusMessage& resp) {
                shared::TrayEntry entry{};
                std::memcpy(&entry, resp.payload_, sizeof(entry));
                id_ = entry.id_;
            });
    }

    ~XTrayIcon() noexcept { unregister(); }

    XTrayIcon(const XTrayIcon&) = delete;
    XTrayIcon& operator=(const XTrayIcon&) = delete;

    void setIconText(const std::string& text) noexcept {
        if (!xbus_ || !id_) return;
        shared::TrayUpdateRequest req{};
        req.id_ = id_;
        std::strncpy(req.value_, text.c_str(), sizeof(req.value_)-1);
        xbus_->Call("com.freebsdx.Tray",
            static_cast<uint32_t>(shared::TrayOpcode::SetIcon),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    }

    void setTooltip(const std::string& tip) noexcept {
        if (!xbus_ || !id_) return;
        shared::TrayUpdateRequest req{};
        req.id_ = id_;
        std::strncpy(req.value_, tip.c_str(), sizeof(req.value_)-1);
        xbus_->Call("com.freebsdx.Tray",
            static_cast<uint32_t>(shared::TrayOpcode::SetTooltip),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    }

    void setStatus(shared::TrayStatus st) noexcept {
        if (!xbus_ || !id_) return;
        shared::TrayStatusRequest req{};
        req.id_     = id_;
        req.status_ = static_cast<uint32_t>(st);
        xbus_->Call("com.freebsdx.Tray",
            static_cast<uint32_t>(shared::TrayOpcode::SetStatus),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    }

    void setOnClick(std::function<void()> cb) noexcept { on_click_ = std::move(cb); }

    // Call this from your XBusClient::OnBroadcast handler:
    //   if (XTrayIcon::handleBroadcast(msg)) return;
    bool handleBroadcast(const shared::XBusMessage& msg) noexcept {
        if (std::strcmp(msg.service_name_, "com.freebsdx.Tray") != 0) return false;
        if (msg.opcode_ != static_cast<uint32_t>(shared::TrayOpcode::Click)) return false;
        shared::TrayIdRequest cr{};
        std::memcpy(&cr, msg.payload_, sizeof(cr));
        if (cr.id_ == id_ && on_click_) on_click_();
        return true;
    }

    void unregister() noexcept {
        if (!xbus_ || !id_) return;
        shared::TrayIdRequest req{};
        req.id_ = id_;
        xbus_->Call("com.freebsdx.Tray",
            static_cast<uint32_t>(shared::TrayOpcode::Unregister),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        id_ = 0;
    }

    [[nodiscard]] uint32_t id() const noexcept { return id_; }

private:
    XBusClient*           xbus_;
    uint32_t              id_;
    std::function<void()> on_click_;
};

} // namespace fbx::xkit
