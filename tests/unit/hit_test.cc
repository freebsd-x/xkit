#include "xkit/render_node.h"
#include <gtest/gtest.h>
#include <memory>

namespace fbx::xkit::testing {

class HitTestTest : public ::testing::Test {};

TEST_F(HitTestTest, RecursiveHitTestCalculatesCorrectNode) {
    // Construct a simple tree:
    // Root (200x200)
    //  +- Child A (50x50 at 0,0)
    //  +- Child B (50x50 at 100,100)

    auto root = std::make_shared<RenderBox>(200.0f, 200.0f);
    auto childA = std::make_shared<RenderBox>(50.0f, 50.0f);
    auto childB = std::make_shared<RenderBox>(50.0f, 50.0f);

    childA->setOffset({0.0f, 0.0f});
    childB->setOffset({100.0f, 100.0f});

    root->addChild(childA);
    root->addChild(childB);

    Constraints c{0, 200, 0, 200};
    (void)root->layout(c); // Force size calculation

    // Test 1: Hit Child A
    auto hit1 = root->hitTest({10.0f, 10.0f});
    EXPECT_EQ(hit1, childA);

    // Test 2: Hit Child B
    auto hit2 = root->hitTest({110.0f, 110.0f});
    EXPECT_EQ(hit2, childB);

    // Test 3: Hit Root (background)
    auto hit3 = root->hitTest({75.0f, 75.0f});
    EXPECT_EQ(hit3, root);

    // Test 4: Miss entirely
    auto hit4 = root->hitTest({300.0f, 300.0f});
    EXPECT_EQ(hit4, nullptr);
}

TEST_F(HitTestTest, ZOrderReverseTraversal) {
    // Overlapping boxes to test Z-order (last child added should be on top)
    auto root = std::make_shared<RenderBox>(100.0f, 100.0f);
    auto bottom = std::make_shared<RenderBox>(50.0f, 50.0f);
    auto top = std::make_shared<RenderBox>(50.0f, 50.0f);

    bottom->setOffset({0,0});
    top->setOffset({0,0});

    root->addChild(bottom);
    root->addChild(top);

    Constraints c{0, 100, 0, 100};
    (void)root->layout(c);

    // Coordinate (10,10) is inside both. We expect 'top' because of reverse traversal.
    auto hit = root->hitTest({10.0f, 10.0f});
    EXPECT_EQ(hit, top);
}

} // namespace fbx::xkit::testing
