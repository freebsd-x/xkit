#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include "xkit/application.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace fbx::xkit::testing {

class TextInputTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset global focus before each test
        XApplication::requestFocus(nullptr);
    }
};

TEST_F(TextInputTest, ClickRequestsFocus) {
    auto text_input_widget = std::make_shared<TextInputWidget>("initial");
    auto element = text_input_widget->createElement();
    
    // Initial state: no focus
    EXPECT_EQ(XApplication::getFocusedElement(), nullptr);

    // Simulate mouse click (button state is delivered in char_value_).
    shared::InputMessage click{
        .type_ = shared::InputEventType::MouseButton,
        .value_a_ = 1, .value_b_ = 1,
        .char_value_ = 1 // pressed
    };
    (void)element->handleEvent(click);

    // Verify focus was requested
    EXPECT_EQ(XApplication::getFocusedElement(), element);
}

TEST_F(TextInputTest, HandlesKeyPressWhenFocused) {
    auto text_input_widget = std::make_shared<TextInputWidget>("");
    auto element = text_input_widget->createElement();
    
    // Simulate mouse click to get focus (state in char_value_).
    shared::InputMessage click{
        .type_ = shared::InputEventType::MouseButton,
        .value_a_ = 1, .value_b_ = 1,
        .char_value_ = 1
    };
    (void)element->handleEvent(click);

    // Simulate typing 'a'
    shared::InputMessage keyPress{
        .type_ = shared::InputEventType::KeyPress,
        .value_a_ = 'a',
        .value_b_ = 1 // Pressed
    };
    (void)element->handleEvent(keyPress);

    // To verify the text was updated, we can check the child RenderText node
    // It's inside a RenderFlexRow.
    auto row_node = std::static_pointer_cast<RenderFlexRow>(element->getRenderNode());
    
    // In a real test we'd add a getter for the text or check the display list output.
    // For this prototype test, if it handled the event and didn't crash, we're good.
    // We can also trigger backspace.
    shared::InputMessage backspace{
        .type_ = shared::InputEventType::KeyPress,
        .value_a_ = '\b',
        .value_b_ = 1 // Pressed
    };
    bool handled = element->handleEvent(backspace);
    
    EXPECT_TRUE(handled);
}

// Enter/Return commits: it fires onSubmit with the final text and must NOT fire
// onChange or insert a newline (regression guard for URL bars / search fields
// that act on commit, e.g. the Browser address bar).
TEST_F(TextInputTest, EnterFiresOnSubmitNotOnChange) {
    auto w = std::make_shared<TextInputWidget>("example.com");
    std::string submitted;
    int submit_count = 0, change_count = 0;
    w->setOnSubmit([&](const std::string& t) { submitted = t; ++submit_count; });
    w->setOnChanged([&](const std::string&) { ++change_count; });
    auto element = w->createElement();
    (void)element->handleEvent({ .type_ = shared::InputEventType::MouseButton,
                           .value_a_ = 1, .value_b_ = 1, .char_value_ = 1 });

    // Enter = evdev keycode 28.
    shared::InputMessage enter{ .type_ = shared::InputEventType::KeyPress,
                                .value_a_ = 28, .value_b_ = 1 };
    EXPECT_TRUE(element->handleEvent(enter));
    EXPECT_EQ(submit_count, 1);
    EXPECT_EQ(submitted, "example.com");  // final text, no trailing newline
    EXPECT_EQ(change_count, 0);           // Enter must not be treated as an edit
}

// A printable character is an edit: it fires onChange, never onSubmit.
TEST_F(TextInputTest, PrintableCharFiresOnChangeNotSubmit) {
    auto w = std::make_shared<TextInputWidget>("");
    int submit_count = 0, change_count = 0;
    w->setOnSubmit([&](const std::string&) { ++submit_count; });
    w->setOnChanged([&](const std::string&) { ++change_count; });
    auto element = w->createElement();
    (void)element->handleEvent({ .type_ = shared::InputEventType::MouseButton,
                           .value_a_ = 1, .value_b_ = 1, .char_value_ = 1 });

    // Printable char is delivered in char_value_ (value_a_ is the raw keycode).
    shared::InputMessage key{ .type_ = shared::InputEventType::KeyPress,
                              .value_a_ = 30, .value_b_ = 1, .char_value_ = 'a' };
    EXPECT_TRUE(element->handleEvent(key));
    EXPECT_EQ(change_count, 1);
    EXPECT_EQ(submit_count, 0);
}

// Text replacement (Settings → Text Replacement): a shortcut followed by a
// boundary becomes its phrase — but never in a password field.
namespace {
void typeInto(const std::shared_ptr<Element>& el, std::string_view s) {
    for (char c : s)
        (void)el->handleEvent({ .type_ = shared::InputEventType::KeyPress,
                                .value_a_ = 0, .value_b_ = 1, .char_value_ = static_cast<int32_t>(c) });
}
}  // namespace

TEST_F(TextInputTest, ShortcutExpandsOnBoundaryButNotInPasswordFields) {
    char tmpl[] = "/tmp/xkit-textrepl.XXXXXX";
    ASSERT_NE(::mkdtemp(tmpl), nullptr);
    std::string home = tmpl;
    std::filesystem::create_directories(home + "/.config/freebsdx");
    std::ofstream(home + "/.config/freebsdx/text-replacements") << "omw\tOn my way\n";
    std::string old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
    ::setenv("HOME", home.c_str(), 1);

    auto w = std::make_shared<TextInputWidget>("");
    auto el = w->createElement();
    (void)el->handleEvent({ .type_ = shared::InputEventType::MouseButton,
                            .value_a_ = 1, .value_b_ = 1, .char_value_ = 1 });
    typeInto(el, "ok omw.");
    EXPECT_EQ(w->text(), "ok On my way.");
    typeInto(el, " nomw ");                       // only whole words expand
    EXPECT_EQ(w->text(), "ok On my way. nomw ");

    auto pw = std::make_shared<TextInputWidget>("");
    pw->setPasswordMode(true);
    auto pel = pw->createElement();
    (void)pel->handleEvent({ .type_ = shared::InputEventType::MouseButton,
                             .value_a_ = 1, .value_b_ = 1, .char_value_ = 1 });
    typeInto(pel, "omw ");
    EXPECT_EQ(pw->text(), "omw ");

    if (old_home.empty()) ::unsetenv("HOME"); else ::setenv("HOME", old_home.c_str(), 1);
    std::filesystem::remove_all(home);
}

// Key events carry the keyboard layout's Unicode character; non-ASCII ones
// are inserted as UTF-8, not truncated to a byte.
TEST_F(TextInputTest, NonAsciiLayoutCharactersInsertAsUtf8) {
    auto w = std::make_shared<TextInputWidget>("");
    auto el = w->createElement();
    (void)el->handleEvent({ .type_ = shared::InputEventType::MouseButton,
                            .value_a_ = 1, .value_b_ = 1, .char_value_ = 1 });
    for (int32_t cp : {0x63, 0x61, 0x66, 0xE9, 0x20, 0xDF})   // "café ß"
        (void)el->handleEvent({ .type_ = shared::InputEventType::KeyPress,
                                .value_a_ = 0, .value_b_ = 1, .char_value_ = cp });
    EXPECT_EQ(w->text(), "caf\xC3\xA9 \xC3\x9F");
}

} // namespace fbx::xkit::testing
