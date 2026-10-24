// Tests for modal Dialog on the overlay layer: shows a centered modal card;
// actions fire their callback and dismiss; confirm/alert compose correctly.

#include "xkit/dialog.h"
#include "xkit/overlay.h"
#include "xkit/widget.h"
#include <gtest/gtest.h>
#include <memory>

using namespace fbx::xkit;

class DialogTest : public ::testing::Test {
protected:
    void SetUp() override    { OverlayManager::instance().dismissAll(); }
    void TearDown() override { OverlayManager::instance().dismissAll(); }
};

TEST_F(DialogTest, ShowCreatesCenteredModalOverlay) {
    Dialog::show("Title", "Message", {DialogAction{"OK", nullptr, true}});
    auto& mgr = OverlayManager::instance();
    ASSERT_EQ(mgr.count(), 1u);
    const OverlaySpec& s = mgr.overlays().back();
    EXPECT_TRUE(s.centered);
    EXPECT_TRUE(s.modal);
    // Card = FlexColumn{ title, message, button-row }.
    auto card = std::dynamic_pointer_cast<FlexColumn>(s.content);
    ASSERT_TRUE(card);
    EXPECT_EQ(card->childCount(), 3u);
}

TEST_F(DialogTest, OutsideClickIsSwallowedByModal) {
    Dialog::alert("A", "B");
    // A click anywhere outside the (not-yet-laid-out, zero-bounds) card must be
    // swallowed, not dismiss — modal semantics.
    EXPECT_EQ(OverlayManager::classifyClick(OverlayManager::instance().overlays(),
                                            overlay::Point{5, 5}),
              ClickAction::SwallowModal);
}

TEST_F(DialogTest, ConfirmPrimaryActionFiresAndDismisses) {
    bool confirmed = false, cancelled = false;
    Dialog::confirm("Delete?", "This cannot be undone.", "Delete",
                    [&] { confirmed = true; }, [&] { cancelled = true; });

    auto& mgr = OverlayManager::instance();
    ASSERT_EQ(mgr.count(), 1u);
    auto card = std::dynamic_pointer_cast<FlexColumn>(mgr.overlays().back().content);
    ASSERT_TRUE(card);
    auto row = std::dynamic_pointer_cast<FlexRow>(card->childAt(2));
    ASSERT_TRUE(row);
    ASSERT_EQ(row->childCount(), 2u);           // Cancel + Delete

    auto deleteBtn = std::dynamic_pointer_cast<ButtonWidget>(row->childAt(1));
    ASSERT_TRUE(deleteBtn);
    ASSERT_TRUE(deleteBtn->onPressed());
    deleteBtn->onPressed()();                    // tap "Delete"

    EXPECT_TRUE(confirmed);
    EXPECT_FALSE(cancelled);
    EXPECT_TRUE(mgr.empty());                     // dialog dismissed
}

TEST_F(DialogTest, CancelFiresCancelCallback) {
    bool confirmed = false, cancelled = false;
    Dialog::confirm("Q", "M", "Yes", [&] { confirmed = true; }, [&] { cancelled = true; });
    auto card = std::dynamic_pointer_cast<FlexColumn>(
        OverlayManager::instance().overlays().back().content);
    auto row = std::dynamic_pointer_cast<FlexRow>(card->childAt(2));
    auto cancelBtn = std::dynamic_pointer_cast<ButtonWidget>(row->childAt(0));
    ASSERT_TRUE(cancelBtn);
    cancelBtn->onPressed()();                     // tap "Cancel"
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(confirmed);
    EXPECT_TRUE(OverlayManager::instance().empty());
}

TEST_F(DialogTest, AlertHasSingleButton) {
    Dialog::alert("Heads up", "Done.");
    auto card = std::dynamic_pointer_cast<FlexColumn>(
        OverlayManager::instance().overlays().back().content);
    auto row = std::dynamic_pointer_cast<FlexRow>(card->childAt(2));
    ASSERT_TRUE(row);
    EXPECT_EQ(row->childCount(), 1u);
}
