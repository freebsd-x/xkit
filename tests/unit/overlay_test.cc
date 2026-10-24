// Unit tests for overlay placement geometry + the overlay-stack model.

#include "xkit/overlay.h"
#include "xkit/overlay_layout.h"
#include "xkit/render_node.h"
#include "xkit/widget.h"
#include "xkit/dialog.h"
#include "xkit/menu.h"
#include "xrender/display_list.h"
#include "shared/protocol.h"
#include <gtest/gtest.h>
#include <cstring>
#include <memory>

using namespace fbx::xkit;
using overlay::Rect;
using overlay::Point;
using overlay::Side;

// ── placePopup ────────────────────────────────────────────────────────────────

TEST(OverlayLayout, BelowWithRoom) {
    // anchor at (10,10) 100x20 in a 500x500 viewport; popup 100x80.
    auto p = overlay::placePopup(Rect{10, 10, 100, 20}, 100, 80, 500, 500, Side::Below);
    EXPECT_EQ(p.side, Side::Below);
    EXPECT_FLOAT_EQ(p.x, 10.0f);            // left-aligned to anchor
    EXPECT_FLOAT_EQ(p.y, 10 + 20 + 4.0f);   // below anchor + gap
}

TEST(OverlayLayout, BelowFlipsAboveWhenNoRoom) {
    // anchor near the bottom: no room below, room above → flip.
    auto p = overlay::placePopup(Rect{10, 460, 100, 20}, 100, 80, 500, 500, Side::Below);
    EXPECT_EQ(p.side, Side::Above);
    EXPECT_FLOAT_EQ(p.y, 460 - 4.0f - 80);  // above anchor - gap - height
}

TEST(OverlayLayout, AbovePreferredWithRoom) {
    auto p = overlay::placePopup(Rect{10, 200, 100, 20}, 100, 80, 500, 500, Side::Above);
    EXPECT_EQ(p.side, Side::Above);
    EXPECT_FLOAT_EQ(p.y, 200 - 4.0f - 80);
}

TEST(OverlayLayout, HorizontalClampToViewport) {
    // anchor near right edge → popup would spill; x clamps to vw - popup_w.
    auto p = overlay::placePopup(Rect{460, 10, 30, 20}, 100, 40, 500, 500, Side::Below);
    EXPECT_FLOAT_EQ(p.x, 500 - 100);        // clamped
}

TEST(OverlayLayout, RightFlipsLeftWhenNoRoom) {
    auto p = overlay::placePopup(Rect{460, 10, 30, 20}, 100, 40, 500, 500, Side::Right);
    EXPECT_EQ(p.side, Side::Left);
    EXPECT_FLOAT_EQ(p.x, 460 - 4.0f - 100);
}

TEST(OverlayLayout, PopupLargerThanViewportPinsToOrigin) {
    auto p = overlay::placePopup(Rect{10, 10, 50, 20}, 600, 600, 500, 500, Side::Below);
    EXPECT_FLOAT_EQ(p.x, 0.0f);
    EXPECT_FLOAT_EQ(p.y, 0.0f);
}

TEST(OverlayLayout, Contains) {
    Rect r{10, 10, 100, 50};
    EXPECT_TRUE(overlay::contains(r, Point{50, 30}));
    EXPECT_FALSE(overlay::contains(r, Point{5, 30}));     // left of
    EXPECT_FALSE(overlay::contains(r, Point{110, 30}));   // right edge exclusive
}

// ── OverlayManager ────────────────────────────────────────────────────────────

static OverlaySpec boxed(Rect bounds, bool modal = false) {
    OverlaySpec s;
    s.bounds = bounds;
    s.modal  = modal;
    return s;
}

TEST(OverlayManager, ShowAssignsIncreasingIdsAndDismiss) {
    OverlayManager m;
    uint64_t a = m.show(boxed({0, 0, 10, 10}));
    uint64_t b = m.show(boxed({0, 0, 10, 10}));
    EXPECT_NE(a, b);
    EXPECT_EQ(m.count(), 2u);
    m.dismiss(a);
    EXPECT_EQ(m.count(), 1u);
    EXPECT_EQ(m.overlays().front().id, b);
}

