#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include <gtest/gtest.h>
#include <memory>

namespace fbx::xkit::testing {

class ScrollViewTest : public ::testing::Test {};

TEST_F(ScrollViewTest, ConstraintsAreInfiniteForChild) {
    // A ScrollView with max constraints 500x500
    // Its child is a FlexColumn containing two 300-height boxes (total 600)
    
    // We mock this by creating a RenderScrollView and a child that wants to be 1000 height
    auto scroll_node = std::make_shared<RenderScrollView>();
    auto child_node = std::make_shared<RenderBox>(100.0f, 1000.0f);
    scroll_node->addChild(child_node);

    Constraints c{0, 500, 0, 500};
    Size scroll_size = scroll_node->layout(c);

    // The ScrollView itself should clamp to its parent's constraints
    EXPECT_EQ(scroll_size.height_, 500.0f);

    // But the child should have been allowed to layout to its full 1000 height
    EXPECT_EQ(child_node->getSize().height_, 1000.0f);
}

TEST_F(ScrollViewTest, ScrollingTranslatesChildOffset) {
    auto scroll_node = std::make_shared<RenderScrollView>();
    auto child_node = std::make_shared<RenderBox>(100.0f, 1000.0f);
    scroll_node->addChild(child_node);

    Constraints c{0, 500, 0, 500};
    (void)scroll_node->layout(c);

    // Initial offset should be 0
    EXPECT_EQ(child_node->getOffset().y_, 0.0f);

    // Apply scroll
    scroll_node->setScrollY(150.0f);
    (void)scroll_node->layout(c); // Re-layout updates the offset

    // Child should be shifted up
    EXPECT_EQ(child_node->getOffset().y_, -150.0f);
}

} // namespace fbx::xkit::testing
