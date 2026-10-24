#pragma once

#include "xkit/xbus_client.h"
#include <string>
#include <functional>

namespace fbx::xkit {

class Clipboard {
public:
    static void setString(XBusClient& bus, const std::string& text) noexcept;
    static void getString(XBusClient& bus, std::function<void(const std::string&)> callback) noexcept;
};

} // namespace fbx::xkit
