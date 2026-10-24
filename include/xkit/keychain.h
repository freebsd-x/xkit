#pragma once
// XKeychain — header-only helper for the com.freebsdx.Keychain portal.
//
// Usage:
//   // 1. Unlock the keychain (typically on app start or first use):
//   XKeychain::unlock(xbus, "masterpassword",
//       [](bool ok, uint32_t count) { if (ok) /* ready */ });
//
//   // 2. Store a credential:
//   XKeychain::store(xbus, "com.freebsdx.Browser", "user@example.com",
//       "s3cr3t", [](bool ok) { });
//
//   // 3. Retrieve a credential:
//   XKeychain::retrieve(xbus, "com.freebsdx.Browser", "user@example.com",
//       [](bool found, const std::string& secret) { });
//
//   // 4. Delete a credential:
//   XKeychain::remove(xbus, "com.freebsdx.Browser", "user@example.com",
//       [](bool ok) { });
//
//   // 5. Subscribe to changes (call from XApplication::onBroadcast):
//   XKeychain::handleBroadcast(msg, []() { /* re-list */ });
//
// Callbacks are invoked on the XBus receive thread; dispatch UI work through
// a queued update if required.

#include "shared/protocol.h"
#include "xkit/xbus_client.h"
#include <functional>
#include <string>
#include <cstring>

namespace fbx::xkit {

class XKeychain {
public:
    // Unlock the keychain with the master password.
    // onResult(success, item_count)
    static void unlock(XBusClient& xbus,
                       const std::string& password,
                       std::function<void(bool, uint32_t)> onResult = {})
    {
        shared::KeychainUnlockRequest req{};
        std::strncpy(req.master_password_, password.c_str(),
                     sizeof(req.master_password_) - 1);
        xbus.Call("com.freebsdx.Keychain",
                  static_cast<uint32_t>(shared::KeychainOpcode::Unlock),
                  {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                  [cb = std::move(onResult)](const shared::XBusMessage& ack) {
                      if (!cb) return;
                      shared::KeychainStatusResponse r{};
                      std::memcpy(&r, ack.payload_, sizeof(r));
                      cb(r.success_ != 0, r.item_count_);
                  });
    }

    // Lock the keychain — clears the session key from compositor memory.
    static void lock(XBusClient& xbus,
                     std::function<void(bool)> onResult = {})
    {
        xbus.Call("com.freebsdx.Keychain",
                  static_cast<uint32_t>(shared::KeychainOpcode::Lock),
                  {},
                  [cb = std::move(onResult)](const shared::XBusMessage& ack) {
                      if (!cb) return;
                      shared::KeychainStatusResponse r{};
                      std::memcpy(&r, ack.payload_, sizeof(r));
                      cb(r.success_ != 0);
                  });
    }

    // Store or overwrite a credential.
    // onResult(success) — true if the item was stored (or updated).
    static void store(XBusClient& xbus,
                      const std::string& service,
                      const std::string& account,
                      const std::string& secret,
                      std::function<void(bool)> onResult = {})
    {
        shared::KeychainStoreRequest req{};
        std::strncpy(req.service_, service.c_str(), sizeof(req.service_) - 1);
        std::strncpy(req.account_, account.c_str(), sizeof(req.account_) - 1);
        std::strncpy(req.secret_,  secret.c_str(),  sizeof(req.secret_)  - 1);
        xbus.Call("com.freebsdx.Keychain",
                  static_cast<uint32_t>(shared::KeychainOpcode::Store),
                  {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                  [cb = std::move(onResult)](const shared::XBusMessage& ack) {
                      if (!cb) return;
                      shared::KeychainItemResponse r{};
                      std::memcpy(&r, ack.payload_, sizeof(r));
                      cb(r.found_ != 0);
                  });
    }

    // Retrieve a credential by (service, account).
    // onResult(found, secret) — secret is empty when not found or keychain locked.
    static void retrieve(XBusClient& xbus,
                         const std::string& service,
                         const std::string& account,
                         std::function<void(bool, std::string)> onResult)
    {
        shared::KeychainLookupRequest req{};
        std::strncpy(req.service_, service.c_str(), sizeof(req.service_) - 1);
        std::strncpy(req.account_, account.c_str(), sizeof(req.account_) - 1);
        xbus.Call("com.freebsdx.Keychain",
                  static_cast<uint32_t>(shared::KeychainOpcode::Retrieve),
                  {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                  [cb = std::move(onResult)](const shared::XBusMessage& ack) {
                      shared::KeychainItemResponse r{};
                      std::memcpy(&r, ack.payload_, sizeof(r));
                      r.secret_[sizeof(r.secret_) - 1] = '\0';
                      cb(r.found_ != 0, r.found_ ? std::string(r.secret_) : std::string{});
                  });
    }

    // Delete a credential.
    // onResult(success) — true if the item was found and removed.
    static void remove(XBusClient& xbus,
                       const std::string& service,
                       const std::string& account,
                       std::function<void(bool)> onResult = {})
    {
        shared::KeychainLookupRequest req{};
        std::strncpy(req.service_, service.c_str(), sizeof(req.service_) - 1);
        std::strncpy(req.account_, account.c_str(), sizeof(req.account_) - 1);
        xbus.Call("com.freebsdx.Keychain",
                  static_cast<uint32_t>(shared::KeychainOpcode::Delete),
                  {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                  [cb = std::move(onResult)](const shared::XBusMessage& ack) {
                      if (!cb) return;
                      shared::KeychainStatusResponse r{};
                      std::memcpy(&r, ack.payload_, sizeof(r));
                      cb(r.success_ != 0);
                  });
    }

    // List credentials page by page. Start with page=0, then increment while
    // has_more is true. Secrets are never returned by List.
    // onPage(page, total, has_more, valid, service, account)
    static void list(XBusClient& xbus, uint32_t page,
                     std::function<void(uint32_t /*page*/,
                                        uint32_t /*total*/,
                                        bool     /*has_more*/,
                                        bool     /*valid*/,
                                        std::string /*service*/,
                                        std::string /*account*/)> onPage)
    {
        shared::KeychainPageRequest req{};
        req.page_ = page;
        xbus.Call("com.freebsdx.Keychain",
                  static_cast<uint32_t>(shared::KeychainOpcode::List),
                  {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                  [cb = std::move(onPage)](const shared::XBusMessage& ack) {
                      shared::KeychainListPage r{};
                      std::memcpy(&r, ack.payload_, sizeof(r));
                      r.entry_.service_[sizeof(r.entry_.service_) - 1] = '\0';
                      r.entry_.account_[sizeof(r.entry_.account_) - 1] = '\0';
                      cb(r.page_, r.total_,
                         r.has_more_ != 0, r.valid_ != 0,
                         r.entry_.service_, r.entry_.account_);
                  });
    }

    // Call from XApplication::onBroadcast to receive Changed notifications.
    // Returns true if the message was consumed.
    static bool handleBroadcast(const shared::XBusMessage& msg,
                                 std::function<void()> onChanged = {})
    {
        if (std::strcmp(msg.service_name_, "com.freebsdx.Keychain") != 0)
            return false;
        if (msg.opcode_ != static_cast<uint32_t>(shared::KeychainOpcode::Changed))
            return false;
        if (onChanged) onChanged();
        return true;
    }
};

} // namespace fbx::xkit
