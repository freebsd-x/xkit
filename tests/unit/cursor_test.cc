// Client-side cursor-resolution tests — the resolveCursorAt path that decides
// which cursor shape a widget requests on hover.
//
// These exercise the real widget/element/render-node code (not a mock): the
// cursor each control declares, the walk-up inheritance in resolveCursorAt, and
// the disabled → not-allowed override. This is the client half of the cursor
// system that pixel tests and the headless harness can't reach directly.

#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include "xkit/cursor.h"
#include <gtest/gtest.h>
#include <memory>

namespace fbx::xkit::testing {

class CursorTest : public ::testing::Test {};

// A button declares the Pointer (hand) cursor.
TEST_F(CursorTest, ButtonRequestsPointer) {
    auto btn = std::make_shared<ButtonWidget>("ok");
    auto el  = btn->createElement();
    el->mount(nullptr);
    el->syncState();
    EXPECT_EQ(el->getRenderNode()->cursor(), CursorShape::Pointer);
}

// A text input declares the Text (I-beam) cursor.
TEST_F(CursorTest, TextInputRequestsText) {
    auto ti = std::make_shared<TextInputWidget>("");
    auto el = ti->createElement();
    el->mount(nullptr);
    el->syncState();
    EXPECT_EQ(el->getRenderNode()->cursor(), CursorShape::Text);
}

// A disabled button overrides its cursor to not-allowed each frame (syncState).
TEST_F(CursorTest, DisabledButtonRequestsNotAllowed) {
    auto btn = std::make_shared<ButtonWidget>("ok");
    btn->setDisabled(true);
    auto el = btn->createElement();
    el->mount(nullptr);
    el->syncState();
    EXPECT_EQ(el->getRenderNode()->cursor(), CursorShape::NotAllowed);

    // Re-enabling flips it back to the hand.
    btn->setDisabled(false);
    el->syncState();
    EXPECT_EQ(el->getRenderNode()->cursor(), CursorShape::Pointer);
}

// resolveCursorAt hit-tests to the deepest node, then walks up to the nearest
// ancestor that set a shape — mirroring how a hovered child inherits its
// container's cursor (e.g. a text input's beam glyph over its inner text node).
TEST_F(CursorTest, ResolveWalksUpToAncestor) {
    auto root  = std::make_shared<RenderBox>(200.0f, 200.0f);   // no cursor
    auto panel = std::make_shared<RenderBox>(50.0f, 50.0f);     // declares Text
    auto inner = std::make_shared<RenderBox>(20.0f, 20.0f);     // no cursor
    panel->setCursor(CursorShape::Text);
    panel->setOffset({0.0f, 0.0f});
    inner->setOffset({0.0f, 0.0f});
    panel->addChild(inner);
    root->addChild(panel);

    Constraints c{0, 200, 0, 200};
    (void)root->layout(c);

    // Over the inner (cursor-less) child → inherits the panel's Text.
    EXPECT_EQ(root->resolveCursorAt({5.0f, 5.0f}),   CursorShape::Text);
    // Over the panel's own area → its Text.
    EXPECT_EQ(root->resolveCursorAt({40.0f, 40.0f}), CursorShape::Text);
    // Over the bare root background → Default (nothing set one).
    EXPECT_EQ(root->resolveCursorAt({150.0f, 150.0f}), CursorShape::Default);
}

// A real button, resolved through resolveCursorAt on a laid-out tree, yields the
// Pointer cursor when hovered (end-to-end: widget cursor → node → resolution).
TEST_F(CursorTest, ResolveOverRealButtonIsPointer) {
    auto btn = std::make_shared<ButtonWidget>("ok");
    auto el  = btn->createElement();
    el->mount(nullptr);
    el->syncState();
    auto node = el->getRenderNode();
    Constraints c{0, 100, 0, 40};
    (void)node->layout(c);
    // Any point inside the button (incl. over its label child) resolves Pointer.
    EXPECT_EQ(node->resolveCursorAt({10.0f, 10.0f}), CursorShape::Pointer);
}

} // namespace fbx::xkit::testing
