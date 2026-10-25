#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include <gtest/gtest.h>
#include <memory>

namespace fbx::xkit::testing {

class AdaptiveTest : public ::testing::Test {};

// RenderAdaptive shows the child with the largest min-width that fits the
// available width; the rest collapse to 0x0 so paint/hit-test/events skip them.
TEST_F(AdaptiveTest, PicksChildByAvailableWidth) {
    auto node    = std::make_shared<RenderAdaptive>();
    auto compact = std::make_shared<RenderBox>(50.0f, 20.0f);   // breakpoint 0
    auto wide    = std::make_shared<RenderBox>(80.0f, 40.0f);   // breakpoint 900
    node->addChild(compact);
    node->addChild(wide);
    node->setMinWidths({0.0f, 900.0f});

    // Narrow viewport (< 900) -> compact is active, wide collapsed.
    (void)node->layout(Constraints{0, 500, 0, 1000});
    EXPECT_EQ(node->activeIndex(), 0);
    EXPECT_GT(compact->getSize().width_, 0.0f);
    EXPECT_EQ(wide->getSize().width_, 0.0f);

    // Wide viewport (>= 900) -> wide is active, compact collapsed.
    (void)node->layout(Constraints{0, 1200, 0, 1000});
    EXPECT_EQ(node->activeIndex(), 1);
    EXPECT_GT(wide->getSize().width_, 0.0f);
    EXPECT_EQ(compact->getSize().width_, 0.0f);

    // Exactly at the breakpoint (900) -> wide (min_width <= avail is inclusive).
    (void)node->layout(Constraints{0, 900, 0, 1000});
    EXPECT_EQ(node->activeIndex(), 1);
}

// Below every breakpoint, the first entry is the fallback.
TEST_F(AdaptiveTest, FallsBackToFirstBelowAllBreakpoints) {
    auto node = std::make_shared<RenderAdaptive>();
    node->addChild(std::make_shared<RenderBox>(10.0f, 10.0f));
    node->addChild(std::make_shared<RenderBox>(10.0f, 10.0f));
    node->setMinWidths({300.0f, 900.0f});   // note: no 0 entry

    (void)node->layout(Constraints{0, 100, 0, 100});   // 100 < 300
    EXPECT_EQ(node->activeIndex(), 0);
}

// Three tiers: compact / medium / wide.
TEST_F(AdaptiveTest, PicksLargestFittingOfThree) {
    auto node = std::make_shared<RenderAdaptive>();
    for (int i = 0; i < 3; ++i) node->addChild(std::make_shared<RenderBox>(10.0f, 10.0f));
    node->setMinWidths({0.0f, 600.0f, 1000.0f});

    (void)node->layout(Constraints{0, 400,  0, 100}); EXPECT_EQ(node->activeIndex(), 0);
    (void)node->layout(Constraints{0, 700,  0, 100}); EXPECT_EQ(node->activeIndex(), 1);
    (void)node->layout(Constraints{0, 1400, 0, 100}); EXPECT_EQ(node->activeIndex(), 2);
}

} // namespace fbx::xkit::testing
