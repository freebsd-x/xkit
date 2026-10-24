#pragma once

#include "xkit/render_node.h"
#include <memory>
#include <vector>
#include <string>

namespace fbx::xkit {

class Widget;

struct SemanticNode {
    uint32_t    id;
    std::string role;    // "Button", "Label", "TextInput", "Checkbox", "Switch",
                         //   "Slider", "Radio", "Image", "ListItem", "Container"
    std::string label;   // human-readable name (widget label or content text)
    std::string value;   // current value string (checked/unchecked, "75%", typed text…)
    bool        focused{false};
    float x{0}, y{0}, width{0}, height{0};
    std::vector<SemanticNode> children;
};

// Base class for the Element Tree.
// Manages state, lifecycle, and maintains the RenderNode.
class Element : public std::enable_shared_from_this<Element> {
public:
    virtual ~Element() noexcept = default;

    virtual void mount(std::shared_ptr<Element> parent) noexcept = 0;
    virtual void update(std::shared_ptr<Widget> newWidget) noexcept = 0;
    virtual void unmount() noexcept = 0;

    virtual void onThemeChanged(const Theme& theme) noexcept { (void)theme; }
    virtual void syncState() noexcept;

    [[nodiscard]] std::shared_ptr<RenderNode> getRenderNode() const noexcept { return renderNode_; }

    // The widget this element was built from. Reconcilers use it to reuse an
    // existing element when the same widget instance reappears at a position,
    // instead of tearing the element down and rebuilding it.
    [[nodiscard]] std::shared_ptr<Widget> sourceWidget() const noexcept { return widget_; }

    virtual SemanticNode getSemanticTree() const noexcept {
        auto abs = renderNode_->getAbsoluteOffset();
        auto sz  = renderNode_->getSize();
        return SemanticNode{
            .id      = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF),
            .role    = renderNode_->accessibilityRole().empty()
                           ? "Container"
                           : renderNode_->accessibilityRole(),
            .label   = renderNode_->accessibilityLabel(),
            .value   = "",
            .focused = false,
            .x = abs.x_, .y = abs.y_,
            .width = sz.width_, .height = sz.height_,
            .children = {}
        };
    }

    // Flatten this element's semantic tree into a pre-order list (BFS caller uses this).
    void collectSemanticNodes(std::vector<SemanticNode>& out,
                              uint32_t parent_id = 0) const noexcept {
        (void)parent_id;
        out.push_back(getSemanticTree());
    }

    // Event Bubbling implementation
    [[nodiscard]] virtual bool handleEvent(const shared::InputMessage& event) noexcept {
        // 1. Perform hit-testing for mouse events
        if (event.type_ == shared::InputEventType::MouseMove || event.type_ == shared::InputEventType::MouseButton) {
            auto hit_node = renderNode_->hitTest({static_cast<float>(event.value_a_), static_cast<float>(event.value_b_)});
            if (hit_node) {
                if (auto owner = hit_node->getOwnerElement()) {
                    // Route to the actual element that was hit
                    // owner->onEvent(event); 
                }
            }
        }

        // Do NOT bubble unconsumed events up to the parent. Dispatch already
        // routes keyboard to the focused element and otherwise fans out from the
        // root, so upward bubbling is redundant — and actively fatal: a focused
        // child that ignores an event (e.g. a TextInput on a key *release*)
        // would bubble to its parent, whose fan-out handleEvent forwards right
        // back down to that same child, recursing until the stack overflows.
        // Simply report the event as unconsumed.
        return false; // Not consumed by this element
    }

protected:
    std::shared_ptr<Widget> widget_;
    std::shared_ptr<RenderNode> renderNode_;
    std::weak_ptr<Element> parent_;
};

} // namespace fbx::xkit
