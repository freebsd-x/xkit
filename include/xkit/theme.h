#pragma once

#include <cstdint>
#include <atomic>
#include <algorithm>

namespace fbx::xkit {

struct Color {
    float r, g, b, a;
};

struct Theme {
    Color background;
    Color surface;
    Color text;
    Color accent;
};

// Three system accent presets matching Settings UI: Blue, Purple, Orange.
// Indices must match ThemeSetRequest::accent_idx_ in protocol.h.
inline constexpr Color kAccentPresets[3] = {
    {0.04f, 0.52f, 1.00f, 1.0f},  // 0: Blue   #0A84FF
    {0.75f, 0.35f, 0.95f, 1.0f},  // 1: Purple #BF5AF2
    {1.00f, 0.62f, 0.04f, 1.0f},  // 2: Orange #FF9F0A
};

class ThemeManager {
public:
    static ThemeManager& getInstance() noexcept {
        static ThemeManager instance;
        return instance;
    }

    [[nodiscard]] const Theme& getCurrentTheme() const noexcept {
        return is_dark_mode_ ? dark_theme_ : light_theme_;
    }

    void setDarkMode(bool dark) noexcept {
        is_dark_mode_ = dark;
    }

    [[nodiscard]] bool isDarkMode() const noexcept {
        return is_dark_mode_;
    }

    // idx: 0=Blue, 1=Purple, 2=Orange (matches Settings UI and protocol.h).
    void setAccentIndex(int idx) noexcept {
        if (idx < 0 || idx > 2) return;
        accent_idx_ = idx;
        dark_theme_.accent  = kAccentPresets[idx];
        light_theme_.accent = kAccentPresets[idx];
    }

    [[nodiscard]] int accentIndex() const noexcept { return accent_idx_; }

private:
    ThemeManager() noexcept = default;

    std::atomic<bool> is_dark_mode_{false};
    int accent_idx_{0};

    Theme light_theme_ = {
        .background = {0.95f, 0.95f, 0.95f, 1.0f},
        .surface    = {1.00f, 1.00f, 1.00f, 1.0f},
        .text       = {0.10f, 0.10f, 0.10f, 1.0f},
        .accent     = kAccentPresets[0],
    };

    Theme dark_theme_ = {
        .background = {0.10f, 0.10f, 0.10f, 1.0f},
        .surface    = {0.15f, 0.15f, 0.15f, 1.0f},
        .text       = {0.90f, 0.90f, 0.90f, 1.0f},
        .accent     = kAccentPresets[0],
    };
};

} // namespace fbx::xkit
