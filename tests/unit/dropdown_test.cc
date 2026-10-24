// Integration test for DropdownWidget on the overlay layer: a press opens an
// anchored options popup via OverlayManager; picking an option fires onChange and
// dismisses. Drives the element + widget logic directly (no compositor).

#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include "xkit/overlay.h"
#include "shared/protocol.h"
#include <gtest/gtest.h>
#include <memory>

using namespace fbx::xkit;
namespace shared = fbx::shared;

namespace {
shared::InputMessage mousePress(int x, int y) {
    shared::InputMessage m{};
    m.type_       = shared::InputEventType::MouseButton;
    m.char_value_ = 1;          // press (release is 0)
    m.value_a_    = x;
    m.value_b_    = y;
    return m;
}
}  // namespace

class DropdownTest : public ::testing::Test {
protected:
    void SetUp() override    { OverlayManager::instance().dismissAll(); }
    void TearDown() override { OverlayManager::instance().dismissAll(); }
};

TEST_F(DropdownTest, SelectedLabelFallsBackToPlaceholder) {
    DropdownWidget none({"A", "B"}, -1, nullptr, "Pick one");
    EXPECT_EQ(none.selectedLabel(), "Pick one");
    DropdownWidget sel({"A", "B"}, 1);
    EXPECT_EQ(sel.selectedLabel(), "B");
}

TEST_F(DropdownTest, PressOpensAnchoredPopupWithOptions) {
    auto dd = std::make_shared<DropdownWidget>(
        std::vector<std::string>{"Red", "Green", "Blue"}, 0);
    auto el = dd->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 500, 0, 500});

    ASSERT_TRUE(OverlayManager::instance().empty());
    EXPECT_TRUE(el->handleEvent(mousePress(10, 10)));       // press consumed

    auto& mgr = OverlayManager::instance();
    ASSERT_EQ(mgr.count(), 1u);
    const OverlaySpec& spec = mgr.overlays().back();
    EXPECT_EQ(spec.preferred, overlay::Side::Below);
    EXPECT_FLOAT_EQ(spec.anchor.x, 0.0f);                   // trigger at origin
    EXPECT_FLOAT_EQ(spec.anchor.y, 0.0f);

    // Popup content is a FlexColumn with one row per option.
    auto col = std::dynamic_pointer_cast<FlexColumn>(spec.content);
    ASSERT_TRUE(col);
    EXPECT_EQ(col->childCount(), 3u);
}

TEST_F(DropdownTest, SelectingOptionFiresOnChangeAndDismisses) {
    int chosen = -1;
    auto dd = std::make_shared<DropdownWidget>(
        std::vector<std::string>{"Red", "Green", "Blue"}, 0,
        [&](int i) { chosen = i; });
    auto el = dd->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 500, 0, 500});
    el->handleEvent(mousePress(10, 10));                    // open

    auto col = std::dynamic_pointer_cast<FlexColumn>(
        OverlayManager::instance().overlays().back().content);
    ASSERT_TRUE(col);
    auto optBlue = std::dynamic_pointer_cast<ButtonWidget>(col->childAt(2));
    ASSERT_TRUE(optBlue);
    ASSERT_TRUE(optBlue->onPressed());

    optBlue->onPressed()();                                 // pick "Blue"
    EXPECT_EQ(chosen, 2);
    EXPECT_TRUE(OverlayManager::instance().empty());        // popup dismissed
    EXPECT_EQ(dd->selected(), 2);                           // self-managed selection
    EXPECT_EQ(dd->selectedLabel(), "Blue");                 // reflects the pick live
}

TEST_F(DropdownTest, ReleaseDoesNotOpen) {
    auto dd = std::make_shared<DropdownWidget>(std::vector<std::string>{"A"}, 0);
    auto el = dd->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 500, 0, 500});

    shared::InputMessage release = mousePress(10, 10);
    release.char_value_ = 0;                                // release, not press
    el->handleEvent(release);
    EXPECT_TRUE(OverlayManager::instance().empty());        // only a press opens
}
