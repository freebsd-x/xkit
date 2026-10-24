// Tests for pointer-button plumbing: the button code packed into a MouseButton
// event and ButtonWidget routing left→primary, right→secondary.

#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include "xkit/application.h"
#include "shared/protocol.h"
#include <gtest/gtest.h>
#include <memory>

using namespace fbx::xkit;
namespace shared = fbx::shared;

namespace {
// A MouseButton press for `button` (272/273/274) at (x,y), mimicking how
// XApplication::Run encodes it: state in char_value_, code in modifiers_ high bits.
shared::InputMessage press(int button, int x = 5, int y = 5) {
    shared::InputMessage m{};
    m.type_       = shared::InputEventType::MouseButton;
    m.char_value_ = 1;                                     // press
    m.value_a_    = x;
    m.value_b_    = y;
    m.modifiers_  = static_cast<uint32_t>(button) << 16;   // packed button code
    return m;
}
}  // namespace

TEST(PointerButton, DecodeFromEvent) {
    using PB = XApplication::PointerButton;
    EXPECT_EQ(XApplication::pointerButton(press(272)), PB::Left);
    EXPECT_EQ(XApplication::pointerButton(press(273)), PB::Right);
    EXPECT_EQ(XApplication::pointerButton(press(274)), PB::Middle);
    shared::InputMessage none{};
    EXPECT_EQ(XApplication::pointerButton(none), PB::Unknown);
}

TEST(PointerButton, KeyboardModsSurviveInLowBits) {
    // Ctrl (bit 1<<1 = 2) held during a right-click: mods readable, button too.
    auto e = press(273);
    e.modifiers_ |= 2u;                                    // Ctrl
    EXPECT_EQ(XApplication::pointerButton(e), XApplication::PointerButton::Right);
    EXPECT_TRUE(e.modifiers_ & 2u);                        // Ctrl still detectable
}

TEST(PointerButton, ButtonRoutesLeftToPrimaryRightToSecondary) {
    bool primary = false, secondary = false;
    auto btn = std::make_shared<ButtonWidget>(std::string("Hi"),
                                              [&] { primary = true; });
    btn->setOnSecondaryPressed([&] { secondary = true; });
    auto el = btn->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 200, 0, 200});

    EXPECT_TRUE(el->handleEvent(press(272)));      // left → primary
    EXPECT_TRUE(primary);
    EXPECT_FALSE(secondary);

    primary = false;
    EXPECT_TRUE(el->handleEvent(press(273)));      // right → secondary
    EXPECT_TRUE(secondary);
    EXPECT_FALSE(primary);
}

TEST(PointerButton, RightClickWithoutSecondaryHandlerDoesNotFirePrimary) {
    bool primary = false;
    auto btn = std::make_shared<ButtonWidget>(std::string("Hi"), [&] { primary = true; });
    auto el = btn->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 200, 0, 200});

    el->handleEvent(press(273));                   // right, no secondary handler
    EXPECT_FALSE(primary);                          // must NOT invoke the primary action
}

TEST(PointerButton, UnknownButtonTreatedAsPrimary) {
    bool primary = false;
    auto btn = std::make_shared<ButtonWidget>(std::string("Hi"), [&] { primary = true; });
    auto el = btn->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 200, 0, 200});

    el->handleEvent(press(0));                      // un-encoded (synthetic) press
    EXPECT_TRUE(primary);
}
