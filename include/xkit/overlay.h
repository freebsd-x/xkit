#pragma once

#include "xkit/overlay_layout.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace fbx::xkit {

class Widget;  // forward — OverlaySpec only holds a shared_ptr

// One active overlay: a content widget anchored to a rect, optionally modal.
struct OverlaySpec {
    uint64_t                id = 0;              // assigned by show() if 0
    std::shared_ptr<Widget> content;            // the popup's widget subtree
    overlay::Rect           anchor{};           // anchor rect in viewport coords
    overlay::Side           preferred = overlay::Side::Below;
    bool                    centered = false;   // center in the viewport (dialogs), ignore anchor
    bool                    bottom = false;      // pin to bottom-centre (toasts), ignore anchor
    bool                    modal = false;      // dims (scrim) + swallows background input
    bool                    interactive = true; // false = pointer-transparent (tooltips): never
                                                //   receives clicks, never click-dismissed
    std::function<void()>   on_dismiss;         // fired when the overlay closes
    // Auto-dismiss time (toasts); default (epoch) = never auto-dismiss.
    std::chrono::steady_clock::time_point expires{};
    overlay::Rect           bounds{};           // resolved placement (host fills)
};

// What a pointer-down should do given the current overlay stack.
enum class ClickAction {
    PassToContent,  // no overlays → normal app input
    PassToOverlay,  // click landed inside an overlay → route to it
    DismissTop,     // click outside a non-modal overlay → close it
    SwallowModal,   // click outside a modal overlay → block, don't close
};

// Per-app stack of active overlays. Widgets push overlays here; the OverlayHost
// (wrapped around the app root in XApplication::Run) renders and routes them.
class OverlayManager {
public:
    static OverlayManager& instance() noexcept {
        static OverlayManager m;
        return m;
    }

    uint64_t show(OverlaySpec spec) noexcept {
        if (spec.id == 0) spec.id = next_id_++;
        overlays_.push_back(std::move(spec));
        return overlays_.back().id;
    }

    void dismiss(uint64_t id) noexcept {
        for (auto it = overlays_.begin(); it != overlays_.end(); ++it) {
            if (it->id == id) {
                auto cb = std::move(it->on_dismiss);
                overlays_.erase(it);
                if (cb) cb();
                return;
            }
        }
    }

    void dismissTop() noexcept {
        if (!overlays_.empty()) dismiss(overlays_.back().id);
    }

    void dismissAll() noexcept {
        while (!overlays_.empty()) dismiss(overlays_.back().id);
    }

    [[nodiscard]] bool   empty() const noexcept { return overlays_.empty(); }
    [[nodiscard]] size_t count() const noexcept { return overlays_.size(); }
    [[nodiscard]] std::vector<OverlaySpec>&       overlays() noexcept { return overlays_; }
    [[nodiscard]] const std::vector<OverlaySpec>& overlays() const noexcept { return overlays_; }

    // ── Pure decision helpers (static, unit-testable on bounds/modal alone) ──

    // Index of the topmost *interactive* overlay whose bounds contain pt, else -1.
    // Non-interactive overlays (tooltips) are pointer-transparent and skipped.
    [[nodiscard]] static int topmostAt(const std::vector<OverlaySpec>& v,
                                       overlay::Point pt) noexcept {
        for (int i = static_cast<int>(v.size()) - 1; i >= 0; --i) {
            const auto& o = v[static_cast<size_t>(i)];
            if (o.interactive && overlay::contains(o.bounds, pt)) return i;
        }
        return -1;
    }

    // Index of the topmost interactive overlay (regardless of pt), else -1.
    [[nodiscard]] static int topmostInteractive(const std::vector<OverlaySpec>& v) noexcept {
        for (int i = static_cast<int>(v.size()) - 1; i >= 0; --i)
            if (v[static_cast<size_t>(i)].interactive) return i;
        return -1;
    }

    // Classify a pointer-down against the overlay stack. Tooltips are ignored
    // (clicks pass straight through them).
    [[nodiscard]] static ClickAction classifyClick(const std::vector<OverlaySpec>& v,
                                                    overlay::Point pt) noexcept {
        int top = topmostInteractive(v);
        if (top < 0) return ClickAction::PassToContent;          // none / only tooltips
        if (topmostAt(v, pt) >= 0) return ClickAction::PassToOverlay;
        return v[static_cast<size_t>(top)].modal ? ClickAction::SwallowModal
                                                 : ClickAction::DismissTop;
    }

private:
    std::vector<OverlaySpec> overlays_;
    uint64_t next_id_ = 1;
};

}  // namespace fbx::xkit
