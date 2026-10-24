#pragma once

#include <cstdint>
#include <string>

namespace fbx::xkit {

// A transient status message shown briefly at the bottom-centre of the screen,
// then auto-dismissed. Pointer-transparent (never blocks input) and shown
// imperatively from anywhere: Toast::show("Saved"). Requires the OverlayHost
// (present on every app root via XApplication::Run).
class Toast {
public:
    // Show `message` for `duration_ms`, returning the overlay id (rarely needed).
    static uint64_t show(const std::string& message, int duration_ms = 2500) noexcept;
};

}  // namespace fbx::xkit
