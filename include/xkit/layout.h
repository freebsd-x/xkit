#pragma once

#include <algorithm>

namespace fbx::xkit {

enum class Alignment {
    Start,
    Center,
    End
};

// Per-child placement within a StackWidget. Inherit (default) keeps the stack's
// own alignment (Center → centred, else top-left); the corner values let an
// individual overlay anchor itself regardless of the stack's alignment.
enum class StackAnchor {
    Inherit,
    Center,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

// How children are distributed along the main axis (justify-content).
enum class MainAxisAlignment {
    Start,        // pack at start (default)
    Center,       // pack at center
    End,          // pack at end
    SpaceBetween, // equal space between children, none at ends
    SpaceAround,  // equal space around each child (half-space at ends)
    SpaceEvenly,  // equal space between every slot including ends
};

// How children are sized/aligned along the cross axis (align-items).
enum class CrossAxisAlignment {
    Start,        // align to start of cross axis (default)
    Center,       // center on cross axis
    End,          // align to end of cross axis
    Stretch,      // force children to fill the cross axis
};

struct Size {
    float width_{0.0f};
    float height_{0.0f};

    constexpr bool operator==(const Size& other) const noexcept {
        return width_ == other.width_ && height_ == other.height_;
    }
    constexpr bool operator!=(const Size& other) const noexcept {
        return !(*this == other);
    }
};

struct Offset {
    float x_{0.0f};
    float y_{0.0f};

    constexpr bool operator==(const Offset& other) const noexcept {
        return x_ == other.x_ && y_ == other.y_;
    }
    constexpr bool operator!=(const Offset& other) const noexcept {
        return !(*this == other);
    }
};

struct Constraints {
    float min_width_{0.0f};
    float max_width_{0.0f};
    float min_height_{0.0f};
    float max_height_{0.0f};

    [[nodiscard]] constexpr Constraints constrain(const Constraints& childConstraints) const noexcept {
        return {
            .min_width_ = std::max(min_width_, std::min(childConstraints.min_width_, max_width_)),
            .max_width_ = std::max(min_width_, std::min(childConstraints.max_width_, max_width_)),
            .min_height_ = std::max(min_height_, std::min(childConstraints.min_height_, max_height_)),
            .max_height_ = std::max(min_height_, std::min(childConstraints.max_height_, max_height_))
        };
    }
    
    [[nodiscard]] constexpr Size clamp(Size size) const noexcept {
        return {
            .width_ = std::max(min_width_, std::min(size.width_, max_width_)),
            .height_ = std::max(min_height_, std::min(size.height_, max_height_))
        };
    }

    static constexpr Constraints tight(Size size) noexcept {
        return {size.width_, size.width_, size.height_, size.height_};
    }
};

} // namespace fbx::xkit
