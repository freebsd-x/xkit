#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fbx::xkit {

// One dialog button. on_tap fires before the dialog dismisses; primary buttons
// are accent-coloured (the confirm/OK action).
struct DialogAction {
    std::string           label;
    std::function<void()> on_tap;
    bool                  primary = false;
};

// Modal dialogs, shown via the overlay layer: centered in the viewport with a
// dim scrim, dismissed only by tapping an action (outside clicks are swallowed
// by the modal). Requires the OverlayHost (present on every app root via
// XApplication::Run). Each returns the overlay id (rarely needed — actions
// auto-dismiss).
class Dialog {
public:
    static uint64_t show(const std::string& title, const std::string& message,
                         std::vector<DialogAction> actions) noexcept;

    // Cancel + a primary confirm button.
    static uint64_t confirm(const std::string& title, const std::string& message,
                            const std::string& confirm_label,
                            std::function<void()> on_confirm,
                            std::function<void()> on_cancel = nullptr) noexcept;

    // Single-button acknowledgement.
    static uint64_t alert(const std::string& title, const std::string& message,
                          std::function<void()> on_ok = nullptr) noexcept;
};

}  // namespace fbx::xkit
