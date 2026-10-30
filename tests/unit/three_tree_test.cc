#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include <gtest/gtest.h>
#include <memory>

namespace fbx::xkit::testing {

class ThreeTreeTest : public ::testing::Test {};

// Mock Element for testing
class MockElement : public Element {
public:
    explicit MockElement(std::shared_ptr<Widget> widget) noexcept {
        widget_ = std::move(widget);
        // Create a dummy render node
        renderNode_ = std::make_shared<RenderBox>(100.0f, 100.0f);
    }

    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
    }

    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        // In a real Element, we would cast newWidget and update renderNode_ properties here.
        // The crucial test is that renderNode_ itself is NOT replaced.
        
        if (auto container = std::dynamic_pointer_cast<const ContainerWidget>(widget_)) {
            auto box = std::static_pointer_cast<RenderBox>(renderNode_);
            box->setDesiredSize(container->fixedWidth(), container->fixedHeight());
        }
    }

    void unmount() noexcept override {
        parent_.reset();
    }
};

class MockContainerWidget : public ContainerWidget {
public:
    MockContainerWidget(float width, float height) noexcept : ContainerWidget(width, height) {}
    
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override {
        // Need std::shared_ptr<Widget> instead of raw this pointer, use a trick for testing
        return std::make_shared<MockElement>(std::shared_ptr<Widget>(this, [](Widget*){}));
    }
};

TEST_F(ThreeTreeTest, ElementUpdateIsO1) {
    // 1. Initial State
    auto initial_widget = std::make_shared<MockContainerWidget>(100.0f, 100.0f);
    auto element = initial_widget->createElement();
    
    // The RenderNode is instantiated ONCE
    auto original_render_node = element->getRenderNode();
    EXPECT_NE(original_render_node, nullptr);
    
    // Check initial size by performing layout
    Constraints c{0, 1000, 0, 1000};
    Size s1 = original_render_node->layout(c);
    EXPECT_EQ(s1.width_, 100.0f);
    
    // 2. State Mutation (e.g., Signal triggered a rebuild)
    auto new_widget = std::make_shared<MockContainerWidget>(200.0f, 200.0f);
    
    // 3. Diffing / Update
    element->update(new_widget);
    
    // 4. Verification
    // The Element should have updated the existing RenderNode's properties
    Size s2 = original_render_node->layout(c);
    EXPECT_EQ(s2.width_, 200.0f);
    
    // Crucially: The RenderNode pointer must be EXACTLY the same.
    // This proves we didn't drop the heavy hardware-backed geometry tree,
    // we only updated its state in O(1) time.
    EXPECT_EQ(element->getRenderNode(), original_render_node);
}

// A label changed after mounting (a clock, a dialog's message) reaches its
// render node on the next sync — through a container's unchanged-children
// fast path too. It used to keep the text it was created with.
TEST_F(ThreeTreeTest, TextChangedAfterMountReachesRenderNode) {
    auto label = std::make_shared<TextWidget>("", 12.0f);
    auto col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{label});
    auto element = col->createElement();
    element->mount(nullptr);
    element->syncState();

    auto label_element = label->createElement();   // standalone, for the direct case
    label_element->mount(nullptr);

    label->setText("Image Viewer has reached today's limit.");
    element->syncState();
    label_element->syncState();

    bool found = false;
    for (const auto& n : element->getRenderNode()->children()) {
        if (auto t = std::dynamic_pointer_cast<RenderText>(n)) {
            EXPECT_EQ(t->text(), "Image Viewer has reached today's limit.");
            found = true;
        }
    }
    EXPECT_TRUE(found);
    auto direct = std::dynamic_pointer_cast<RenderText>(label_element->getRenderNode());
    ASSERT_NE(direct, nullptr);
    EXPECT_EQ(direct->text(), "Image Viewer has reached today's limit.");
}

// Values set from code after mounting (a switch restored from settings, a
// progress update) reach the render nodes — they used to keep their first value.
TEST_F(ThreeTreeTest, ValuesChangedAfterMountReachRenderNodes) {
    auto sw = std::make_shared<SwitchWidget>(false, nullptr);
    auto slider = std::make_shared<SliderWidget>(0.1f, 0.0f, 1.0f, nullptr);
    auto bar = std::make_shared<ProgressBarWidget>(0.0f);
    auto col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{sw, slider, bar});
    auto element = col->createElement();
    element->mount(nullptr);
    element->syncState();

    sw->setValue(true);
    slider->setValue(0.8f);
    bar->setValue(0.5f);
    element->syncState();

    const auto& nodes = element->getRenderNode()->children();
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_TRUE(std::dynamic_pointer_cast<RenderSwitch>(nodes[0])->value());
    EXPECT_FLOAT_EQ(std::dynamic_pointer_cast<RenderSlider>(nodes[1])->value(), 0.8f);
    EXPECT_FLOAT_EQ(std::dynamic_pointer_cast<RenderProgressBar>(nodes[2])->value(), 0.5f);
}

} // namespace fbx::xkit::testing
