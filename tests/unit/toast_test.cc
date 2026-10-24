// Tests for Toast: transient bottom-centre overlay that auto-dismisses.

#include "xkit/toast.h"
#include "xkit/overlay.h"
#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include <gtest/gtest.h>
#include <chrono>
#include <memory>

using namespace fbx::xkit;

class ToastTest : public ::testing::Test {
protected:
    void SetUp() override    { OverlayManager::instance().dismissAll(); }
    void TearDown() override { OverlayManager::instance().dismissAll(); }
};

TEST_F(ToastTest, ShowCreatesBottomTransparentTransientOverlay) {
    auto before = std::chrono::steady_clock::now();
    Toast::show("Saved", 2500);
    auto& mgr = OverlayManager::instance();
    ASSERT_EQ(mgr.count(), 1u);
    const OverlaySpec& s = mgr.overlays().back();
    EXPECT_TRUE(s.bottom);
    EXPECT_FALSE(s.interactive);          // pointer-transparent
    EXPECT_FALSE(s.modal);
    EXPECT_GT(s.expires, before);         // auto-dismiss scheduled in the future
    EXPECT_TRUE(std::dynamic_pointer_cast<FlexColumn>(s.content));
}

TEST_F(ToastTest, PointerTransparentPassesClicksThrough) {
    Toast::show("hi");
    // A lone toast leaves clicks going to content (never eats or dismisses on click).
    EXPECT_EQ(OverlayManager::classifyClick(OverlayManager::instance().overlays(),
                                            overlay::Point{10, 10}),
              ClickAction::PassToContent);
}

TEST_F(ToastTest, AutoDismissesWhenExpired) {
    // Mount a host (as XApplication::Run does) so its syncState runs the expiry sweep.
    auto host = std::make_shared<OverlayHostWidget>(
        std::make_shared<TextWidget>("app", 13.0f));
    auto el = host->createElement();
    el->mount(nullptr);

    Toast::show("bye", 2500);
    ASSERT_EQ(OverlayManager::instance().count(), 1u);

    // Force it past its expiry, then tick the host.
    OverlayManager::instance().overlays().back().expires =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    el->syncState();
    EXPECT_TRUE(OverlayManager::instance().empty());
}

TEST_F(ToastTest, UnexpiredToastSurvivesTick) {
    auto host = std::make_shared<OverlayHostWidget>(
        std::make_shared<TextWidget>("app", 13.0f));
    auto el = host->createElement();
    el->mount(nullptr);

    Toast::show("stay", 5000);
    el->syncState();
    EXPECT_EQ(OverlayManager::instance().count(), 1u);   // not yet expired
}

TEST_F(ToastTest, BottomPlacementPinsToBottomCentre) {
    auto host = std::make_shared<RenderOverlayHost>();
    auto content = std::make_shared<RenderBox>(0.0f, 0.0f);
    auto pill    = std::make_shared<RenderBox>(120.0f, 40.0f);
    host->addChild(content);
    host->addChild(pill);
    host->placement_.push_back({overlay::Rect{0,0,0,0}, overlay::Side::Below,
                                /*centered=*/false, /*bottom=*/true, /*modal=*/false});

    (void)host->layout(Constraints{0, 500, 0, 400});
    EXPECT_FLOAT_EQ(pill->getOffset().x_, (500.0f - 120.0f) / 2.0f);   // centred x
    EXPECT_FLOAT_EQ(pill->getOffset().y_, 400.0f - 40.0f - 24.0f);     // bottom - margin
}
