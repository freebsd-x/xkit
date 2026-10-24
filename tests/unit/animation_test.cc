#include "xkit/animation.h"
#include <gtest/gtest.h>
#include <memory>

namespace fbx::xkit::testing {

class AnimationTest : public ::testing::Test {};

TEST_F(AnimationTest, LerpMathCorrectness) {
    // 1. Float Lerp
    EXPECT_FLOAT_EQ(Lerp<float>::compute(0.0f, 100.0f, 0.5f), 50.0f);
    EXPECT_FLOAT_EQ(Lerp<float>::compute(10.0f, 20.0f, 0.1f), 11.0f);

    // 2. Size Lerp
    Size s1{100, 100};
    Size s2{200, 300};
    Size res = Lerp<Size>::compute(s1, s2, 0.5f);
    EXPECT_FLOAT_EQ(res.width_, 150.0f);
    EXPECT_FLOAT_EQ(res.height_, 200.0f);

    // 3. Offset Lerp
    Offset o1{0, 0};
    Offset o2{100, 100};
    Offset resO = Lerp<Offset>::compute(o1, o2, 0.25f);
    EXPECT_FLOAT_EQ(resO.x_, 25.0f);
    EXPECT_FLOAT_EQ(resO.y_, 25.0f);
}

TEST_F(AnimationTest, TickerManagesLifecycle) {
    Ticker ticker;
    bool called = false;
    
    ticker.addAnimation([&called](float /*dt*/) {
        called = true;
        return true; // Finished immediately
    });

    EXPECT_TRUE(ticker.hasActiveAnimations());
    ticker.tick(0.016f);
    EXPECT_TRUE(called);
    EXPECT_FALSE(ticker.hasActiveAnimations());
}

TEST_F(AnimationTest, AnimatedSignalTransitionsOverTime) {
    auto ticker = std::make_shared<Ticker>();
    AnimatedSignal<float> signal(0.0f, ticker);

    // Animate from 0.0 to 100.0 over 1 second (linear ease not tested here, just end states)
    signal.animateTo(100.0f, 1.0f);

    // Frame 1: 0.5 seconds elapsed (t=0.5)
    // easeOut(0.5) = 1 - (1-0.5)^2 = 1 - 0.25 = 0.75
    ticker->tick(0.5f);
    EXPECT_FLOAT_EQ(signal.get(), 75.0f);

    // Frame 2: Another 0.5 seconds elapsed (t=1.0)
    ticker->tick(0.5f);
    EXPECT_FLOAT_EQ(signal.get(), 100.0f);

    // Verify it's removed from ticker
    EXPECT_FALSE(ticker->hasActiveAnimations());
}

TEST_F(AnimationTest, SpringSimulationOvershoots) {
    SpringSimulation spring(1.0f, 100.0f, 14.0f); // Underdamped (ζ≈0.7, settles in ~2s)
    
    float val = 0.0f;
    float target = 100.0f;
    float velocity = 0.0f;
    
    float max_val = 0.0f;
    
    // Simulate 2 seconds of spring physics at 60FPS
    for (int i = 0; i < 120; ++i) {
        val = spring.step(1.0f/60.0f, val, target, velocity);
        if (val > max_val) max_val = val;
    }
    
    // Because it's underdamped, it should overshoot the target
    EXPECT_GT(max_val, 100.0f);
    // But it should eventually settle near the target
    EXPECT_TRUE(spring.isAtRest(val, target, velocity));
}

} // namespace fbx::xkit::testing
