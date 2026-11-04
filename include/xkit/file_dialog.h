#pragma once
// XFileDialog — header-only helper for opening/saving files via the
// com.freebsdx.FileChooser portal.
//
// Usage:
//   // 1. Request a file picker (returns assigned request_id):
//   uint32_t rid = XFileDialog::openFile(xbus, "Open Image", "image/*",
//       [](const std::string& path) { /* use path */ },
//       []() { /* cancelled */ });
//
//   // 2. In your XApplication::onBroadcast handler:
//   XFileDialog::handleBroadcast(msg);
//
// The Response broadcast matches request_id to the stored callback map.
// Callbacks are invoked on the broadcast thread — call UI operations through
// a queued update if required.
//
// A sandboxed app can't open a path it is told, so it asks for the file itself:
//   XFileDialog::pickToOpen(xbus, "Open", "", [](XFileDialog::Picked p) {
//       if (p.fd) read(p.fd.get(), ...);        // else p.cancelled or p.error
//   });
// The user picks in the shell and the app receives that one descriptor.

#include "shared/protocol.h"
#include "xkit/xbus_client.h"
#include <functional>
#include <unordered_map>
#include <string>
#include <cstring>

namespace fbx::xkit {

class XFileDialog {
public:
    struct Picked {
        xserver::UniqueFd fd;     // valid when the user chose a file
        std::string path;         // what they chose (for display)
        bool cancelled = false;
        std::string error;        // why it couldn't be opened
    };

    // Open a file the user picks: read + write where allowed, else read-only.
    static void pickToOpen(XBusClient& xbus, const std::string& title, const std::string& initialDir,
                           std::function<void(Picked)> done) {
        pick(xbus, shared::FileChooserOpcode::OpenFd, title, initialDir, std::move(done));
    }

    // A file to write to, named by the user; created if new. Its old contents are
    // kept until the app replaces them.
    static void pickToSave(XBusClient& xbus, const std::string& title, const std::string& initialDir,
                           std::function<void(Picked)> done) {
        pick(xbus, shared::FileChooserOpcode::SaveFd, title, initialDir, std::move(done));
    }

    // Request a single-file open dialog.
    // Returns the server-assigned request_id (available after the async ack).
    // Provide onPicked(path) for success, optional onCancel() for dismissal.
    static uint32_t openFile(
            XBusClient& xbus,
            const std::string& title,
            const std::string& mimeFilter,
            std::function<void(std::string)> onPicked,
            std::function<void()> onCancel = {}) {
        return request(xbus, title, mimeFilter, "",
                       false, false, std::move(onPicked), std::move(onCancel));
    }

    // Request a multi-file open dialog.
    // Only the first selected path is provided via onPicked in this release.
    static uint32_t openFiles(
            XBusClient& xbus,
            const std::string& title,
            const std::string& mimeFilter,
            std::function<void(std::string)> onPicked,
            std::function<void()> onCancel = {}) {
        return request(xbus, title, mimeFilter, "",
                       true, false, std::move(onPicked), std::move(onCancel));
    }

    // Request a save-as dialog with an optional initial directory.
    static uint32_t saveFile(
            XBusClient& xbus,
            const std::string& title,
            const std::string& mimeFilter,
            const std::string& initialPath = "",
            std::function<void(std::string)> onPicked = {},
            std::function<void()> onCancel = {}) {
        return request(xbus, title, mimeFilter, initialPath,
                       false, true, std::move(onPicked), std::move(onCancel));
    }