TEST(OverlayManager, DismissFiresCallback) {
    OverlayManager m;
    bool fired = false;
    OverlaySpec s = boxed({0, 0, 10, 10});
    s.on_dismiss = [&] { fired = true; };
    uint64_t id = m.show(std::move(s));
    m.dismiss(id);
    EXPECT_TRUE(fired);
    EXPECT_TRUE(m.empty());
}

TEST(OverlayManager, DismissAll) {
    OverlayManager m;
    m.show(boxed({0, 0, 10, 10}));
    m.show(boxed({0, 0, 10, 10}));
    m.dismissAll();
    EXPECT_TRUE(m.empty());
}

TEST(OverlayManager, TopmostAtReturnsLastOverlapping) {
    OverlayManager m;
    m.show(boxed({0, 0, 100, 100}));      // index 0
    m.show(boxed({50, 50, 100, 100}));    // index 1 (on top)
    // point in the overlap → topmost (index 1)
    EXPECT_EQ(OverlayManager::topmostAt(m.overlays(), Point{60, 60}), 1);
    // point only in the bottom one
    EXPECT_EQ(OverlayManager::topmostAt(m.overlays(), Point{10, 10}), 0);
    // point in neither
    EXPECT_EQ(OverlayManager::topmostAt(m.overlays(), Point{300, 300}), -1);
}

TEST(OverlayManager, ClassifyClick) {
    OverlayManager m;
    // empty stack
    EXPECT_EQ(OverlayManager::classifyClick(m.overlays(), Point{5, 5}),
              ClickAction::PassToContent);
    // non-modal overlay
    m.show(boxed({100, 100, 50, 50}, /*modal=*/false));
    EXPECT_EQ(OverlayManager::classifyClick(m.overlays(), Point{120, 120}),
              ClickAction::PassToOverlay);   // inside
    EXPECT_EQ(OverlayManager::classifyClick(m.overlays(), Point{5, 5}),
              ClickAction::DismissTop);       // outside non-modal
    // modal on top
    m.show(boxed({200, 200, 50, 50}, /*modal=*/true));
    EXPECT_EQ(OverlayManager::classifyClick(m.overlays(), Point{5, 5}),
              ClickAction::SwallowModal);     // outside modal → blocked
    EXPECT_EQ(OverlayManager::classifyClick(m.overlays(), Point{210, 210}),
              ClickAction::PassToOverlay);     // inside modal
}

TEST(OverlayManager, NonInteractiveOverlayIsPointerTransparent) {
    OverlayManager m;
    OverlaySpec tip = boxed({100, 100, 80, 30});
    tip.interactive = false;                    // a tooltip
    m.show(std::move(tip));
    // A click on the tooltip is not a hit, and with no interactive overlays the
    // click passes through to content — the tooltip is never click-dismissed.
    EXPECT_EQ(OverlayManager::topmostAt(m.overlays(), Point{120, 110}), -1);
    EXPECT_EQ(OverlayManager::classifyClick(m.overlays(), Point{120, 110}),
              ClickAction::PassToContent);
    EXPECT_EQ(OverlayManager::classifyClick(m.overlays(), Point{5, 5}),
              ClickAction::PassToContent);
}

TEST(OverlayManager, TooltipOverPopupStillTargetsPopup) {
    OverlayManager m;
    m.show(boxed({0, 0, 100, 100}));            // interactive popup (index 0)
    OverlaySpec tip = boxed({0, 0, 100, 100});  // tooltip on top, same area
    tip.interactive = false;
    m.show(std::move(tip));                      // index 1
    // The click resolves to the interactive popup beneath the tooltip.
    EXPECT_EQ(OverlayManager::topmostAt(m.overlays(), Point{50, 50}), 0);
    EXPECT_EQ(OverlayManager::classifyClick(m.overlays(), Point{50, 50}),
              ClickAction::PassToOverlay);
    // Outside the popup → dismiss the popup (not the tooltip).
    EXPECT_EQ(OverlayManager::classifyClick(m.overlays(), Point{500, 500}),
              ClickAction::DismissTop);
    EXPECT_EQ(OverlayManager::topmostInteractive(m.overlays()), 0);
}

