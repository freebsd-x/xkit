// Unit tests for screen-reader phrasing (pure logic).

#include "xkit/a11y_phrasing.h"
#include <gtest/gtest.h>

using fbx::xkit::a11y::NodeDesc;
using fbx::xkit::a11y::describe;
using fbx::xkit::a11y::roleWord;

// ── roleWord ──────────────────────────────────────────────────────────────────

TEST(A11yPhrasing, RoleWordMappings) {
    EXPECT_EQ(roleWord("Button"), "button");
    EXPECT_EQ(roleWord("TextInput"), "text field");
    EXPECT_EQ(roleWord("Radio"), "radio button");
    EXPECT_EQ(roleWord("Switch"), "switch");
}

TEST(A11yPhrasing, StructuralRolesHaveNoRoleWord) {
    EXPECT_EQ(roleWord("Label"), "");
    EXPECT_EQ(roleWord("Container"), "");
    EXPECT_EQ(roleWord(""), "");
}

TEST(A11yPhrasing, UnknownRoleLowercased) {
    EXPECT_EQ(roleWord("Spinner"), "spinner");
}

// ── describe: basic label + role ──────────────────────────────────────────────

TEST(A11yPhrasing, ButtonLabelThenRole) {
    EXPECT_EQ(describe({.role = "Button", .label = "Save"}), "Save, button");
}

TEST(A11yPhrasing, StaticLabelSpokenAlone) {
    EXPECT_EQ(describe({.role = "Label", .label = "Welcome"}), "Welcome");
}

TEST(A11yPhrasing, ControlWithNoLabelSpeaksRoleAlone) {
    EXPECT_EQ(describe({.role = "Button"}), "button");
}

// ── describe: state ───────────────────────────────────────────────────────────

TEST(A11yPhrasing, CheckboxChecked) {
    EXPECT_EQ(describe({.role = "Checkbox", .label = "Accept", .checked = 1}),
              "Accept, checkbox, checked");
}

TEST(A11yPhrasing, CheckboxUncheckedNaIsSilent) {
    // checked == -1 (n/a) omits state entirely.
    EXPECT_EQ(describe({.role = "Checkbox", .label = "Accept"}),
              "Accept, checkbox");
    EXPECT_EQ(describe({.role = "Checkbox", .label = "Accept", .checked = 0}),
              "Accept, checkbox, unchecked");
}

TEST(A11yPhrasing, SwitchOnOff) {
    EXPECT_EQ(describe({.role = "Switch", .label = "Wi-Fi", .checked = 1}),
              "Wi-Fi, switch, on");
    EXPECT_EQ(describe({.role = "Switch", .label = "Wi-Fi", .checked = 0}),
              "Wi-Fi, switch, off");
}

TEST(A11yPhrasing, RadioSelected) {
    EXPECT_EQ(describe({.role = "Radio", .label = "Medium", .checked = 1}),
              "Medium, radio button, selected");
    EXPECT_EQ(describe({.role = "Radio", .label = "Medium", .checked = 0}),
              "Medium, radio button, not selected");
}

// ── describe: text field value ────────────────────────────────────────────────

TEST(A11yPhrasing, TextFieldBlank) {
    EXPECT_EQ(describe({.role = "TextInput", .label = "Email"}),
              "Email, text field, blank");
}

TEST(A11yPhrasing, TextFieldWithValue) {
    EXPECT_EQ(describe({.role = "TextInput", .label = "Email", .value = "a@b.co"}),
              "Email, text field, a@b.co");
}

TEST(A11yPhrasing, SliderValue) {
    EXPECT_EQ(describe({.role = "Slider", .label = "Volume", .value = "60%"}),
              "Volume, slider, 60%");
}

// ── describe: disabled + set position ─────────────────────────────────────────

TEST(A11yPhrasing, Disabled) {
    EXPECT_EQ(describe({.role = "Button", .label = "Reply", .disabled = true}),
              "Reply, button, dimmed");
}

TEST(A11yPhrasing, SetPosition) {
    EXPECT_EQ(describe({.role = "ListItem", .label = "Inbox", .pos = 3, .setsize = 7}),
              "Inbox, list item, 3 of 7");
}

TEST(A11yPhrasing, PartialSetPositionOmitted) {
    // pos without setsize (or vice versa) is not spoken.
    EXPECT_EQ(describe({.role = "ListItem", .label = "Inbox", .pos = 3}),
              "Inbox, list item");
}

TEST(A11yPhrasing, EverythingTogether) {
    EXPECT_EQ(describe({.role = "Switch", .label = "Bluetooth", .checked = 0,
                        .disabled = true, .pos = 2, .setsize = 4}),
              "Bluetooth, switch, off, dimmed, 2 of 4");
}
