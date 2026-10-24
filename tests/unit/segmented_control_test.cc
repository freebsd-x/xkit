// Tests for SegmentedControl: self-managed selection + tap-selects-segment.

#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include "xkit/application.h"
#include "shared/protocol.h"
#include <gtest/gtest.h>
#include <memory>

using namespace fbx::xkit;
namespace shared = fbx::shared;

namespace {
shared::InputMessage press(int x, int y) {
    shared::InputMessage m{};
    m.type_       = shared::InputEventType::MouseButton;
    m.char_value_ = 1;
    m.value_a_    = x;
    m.value_b_    = y;
    m.modifiers_  = 272u << 16;   // left button
    return m;
}
constexpr float kW = 90.0f;   // default segment width
}  // namespace

TEST(SegmentedControl, SelectedLabel) {
    SegmentedControlWidget s({"A", "B", "C"}, 1);
    EXPECT_EQ(s.selectedLabel(), "B");
    EXPECT_EQ(s.selected(), 1);
}

TEST(SegmentedControl, TapSelectsSegmentAndFiresOnChange) {
    int chosen = -1;
    auto sc = std::make_shared<SegmentedControlWidget>(
        std::vector<std::string>{"Blue", "Purple", "Orange"}, 0,
        [&](int i) { chosen = i; });
    auto el = sc->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 500, 0, 100});

    // Segment i spans [i*90, (i+1)*90); tap the centre of segment 2 ("Orange").
    EXPECT_TRUE(el->handleEvent(press(static_cast<int>(2 * kW + kW / 2), 14)));
    EXPECT_EQ(chosen, 2);
    EXPECT_EQ(sc->selected(), 2);              // self-managed
    EXPECT_EQ(sc->selectedLabel(), "Orange");
}

TEST(SegmentedControl, TapFirstSegment) {
    int chosen = -1;
    auto sc = std::make_shared<SegmentedControlWidget>(
        std::vector<std::string>{"One", "Two"}, 1, [&](int i) { chosen = i; });
    auto el = sc->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 500, 0, 100});

    EXPECT_TRUE(el->handleEvent(press(static_cast<int>(kW / 2), 14)));   // segment 0
    EXPECT_EQ(chosen, 0);
    EXPECT_EQ(sc->selected(), 0);
}

TEST(SegmentedControl, TapOutsideAllSegmentsDoesNothing) {
    int chosen = -1;
    auto sc = std::make_shared<SegmentedControlWidget>(
        std::vector<std::string>{"One", "Two"}, 0, [&](int i) { chosen = i; });
    auto el = sc->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 500, 0, 100});

    EXPECT_FALSE(el->handleEvent(press(1000, 14)));   // far past the last segment
    EXPECT_EQ(chosen, -1);
    EXPECT_EQ(sc->selected(), 0);
}
