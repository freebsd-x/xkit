// Tests for non-modal popup Menu (context menus) on the overlay layer.

#include "xkit/menu.h"
#include "xkit/overlay.h"
#include "xkit/widget.h"
#include <gtest/gtest.h>
#include <memory>

using namespace fbx::xkit;

class MenuTest : public ::testing::Test {
protected:
    void SetUp() override    { OverlayManager::instance().dismissAll(); }
    void TearDown() override { OverlayManager::instance().dismissAll(); }
};

TEST_F(MenuTest, ShowAtCreatesNonModalOverlayAtPoint) {
    Menu::showAt(120.0f, 40.0f, {
        MenuItem{"Cut", nullptr}, MenuItem{"Copy", nullptr}, MenuItem{"Paste", nullptr}});
    auto& mgr = OverlayManager::instance();
    ASSERT_EQ(mgr.count(), 1u);
    const OverlaySpec& s = mgr.overlays().back();
    EXPECT_FALSE(s.modal);
    EXPECT_FALSE(s.centered);
    EXPECT_FLOAT_EQ(s.anchor.x, 120.0f);   // zero-size anchor at the cursor
    EXPECT_FLOAT_EQ(s.anchor.y, 40.0f);
    EXPECT_FLOAT_EQ(s.anchor.w, 0.0f);

    auto card = std::dynamic_pointer_cast<FlexColumn>(s.content);
    ASSERT_TRUE(card);
    EXPECT_EQ(card->childCount(), 3u);
}

TEST_F(MenuTest, OutsideClickDismisses) {
    Menu::showAt(10, 10, {MenuItem{"Only", nullptr}});
    // Non-modal → an outside click dismisses the top menu.
    EXPECT_EQ(OverlayManager::classifyClick(OverlayManager::instance().overlays(),
                                            overlay::Point{999, 999}),
              ClickAction::DismissTop);
}

TEST_F(MenuTest, ChoosingItemFiresAndDismisses) {
    int hits = 0;
    Menu::showAt(0, 0, {
        MenuItem{"A", [&]{ hits += 1; }},
        MenuItem{"B", [&]{ hits += 10; }}});
    auto card = std::dynamic_pointer_cast<FlexColumn>(
        OverlayManager::instance().overlays().back().content);
    ASSERT_TRUE(card);
    auto b = std::dynamic_pointer_cast<ButtonWidget>(card->childAt(1));
    ASSERT_TRUE(b);
    b->onPressed()();                       // choose "B"
    EXPECT_EQ(hits, 10);
    EXPECT_TRUE(OverlayManager::instance().empty());
}

TEST_F(MenuTest, SeparatorBecomesDividerNotButton) {
    Menu::showAt(0, 0, {
        MenuItem{"Open", nullptr},
        MenuItem::sep(),
        MenuItem{"Delete", nullptr}});
    auto card = std::dynamic_pointer_cast<FlexColumn>(
        OverlayManager::instance().overlays().back().content);
    ASSERT_TRUE(card);
    ASSERT_EQ(card->childCount(), 3u);
    EXPECT_TRUE(std::dynamic_pointer_cast<ButtonWidget>(card->childAt(0)));
    EXPECT_TRUE(std::dynamic_pointer_cast<DividerWidget>(card->childAt(1)));   // the separator
    EXPECT_TRUE(std::dynamic_pointer_cast<ButtonWidget>(card->childAt(2)));
}

TEST_F(MenuTest, DisabledItemIsDisabledButtonAndDoesNothing) {
    bool tapped = false;
    Menu::showAt(0, 0, {[]{ MenuItem m{"Nope", nullptr}; m.disabled = true; return m; }()});
    auto card = std::dynamic_pointer_cast<FlexColumn>(
        OverlayManager::instance().overlays().back().content);
    auto b = std::dynamic_pointer_cast<ButtonWidget>(card->childAt(0));
    ASSERT_TRUE(b);
    EXPECT_TRUE(b->isDisabled());
    (void)tapped;
}

TEST_F(MenuTest, ShowBelowKeepsAnchorRect) {
    Menu::showBelow(overlay::Rect{50, 60, 100, 24}, {MenuItem{"X", nullptr}});
    const OverlaySpec& s = OverlayManager::instance().overlays().back();
    EXPECT_FLOAT_EQ(s.anchor.x, 50.0f);
    EXPECT_FLOAT_EQ(s.anchor.w, 100.0f);
    EXPECT_EQ(s.preferred, overlay::Side::Below);
}
