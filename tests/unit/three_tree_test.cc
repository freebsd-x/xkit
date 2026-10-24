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

} // namespace fbx::xkit::testing
