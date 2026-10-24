#include "xkit/a11y_phrasing.h"

#include <algorithm>
#include <cctype>

namespace fbx::xkit::a11y {

namespace {
std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
bool eq(std::string_view a, const char* b) { return a == b; }

void append(std::string& out, std::string_view piece) {
    if (piece.empty()) return;
    if (!out.empty()) out += ", ";
    out += piece;
}
}  // namespace

std::string roleWord(std::string_view role) noexcept {
    if (eq(role, "Button"))    return "button";
    if (eq(role, "TextInput")) return "text field";
    if (eq(role, "Checkbox"))  return "checkbox";
    if (eq(role, "Switch"))    return "switch";
    if (eq(role, "Slider"))    return "slider";
    if (eq(role, "Radio"))     return "radio button";
    if (eq(role, "Image"))     return "image";
    if (eq(role, "ListItem"))  return "list item";
    // Static / structural roles are spoken as their label alone.
    if (eq(role, "Label") || eq(role, "Text") || eq(role, "Container") ||
        role.empty())
        return "";
    return lower(role);
}

std::string describe(const NodeDesc& n) noexcept {
    std::string out;
    std::string rw = roleWord(n.role);

    // Label, then role word. A structural role with no role word speaks the
    // label alone; a control with no label speaks its role word alone.
    if (!n.label.empty()) out = n.label;
    append(out, rw);

    // State / value.
    if (eq(n.role, "Checkbox")) {
        if (n.checked == 1) append(out, "checked");
        else if (n.checked == 0) append(out, "unchecked");
    } else if (eq(n.role, "Switch")) {
        if (n.checked == 1) append(out, "on");
        else if (n.checked == 0) append(out, "off");
    } else if (eq(n.role, "Radio")) {
        if (n.checked == 1) append(out, "selected");
        else if (n.checked == 0) append(out, "not selected");
    } else if (eq(n.role, "TextInput")) {
        append(out, n.value.empty() ? "blank" : n.value);
    } else if (!n.value.empty()) {
        append(out, n.value);   // slider position, etc.
    }

    if (n.disabled) append(out, "dimmed");

    if (n.pos > 0 && n.setsize > 0)
        append(out, std::to_string(n.pos) + " of " + std::to_string(n.setsize));

    return out;
}

}  // namespace fbx::xkit::a11y
