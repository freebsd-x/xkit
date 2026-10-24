#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include <gtest/gtest.h>
#include <memory>

namespace fbx::xkit::testing {

class EventTest : public ::testing::Test {};

TEST_F(EventTest, ButtonConsumesClickAndTriggersCallback) {
    int fire_count = 0;

    // 1. Create a Button with a Text child
    auto text = std::make_shared<TextWidget>("Click Me", 12.0f);
    auto button_widget = std::make_shared<ButtonWidget>(text, [&fire_count]() {
        ++fire_count;
    });

    // 2. Build the tree
    auto button_element = button_widget->createElement();
    button_element->mount(nullptr);
    
    auto root_render_node = button_element->getRenderNode();
    Constraints c{0, 500, 0, 500};
    (void)root_render_node->layout(c);

    // 3. Find the text render node (leaf)
    // HitTest at (5, 5) should hit the text child
    auto hit_node = root_render_node->hitTest({5.0f, 5.0f});
    ASSERT_NE(hit_node, nullptr);
    
    auto hit_element = hit_node->getOwnerElement();
    // (Note: In this test mock, the hit node might not have owner element yet, let's fix that)
    
    // 4. Simulate a press. For mouse buttons the state is delivered in
    //    char_value_ (1=down, 0=up); value_a_/value_b_ carry the cursor position.
    shared::InputMessage press{
        .type_ = shared::InputEventType::MouseButton,
        .value_a_ = 5, .value_b_ = 5,   // cursor position
        .char_value_ = 1                // down
    };
    bool consumed = button_element->handleEvent(press);
    EXPECT_TRUE(consumed);
    EXPECT_EQ(fire_count, 1);

    // The matching release must NOT fire the callback again (fire once, on press).
    shared::InputMessage release = press;
    release.char_value_ = 0;            // up
    (void)button_element->handleEvent(release);
    EXPECT_EQ(fire_count, 1);
}

} // namespace fbx::xkit::testing
