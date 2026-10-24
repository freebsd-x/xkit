// Tests for StepperWidget: pure value transitions (clamp/step/bounds + formatting)
// and the element's ±-zone click routing with the onChanged callback.

#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include "shared/protocol.h"
#include <gtest/gtest.h>
#include <memory>

using namespace fbx::xkit;
namespace shared = fbx::shared;

namespace {
shared::InputMessage press(int x, int y) {
    shared::InputMessage m{};
    m.type_ = shared::InputEventType::MouseButton;
    m.char_value_ = 1;             // button down
    m.value_a_ = x;
    m.value_b_ = y;
    m.modifiers_ = 272u << 16;     // left button
    return m;
}
// Default geometry: width 140, buttonWidth 36, height 32.
std::shared_ptr<Element> mounted(const std::shared_ptr<StepperWidget>& s) {
    auto el = s->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 400, 0, 200});
    return el;
}
}  // namespace

// ── pure value logic ──────────────────────────────────────────────────────────

TEST(Stepper, ClampsInitialValue) {
    EXPECT_FLOAT_EQ(std::make_shared<StepperWidget>(99, 0, 10)->value(), 10.0f);
    EXPECT_FLOAT_EQ(std::make_shared<StepperWidget>(-5, 0, 10)->value(), 0.0f);
}

TEST(Stepper, StepUpDownClampAndReportMovement) {
    auto s = std::make_shared<StepperWidget>(9, 0, 10, 1);
    EXPECT_TRUE(s->stepUp());             // 9 → 10
    EXPECT_FLOAT_EQ(s->value(), 10.0f);
    EXPECT_FALSE(s->stepUp());            // already at max — no move
    EXPECT_FLOAT_EQ(s->value(), 10.0f);
    EXPECT_TRUE(s->atMax());

    auto t = std::make_shared<StepperWidget>(0, 0, 10, 4);
    EXPECT_TRUE(t->stepUp());             // 0 → 4
    EXPECT_FLOAT_EQ(t->value(), 4.0f);
    EXPECT_TRUE(t->stepDown());           // 4 → 0 moves
    EXPECT_FLOAT_EQ(t->value(), 0.0f);
    EXPECT_FALSE(t->stepDown());          // at min — no move
    EXPECT_TRUE(t->atMin());
}

TEST(Stepper, GuardsInvertedAndDegenerateRange) {
    auto inv = std::make_shared<StepperWidget>(5, 10, 0);   // max<min
    EXPECT_FLOAT_EQ(inv->minVal(), 10.0f);
    EXPECT_FLOAT_EQ(inv->maxVal(), 10.0f);                  // collapsed to min
    inv->setValue(99);
    EXPECT_FLOAT_EQ(inv->value(), 10.0f);                   // pinned
}

TEST(Stepper, FormatsIntegralVsFractional) {
    EXPECT_EQ(std::make_shared<StepperWidget>(5, 0, 10)->formatValue(), "5");
    EXPECT_EQ(std::make_shared<StepperWidget>(2.5f, 0, 10, 0.5f)->formatValue(), "2.5");
    auto cust = std::make_shared<StepperWidget>(3, 0, 10);
    cust->setFormatter([](float v){ return std::to_string((int)v) + "px"; });
    EXPECT_EQ(cust->formatValue(), "3px");
}

// ── element: ±-zone click routing ───────────────────────────────────────────────

TEST(Stepper, PlusZoneIncrementsAndFires) {
    int fired = 0; float last = -1;
    auto s = std::make_shared<StepperWidget>(5, 0, 10, 1,
        [&](float v){ ++fired; last = v; });
    auto el = mounted(s);
    EXPECT_TRUE(el->handleEvent(press(135, 16)));   // right zone (width-bw..width)
    EXPECT_FLOAT_EQ(s->value(), 6.0f);
    EXPECT_EQ(fired, 1);
    EXPECT_FLOAT_EQ(last, 6.0f);
}

TEST(Stepper, MinusZoneDecrementsAndFires) {
    int fired = 0; float last = -1;
    auto s = std::make_shared<StepperWidget>(5, 0, 10, 2,
        [&](float v){ ++fired; last = v; });
    auto el = mounted(s);
    EXPECT_TRUE(el->handleEvent(press(5, 16)));     // left zone [0,bw)
    EXPECT_FLOAT_EQ(s->value(), 3.0f);
    EXPECT_EQ(fired, 1);
    EXPECT_FLOAT_EQ(last, 3.0f);
}

TEST(Stepper, ClickAtBoundDoesNotFire) {
    int fired = 0;
    auto s = std::make_shared<StepperWidget>(0, 0, 10, 1,
        [&](float){ ++fired; });
    auto el = mounted(s);
    EXPECT_TRUE(el->handleEvent(press(5, 16)));     // left zone, but already at min
    EXPECT_FLOAT_EQ(s->value(), 0.0f);              // unchanged
    EXPECT_EQ(fired, 0);                            // no spurious callback
}

TEST(Stepper, MiddleAndOutsideClicksIgnored) {
    int fired = 0;
    auto s = std::make_shared<StepperWidget>(5, 0, 10, 1,
        [&](float){ ++fired; });
    auto el = mounted(s);
    EXPECT_FALSE(el->handleEvent(press(70, 16)));   // middle (value) — not a zone
    EXPECT_FALSE(el->handleEvent(press(300, 16)));  // outside the control
    EXPECT_FLOAT_EQ(s->value(), 5.0f);
    EXPECT_EQ(fired, 0);
}
