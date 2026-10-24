#include "xkit/layout.h"
#include <gtest/gtest.h>

namespace fbx::xkit::testing {

class LayoutMathTest : public ::testing::Test {};

TEST_F(LayoutMathTest, SizeEquality) {
    Size a{100.0f, 200.0f};
    Size b{100.0f, 200.0f};
    Size c{100.1f, 200.0f};

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST_F(LayoutMathTest, OffsetEquality) {
    Offset a{10.0f, 20.0f};
    Offset b{10.0f, 20.0f};
    Offset c{15.0f, 20.0f};

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST_F(LayoutMathTest, ConstraintsClamp) {
    Constraints constraints{
        .min_width_ = 100.0f,
        .max_width_ = 500.0f,
        .min_height_ = 50.0f,
        .max_height_ = 250.0f
    };

    // Test within bounds
    Size normal{200.0f, 100.0f};
    Size clampedNormal = constraints.clamp(normal);
    EXPECT_EQ(clampedNormal.width_, 200.0f);
    EXPECT_EQ(clampedNormal.height_, 100.0f);

    // Test under min bounds
    Size tooSmall{50.0f, 20.0f};
    Size clampedSmall = constraints.clamp(tooSmall);
    EXPECT_EQ(clampedSmall.width_, 100.0f);
    EXPECT_EQ(clampedSmall.height_, 50.0f);

    // Test over max bounds
    Size tooBig{1000.0f, 800.0f};
    Size clampedBig = constraints.clamp(tooBig);
    EXPECT_EQ(clampedBig.width_, 500.0f);
    EXPECT_EQ(clampedBig.height_, 250.0f);
}

TEST_F(LayoutMathTest, ConstraintsConstrain) {
    Constraints parent{
        .min_width_ = 100.0f,
        .max_width_ = 500.0f,
        .min_height_ = 100.0f,
        .max_height_ = 500.0f
    };

    // Child is fully within parent constraints
    Constraints child1{
        .min_width_ = 200.0f,
        .max_width_ = 300.0f,
        .min_height_ = 200.0f,
        .max_height_ = 300.0f
    };
    Constraints result1 = parent.constrain(child1);
    EXPECT_EQ(result1.min_width_, 200.0f);
    EXPECT_EQ(result1.max_width_, 300.0f);

    // Child is completely unconstrained (wants to be smaller or larger)
    Constraints child2{
        .min_width_ = 0.0f,
        .max_width_ = 1000.0f,
        .min_height_ = 0.0f,
        .max_height_ = 1000.0f
    };
    Constraints result2 = parent.constrain(child2);
    // Should be clamped to parent's constraints
    EXPECT_EQ(result2.min_width_, 100.0f);
    EXPECT_EQ(result2.max_width_, 500.0f);
    EXPECT_EQ(result2.min_height_, 100.0f);
    EXPECT_EQ(result2.max_height_, 500.0f);
}

// Compile-time checks (if they fail to compile, the test fails)
static_assert(Size{10.0f, 10.0f} == Size{10.0f, 10.0f}, "Constexpr Size equality failed");
static_assert(Offset{5.0f, 5.0f} == Offset{5.0f, 5.0f}, "Constexpr Offset equality failed");

} // namespace fbx::xkit::testing
