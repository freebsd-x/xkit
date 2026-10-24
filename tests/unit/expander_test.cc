// Tests for ExpanderWidget: collapse/expand state, the layout effect (content
// is removed from layout when collapsed, not just hidden), and header-click
// toggling with the onToggle callback.

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

// Mount an expander, lay it out in a generous box, return its column height.
float layoutHeight(const std::shared_ptr<Element>& el) {
    (void)el->getRenderNode()->layout(Constraints{0, 400, 0, 1000});
    return el->getRenderNode()->getSize().height_;
}
}  // namespace

// ── pure widget state ─────────────────────────────────────────────────────────

TEST(Expander, CollapsedByDefault) {
    auto ex = std::make_shared<ExpanderWidget>(
        "Title", std::make_shared<TextWidget>("Body", 20.0f));
    EXPECT_FALSE(ex->expanded());
}

TEST(Expander, ToggleAndSetExpandedFlipState) {
    auto ex = std::make_shared<ExpanderWidget>(
        "Title", std::make_shared<TextWidget>("Body", 20.0f));
    ex->toggle();
    EXPECT_TRUE(ex->expanded());
    ex->toggle();
    EXPECT_FALSE(ex->expanded());
    ex->setExpanded(true);
    EXPECT_TRUE(ex->expanded());
}

TEST(Expander, InitiallyExpandedCtorArg) {
    auto ex = std::make_shared<ExpanderWidget>(
        std::make_shared<TextWidget>("Head", 14.0f),
        std::make_shared<TextWidget>("Body", 20.0f),
        /*expanded=*/true);
    EXPECT_TRUE(ex->expanded());
}

// ── layout effect: collapse removes content from the column ────────────────────

TEST(Expander, CollapsedIsShorterThanExpanded) {
    // Content is a 30pt text → ~30px tall when present in layout.
    auto ex = std::make_shared<ExpanderWidget>(
        "Head", std::make_shared<TextWidget>("Body", 30.0f));
    auto el = ex->createElement();
    el->mount(nullptr);

    float collapsed = layoutHeight(el);

    // Expand and re-lay-out: height must grow by roughly the content height.
    ex->setExpanded(true);
    el->update(ex);                 // rebuild the element tree for the new state
    float expanded = layoutHeight(el);

    EXPECT_GT(expanded, collapsed);
    EXPECT_GE(expanded - collapsed, 30.0f);   // content (30px) now occupies space
    EXPECT_LT(collapsed, 25.0f);              // collapsed ≈ just the header row
}

// ── header click toggles + fires onToggle ──────────────────────────────────────

TEST(Expander, HeaderClickTogglesAndReportsState) {
    int fired = 0;
    bool last = false;
    auto ex = std::make_shared<ExpanderWidget>(
        "Head", std::make_shared<TextWidget>("Body", 30.0f));
    ex->setOnToggle([&](bool open) { ++fired; last = open; });

    auto el = ex->createElement();
    el->mount(nullptr);
    (void)layoutHeight(el);

    // Click near the top-left — inside the header row.
    EXPECT_TRUE(el->handleEvent(press(5, 5)));
    EXPECT_TRUE(ex->expanded());
    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(last);

    // Click again collapses.
    (void)layoutHeight(el);
    EXPECT_TRUE(el->handleEvent(press(5, 5)));
    EXPECT_FALSE(ex->expanded());
    EXPECT_EQ(fired, 2);
    EXPECT_FALSE(last);
}

TEST(Expander, ClickBelowHeaderDoesNotToggle) {
    auto ex = std::make_shared<ExpanderWidget>(
        "Head", std::make_shared<TextWidget>("Body", 30.0f),
        /*expanded=*/true);
    auto el = ex->createElement();
    el->mount(nullptr);
    float h = layoutHeight(el);

    // A press deep in the content region (well below the header) must not toggle.
    bool consumed = el->handleEvent(press(5, static_cast<int>(h) - 2));
    (void)consumed;                 // content text doesn't consume, but either way:
    EXPECT_TRUE(ex->expanded());    // state unchanged
}
