#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include <gtest/gtest.h>
#include <memory>

namespace fbx::xkit::testing {

class TouchTargetTest : public ::testing::Test {};

// A small icon child in a button floors to the min touch size in BOTH dims.
TEST_F(TouchTargetTest, ButtonFloorsToMinTouchSize) {
    auto icon = std::make_shared<ContainerWidget>(nullptr);
    icon->setFixedSize(16.0f, 16.0f);
    auto btn = std::make_shared<ButtonWidget>(icon, [](){});
    btn->setMinTouchSize(44.0f);

    auto el = btn->createElement(); el->mount(nullptr);
    Size s = el->getRenderNode()->layout(Constraints{0, 1000, 0, 1000});
    EXPECT_GE(s.width_,  44.0f);
    EXPECT_GE(s.height_, 44.0f);
}

// Without a min touch size, a small icon button stays compact (shrink-wrapped
// width is below 44) — proves the floor is what enlarges it.
TEST_F(TouchTargetTest, WithoutMinTouchButtonStaysCompact) {
    auto icon = std::make_shared<ContainerWidget>(nullptr);
    icon->setFixedSize(16.0f, 16.0f);
    auto btn = std::make_shared<ButtonWidget>(icon, [](){});

    auto el = btn->createElement(); el->mount(nullptr);
    Size s = el->getRenderNode()->layout(Constraints{0, 1000, 0, 1000});
    EXPECT_LT(s.width_, 44.0f);
}

// An explicit fixed size still wins over the min touch size (deliberate layout
// isn't overridden).
TEST_F(TouchTargetTest, FixedSizeWinsOverMinTouch) {
    auto icon = std::make_shared<ContainerWidget>(nullptr);
    icon->setFixedSize(16.0f, 16.0f);
    auto btn = std::make_shared<ButtonWidget>(icon, [](){});
    btn->setMinTouchSize(44.0f);
    btn->setFixedSize(28.0f, 28.0f);

    auto el = btn->createElement(); el->mount(nullptr);
    Size s = el->getRenderNode()->layout(Constraints{0, 1000, 0, 1000});
    EXPECT_FLOAT_EQ(s.width_,  28.0f);
    EXPECT_FLOAT_EQ(s.height_, 28.0f);
}

} // namespace fbx::xkit::testing
