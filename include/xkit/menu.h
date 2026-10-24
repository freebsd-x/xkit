#pragma once

#include "xkit/overlay_layout.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fbx::xkit {

// One menu entry. A separator draws a divider and ignores label/on_tap; a
// disabled item is shown dimmed and does nothing. on_tap fires before dismiss.
struct MenuItem {
    std::string           label;
    std::function<void()> on_tap;
    bool                  separator = false;
    bool                  disabled  = false;

    static MenuItem sep() noexcept { MenuItem m; m.separator = true; return m; }
};

// Non-modal popup menus (context menus + menu buttons) on the overlay layer:
// a surface card of item rows, dismissed by choosing an item or clicking
// outside. Requires the OverlayHost (present on every app root via
// XApplication::Run). Returns the overlay id.
class Menu {
public:
    // Context menu at a point (e.g. a right-click position).
    static uint64_t showAt(float x, float y, std::vector<MenuItem> items) noexcept;

    // Menu anchored below a trigger's on-screen rect (auto-flips above near the
    // bottom edge, like Dropdown).
    static uint64_t showBelow(overlay::Rect anchor, std::vector<MenuItem> items) noexcept;
};

}  // namespace fbx::xkit
