#pragma once

#include <cstdint>

// Pure geometry for placing a popup (dropdown, menu, tooltip) relative to an
// anchor within a viewport: pick a side, flip to the opposite side when there
// isn't room, and clamp so the popup stays on screen. Dependency-free and
// header-only so it is unit-testable without any widget/render machinery.
namespace fbx::xkit::overlay {

struct Rect { float x, y, w, h; };
struct Point { float x, y; };

enum class Side : uint8_t { Below, Above, Right, Left };

struct Placement {
    float x, y;   // top-left where the popup should be positioned
    Side  side;   // the side actually used (may differ from preferred after a flip)
};

inline float clampf(float v, float lo, float hi) noexcept {
    if (hi < lo) return lo;             // viewport smaller than popup → pin to lo
    return v < lo ? lo : (v > hi ? hi : v);
}

// Place a `popup` (w×h) against `anchor` inside `viewport` (origin 0,0), on the
// preferred side, leaving `gap` between anchor and popup. Vertical sides
// (Below/Above) align the popup's left edge to the anchor's left and clamp
// horizontally; horizontal sides (Right/Left) align tops and clamp vertically.
// If the preferred side lacks room and the opposite side has more, it flips.
inline Placement placePopup(Rect anchor, float popup_w, float popup_h,
                            float vw, float vh, Side preferred,
                            float gap = 4.0f) noexcept {
    Placement p{};
    switch (preferred) {
    case Side::Below:
    case Side::Above: {
        float below_y = anchor.y + anchor.h + gap;
        float above_y = anchor.y - gap - popup_h;
        float space_below = vh - (anchor.y + anchor.h);
        float space_above = anchor.y;
        bool want_below = (preferred == Side::Below);
        // Flip only if the preferred side doesn't fit but the other does.
        if (want_below && popup_h + gap > space_below && popup_h + gap <= space_above)
            want_below = false;
        else if (!want_below && popup_h + gap > space_above && popup_h + gap <= space_below)
            want_below = true;
        p.side = want_below ? Side::Below : Side::Above;
        p.y = want_below ? below_y : above_y;
        p.x = anchor.x;
        break;
    }
    case Side::Right:
    case Side::Left: {
        float right_x = anchor.x + anchor.w + gap;
        float left_x  = anchor.x - gap - popup_w;
        float space_right = vw - (anchor.x + anchor.w);
        float space_left  = anchor.x;
        bool want_right = (preferred == Side::Right);
        if (want_right && popup_w + gap > space_right && popup_w + gap <= space_left)
            want_right = false;
        else if (!want_right && popup_w + gap > space_left && popup_w + gap <= space_right)
            want_right = true;
        p.side = want_right ? Side::Right : Side::Left;
        p.x = want_right ? right_x : left_x;
        p.y = anchor.y;
        break;
    }
    }
    // Final clamp so the popup never spills off any edge.
    p.x = clampf(p.x, 0.0f, vw - popup_w);
    p.y = clampf(p.y, 0.0f, vh - popup_h);
    return p;
}

// Is `pt` inside `r`? (used for outside-click dismissal / overlay hit-testing)
inline bool contains(Rect r, Point pt) noexcept {
    return pt.x >= r.x && pt.x < r.x + r.w && pt.y >= r.y && pt.y < r.y + r.h;
}

}  // namespace fbx::xkit::overlay