// ── Tooltip show decision (pure) ──────────────────────────────────────────────

TEST(Tooltip, ShouldShowTiming) {
    // hovering long enough, not shown, not suppressed → show
    EXPECT_TRUE(fbx::xkit::TooltipWidget::shouldShow(true, false, false, 600, 500));
    // not hovered long enough
    EXPECT_FALSE(fbx::xkit::TooltipWidget::shouldShow(true, false, false, 300, 500));
    // not hovering
    EXPECT_FALSE(fbx::xkit::TooltipWidget::shouldShow(false, false, false, 999, 500));
    // already shown
    EXPECT_FALSE(fbx::xkit::TooltipWidget::shouldShow(true, true, false, 999, 500));
    // suppressed (clicked)
    EXPECT_FALSE(fbx::xkit::TooltipWidget::shouldShow(true, false, true, 999, 500));
}

// ── RenderOverlayHost integration: content fills, overlay positioned ──────────

TEST(OverlayHostLayout, ContentFillsAndOverlayPlaced) {
    auto host    = std::make_shared<RenderOverlayHost>();
    auto content = std::make_shared<RenderBox>(0.0f, 0.0f);      // fills viewport
    auto popup   = std::make_shared<RenderBox>(100.0f, 80.0f);   // fixed popup size
    host->addChild(content);
    host->addChild(popup);
    host->placement_.push_back({Rect{10, 10, 100, 20}, Side::Below});

    (void)host->layout(Constraints{0, 500, 0, 500});

    // content stretched to the full viewport at the origin
    EXPECT_FLOAT_EQ(content->getSize().width_,  500.0f);
    EXPECT_FLOAT_EQ(content->getSize().height_, 500.0f);
    EXPECT_FLOAT_EQ(content->getOffset().x_, 0.0f);
    EXPECT_FLOAT_EQ(content->getOffset().y_, 0.0f);

    // popup placed just below the anchor, left-aligned
    EXPECT_FLOAT_EQ(popup->getOffset().x_, 10.0f);
    EXPECT_FLOAT_EQ(popup->getOffset().y_, 10.0f + 20.0f + 4.0f);

    // resolved bounds recorded for hit-testing
    ASSERT_EQ(host->resolved_.size(), 1u);
    EXPECT_FLOAT_EQ(host->resolved_[0].w, 100.0f);
    EXPECT_FLOAT_EQ(host->resolved_[0].h, 80.0f);
    EXPECT_FLOAT_EQ(host->resolved_[0].y, 34.0f);
}

TEST(OverlayHostLayout, CenteredOverlayCentersInViewport) {
    auto host    = std::make_shared<RenderOverlayHost>();
    auto content = std::make_shared<RenderBox>(0.0f, 0.0f);
    auto card    = std::make_shared<RenderBox>(200.0f, 100.0f);
    host->addChild(content);
    host->addChild(card);
    host->placement_.push_back({Rect{0, 0, 0, 0}, Side::Below,
                                /*centered=*/true, /*bottom=*/false, /*modal=*/true});

    (void)host->layout(Constraints{0, 500, 0, 400});
    EXPECT_FLOAT_EQ(card->getOffset().x_, (500.0f - 200.0f) / 2.0f);  // 150
    EXPECT_FLOAT_EQ(card->getOffset().y_, (400.0f - 100.0f) / 2.0f);  // 150
}

TEST(OverlayHostLayout, OverlayFlipsAboveNearBottomEdge) {
    auto host    = std::make_shared<RenderOverlayHost>();
    auto content = std::make_shared<RenderBox>(0.0f, 0.0f);
    auto popup   = std::make_shared<RenderBox>(100.0f, 80.0f);
    host->addChild(content);
    host->addChild(popup);
    host->placement_.push_back({Rect{10, 460, 100, 20}, Side::Below});  // near bottom

    (void)host->layout(Constraints{0, 500, 0, 500});
    // no room below → flipped above the anchor
    EXPECT_FLOAT_EQ(popup->getOffset().y_, 460.0f - 4.0f - 80.0f);
}

