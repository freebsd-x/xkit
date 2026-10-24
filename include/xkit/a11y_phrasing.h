#pragma once

#include <string>
#include <string_view>

// Screen-reader phrasing: turn a semantic node into the utterance a screen
// reader speaks. Convention follows VoiceOver/Orca — label first, then the role
// word, then state and set position: "Save, button" / "Email, text field, blank"
// / "Wi-Fi, switch, on" / "Reply, button, dimmed, 3 of 7".
//
// Pure and dependency-free so it is unit-testable and shared by both the
// in-process auto-announce (XApplication::requestFocus) and any out-of-process
// screen-reader consumer.
namespace fbx::xkit::a11y {

struct NodeDesc {
    std::string role;    // "Button","TextInput","Checkbox","Switch","Slider",
                         //   "Radio","Label","Image","ListItem","Container",…
    std::string label;   // accessible name
    std::string value;   // current value (slider/text field content, etc.)
    bool disabled = false;
    int  checked  = -1;  // -1 = n/a, 0 = off/unchecked, 1 = on/checked
    int  pos      = 0;   // 1-based position in a set; 0 = none
    int  setsize  = 0;   // set size; 0 = none
};

// The spoken role word for a role token ("Button"→"button",
// "TextInput"→"text field"). Static/structural roles (Label, Text, Container,
// empty) return "" — their label is spoken bare. Unknown roles are lowercased.
std::string roleWord(std::string_view role) noexcept;

// Build the full utterance for a node.
std::string describe(const NodeDesc& node) noexcept;

}  // namespace fbx::xkit::a11y