    // Call this from your XApplication::onBroadcast to process Response events.
    // Returns true if the message was consumed (FileChooser Response).
    static bool handleBroadcast(const shared::XBusMessage& msg) {
        using namespace shared;
        if (std::strcmp(msg.service_name_, "com.freebsdx.FileChooser") != 0)
            return false;
        if (msg.opcode_ != static_cast<uint32_t>(FileChooserOpcode::Response))
            return false;

        FileChooserResponse resp{};
        std::memcpy(&resp, msg.payload_, sizeof(resp));

        // Pending ack from server (count_==0, path_[0]=='\0') — store request_id
        // NOTE: the real request_id comes back in the ack; the caller already
        // stored it from the Call() callback. Nothing extra to do here.
        if (resp.count_ == 0 && !resp.cancelled_) return true;

        auto it = s_callbacks_.find(resp.request_id_);
        if (it == s_callbacks_.end()) return true;

        auto [on_picked, on_cancel] = std::move(it->second);
        s_callbacks_.erase(it);

        if (resp.cancelled_) {
            if (on_cancel) on_cancel();
        } else {
            if (on_picked) on_picked(std::string(resp.path_));
        }
        return true;
    }

private:
    static void pick(XBusClient& xbus, shared::FileChooserOpcode op, const std::string& title,
                     const std::string& initialDir, std::function<void(Picked)> done) {
        using namespace shared;
        FileChooserRequest req{};
        req.opcode_ = static_cast<uint32_t>(op);
        std::strncpy(req.title_, title.c_str(), sizeof(req.title_) - 1);
        std::strncpy(req.initial_path_, initialDir.c_str(), sizeof(req.initial_path_) - 1);
        xbus.Call("com.freebsdx.FileChooser", req.opcode_,
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [&xbus, done = std::move(done)](const XBusMessage& m) {
                FileChooserResponse r{};
                std::memcpy(&r, m.payload_, sizeof(r));
                r.path_[sizeof(r.path_) - 1] = '\0';
                Picked p;
                p.cancelled = r.cancelled_ != 0;
                if (r.count_ == 1) { p.fd = xbus.takeIncomingFd(); p.path = r.path_; }
                else if (!p.cancelled) p.error = r.path_;
                if (!p.fd && !p.cancelled && p.error.empty()) p.error = "The file wasn't received";
                if (done) done(std::move(p));
            });
    }

    static uint32_t request(
            XBusClient& xbus,
            const std::string& title,
            const std::string& mimeFilter,
            const std::string& initialPath,
            bool multi,
            bool save,
            std::function<void(std::string)> onPicked,
            std::function<void()> onCancel)
    {
        using namespace shared;
        FileChooserRequest req{};
        req.opcode_       = static_cast<uint32_t>(save ? FileChooserOpcode::SaveFile
                                                        : (multi ? FileChooserOpcode::OpenFiles
                                                                 : FileChooserOpcode::OpenFile));
        req.multi_select_ = multi ? 1u : 0u;
        req.save_mode_    = save  ? 1u : 0u;
        std::strncpy(req.title_,        title.c_str(),       sizeof(req.title_)        - 1);
        std::strncpy(req.filter_mime_,  mimeFilter.c_str(),  sizeof(req.filter_mime_)  - 1);
        std::strncpy(req.initial_path_, initialPath.c_str(), sizeof(req.initial_path_) - 1);

        // Temporary slot keyed on 0; replaced with actual request_id from ack.
        static uint32_t s_tmp_key = 0xFFFF0000u;
        uint32_t tmp = ++s_tmp_key;

        auto on_picked_shared = std::make_shared<std::function<void(std::string)>>(std::move(onPicked));
        auto on_cancel_shared = std::make_shared<std::function<void()>>(std::move(onCancel));

        xbus.Call("com.freebsdx.FileChooser",
            req.opcode_,
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [tmp, on_picked_shared, on_cancel_shared](const XBusMessage& ack) {
                FileChooserResponse r{};
                std::memcpy(&r, ack.payload_, sizeof(r));
                if (r.request_id_ == 0) return; // error
                // Re-register with the real server-assigned id
                s_callbacks_.erase(tmp);
                s_callbacks_[r.request_id_] = {*on_picked_shared, *on_cancel_shared};
            });

        // Temporarily register under tmp until the ack arrives
        s_callbacks_[tmp] = {*on_picked_shared, *on_cancel_shared};
        return tmp;
    }

    // Map from request_id → {onPicked, onCancel}
    static inline std::unordered_map<
        uint32_t,
        std::pair<std::function<void(std::string)>, std::function<void()>>
    > s_callbacks_;
};

} // namespace fbx::xkit