// ── Overlay rendering (draw-command level) ────────────────────────────────────

namespace {
std::vector<fbx::shared::RectData> ovRects(const fbx::xrender::DisplayList& dl) {
    std::vector<fbx::shared::RectData> out;
    auto cmds = dl.getCommands();
    auto buf  = dl.getDataBuffer();
    for (const auto& c : cmds) {
        if (c.type_ != fbx::shared::CommandType::DrawRect) continue;
        if (c.data_offset_ + sizeof(fbx::shared::RectData) > buf.size()) continue;
        fbx::shared::RectData r{};
        std::memcpy(&r, buf.data() + c.data_offset_, sizeof(r));
        out.push_back(r);
    }
    return out;
}
// Paint a standalone content widget (overlay card) into dl.
void paintContent(const std::shared_ptr<fbx::xkit::Widget>& w,
                  fbx::xrender::DisplayList& dl) {
    auto el = w->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(fbx::xkit::Constraints{0, 500, 0, 500});
    el->getRenderNode()->paint(dl, {0.0f, 0.0f});
}
bool nearf(float a, float b, float e = 0.6f) { return std::abs(a - b) < e; }
}  // namespace

TEST(OverlayRender, ModalHostPaintsFullViewportScrim) {
    auto host    = std::make_shared<RenderOverlayHost>();
    auto content = std::make_shared<RenderBox>(0.0f, 0.0f);
    auto card    = std::make_shared<RenderBox>(120.0f, 80.0f);
    host->addChild(content);
    host->addChild(card);
    host->placement_.push_back({Rect{0,0,0,0}, Side::Below,
                                /*centered=*/true, /*bottom=*/false, /*modal=*/true});
    (void)host->layout(Constraints{0, 500, 0, 400});

    fbx::xrender::DisplayList dl;
    host->paint(dl, {0.0f, 0.0f});
    // A near-opaque-ish dark scrim covering the whole viewport must be emitted.
    bool scrim = false;
    for (const auto& r : ovRects(dl))
        if (nearf(r.width_, 500) && nearf(r.height_, 400) &&
            nearf(r.r_, 0) && nearf(r.g_, 0) && nearf(r.b_, 0) &&
            r.a_ > 0.2f && r.a_ < 0.9f) { scrim = true; break; }
    EXPECT_TRUE(scrim) << "no modal scrim rect painted";
}

TEST(OverlayRender, NonModalHostPaintsNoScrim) {
    auto host    = std::make_shared<RenderOverlayHost>();
    auto content = std::make_shared<RenderBox>(0.0f, 0.0f);
    auto pop     = std::make_shared<RenderBox>(120.0f, 80.0f);
    host->addChild(content);
    host->addChild(pop);
    host->placement_.push_back({Rect{10,10,50,20}, Side::Below,
                                /*centered=*/false, /*bottom=*/false, /*modal=*/false});
    (void)host->layout(Constraints{0, 500, 0, 400});

    fbx::xrender::DisplayList dl;
    host->paint(dl, {0.0f, 0.0f});
    for (const auto& r : ovRects(dl))
        EXPECT_FALSE(nearf(r.width_, 500) && nearf(r.height_, 400) && r.a_ > 0.2f && r.a_ < 0.9f)
            << "unexpected full-viewport scrim for a non-modal overlay";
}

TEST(OverlayRender, DialogCardPaintsRoundedSurface) {
    OverlayManager::instance().dismissAll();
    Dialog::alert("Title", "Message body");
    auto card = OverlayManager::instance().overlays().back().content;
    ASSERT_TRUE(card);
    fbx::xrender::DisplayList dl;
    paintContent(card, dl);
    // The dialog card sets borderRadius(12) → a rounded rect is emitted.
    bool rounded = false;
    for (const auto& r : ovRects(dl))
        if (nearf(r.border_radius_, 12) && r.width_ > 0 && r.height_ > 0) { rounded = true; break; }
    EXPECT_TRUE(rounded) << "dialog card's rounded surface not painted";
    OverlayManager::instance().dismissAll();
}
