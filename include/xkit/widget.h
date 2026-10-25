#pragma once

#include "xkit/element.h"
#include "xkit/theme.h"
#include "xkit/table.h"
#include "xkit/calendar.h"
#include <memory>
#include <functional>
#include <vector>
#include <string>
#include <algorithm>

namespace fbx::xkit {

// Base class for the Widget Tree.
class Widget : public std::enable_shared_from_this<Widget> {
public:
    virtual ~Widget() noexcept = default;

    [[nodiscard]] virtual std::shared_ptr<Element> createElement() noexcept = 0;

    // ── Common styling properties ──────────────────────────────────────────
    void setPadding(float p) noexcept { padding_ = p; }
    [[nodiscard]] float padding() const noexcept { return padding_; }

    void setBackgroundColor(Color c) noexcept { backgroundColor_ = c; hasBackground_ = true; }
    [[nodiscard]] Color backgroundColor() const noexcept { return backgroundColor_; }
    [[nodiscard]] bool hasBackground() const noexcept { return hasBackground_; }

    // Corner rounding for the widget's background rect (0 = square). Lets any
    // container paint a rounded card/pill without a dedicated wrapper.
    void setBorderRadius(float r) noexcept { borderRadius_ = r; }
    [[nodiscard]] float borderRadius() const noexcept { return borderRadius_; }

    void setFixedSize(float w, float h) noexcept { fixedWidth_ = w; fixedHeight_ = h; hasFixedSize_ = true; }
    [[nodiscard]] float fixedWidth()  const noexcept { return fixedWidth_; }
    [[nodiscard]] float fixedHeight() const noexcept { return fixedHeight_; }
    [[nodiscard]] bool hasFixedSize() const noexcept { return hasFixedSize_; }

    void setAlignment(Alignment a) noexcept { alignment_ = a; }
    [[nodiscard]] Alignment alignment() const noexcept { return alignment_; }
    // Placement within a parent StackWidget (default Inherit = follow the stack).
    void setStackAnchor(StackAnchor a) noexcept { stack_anchor_ = a; }
    [[nodiscard]] StackAnchor stackAnchor() const noexcept { return stack_anchor_; }

    void setVisibility(bool v) noexcept { visible_ = v; }
    [[nodiscard]] bool isVisible() const noexcept { return visible_; }

    // Disabled controls ignore interaction and show a not-allowed cursor on
    // hover. Meaningful on interactive widgets (Button, Checkbox, Switch,
    // Slider); inert elsewhere.
    void setDisabled(bool d) noexcept { disabled_ = d; }
    void setEnabled(bool e)  noexcept { disabled_ = !e; }
    [[nodiscard]] bool isDisabled() const noexcept { return disabled_; }

    // Cursor shown while the pointer hovers this widget. Default (Inherit) lets
    // the widget's element pick, or inherits from an ancestor. Set it to give
    // any surface a shape declaratively — e.g. Text on an editable region.
    void setCursor(CursorShape c) noexcept { cursor_ = c; }
    [[nodiscard]] CursorShape cursor() const noexcept { return cursor_; }

    // ── Flex container properties (meaningful on FlexColumn / FlexRow) ─────
    void setGap(float g) noexcept { gap_ = g; }
    [[nodiscard]] float gap() const noexcept { return gap_; }

    void setMainAxisAlignment(MainAxisAlignment a) noexcept { main_axis_alignment_ = a; }
    [[nodiscard]] MainAxisAlignment mainAxisAlignment() const noexcept { return main_axis_alignment_; }

    void setCrossAxisAlignment(CrossAxisAlignment a) noexcept { cross_axis_alignment_ = a; }
    [[nodiscard]] CrossAxisAlignment crossAxisAlignment() const noexcept { return cross_axis_alignment_; }

    // ── Flex child properties (meaningful when inside a FlexColumn/FlexRow) ─
    void setFlexGrow(float g) noexcept { flex_grow_ = g; }
    [[nodiscard]] float flexGrow() const noexcept { return flex_grow_; }

    void setFlexShrink(float s) noexcept { flex_shrink_ = s; }
    [[nodiscard]] float flexShrink() const noexcept { return flex_shrink_; }

    void setFlexBasis(float b) noexcept { flex_basis_ = b; }
    [[nodiscard]] float flexBasis() const noexcept { return flex_basis_; }

    // ── Accessibility ──────────────────────────────────────────────────────
    // Overrides the default role/label derived from the widget type.
    void setAccessibilityLabel(std::string label) noexcept { accessibility_label_ = std::move(label); }
    void setAccessibilityRole(std::string role)   noexcept { accessibility_role_  = std::move(role);  }
    [[nodiscard]] const std::string& accessibilityLabel() const noexcept { return accessibility_label_; }
    [[nodiscard]] const std::string& accessibilityRole()  const noexcept { return accessibility_role_;  }

    // ── Margin ─────────────────────────────────────────────────────────────
    void setMargin(float top, float right, float bottom, float left) noexcept {
        margin_top_ = top; margin_right_ = right;
        margin_bottom_ = bottom; margin_left_ = left;
    }
    void setMarginAll(float m) noexcept { setMargin(m, m, m, m); }
    void setMarginSymmetric(float vertical, float horizontal) noexcept {
        setMargin(vertical, horizontal, vertical, horizontal);
    }
    [[nodiscard]] float marginTop()    const noexcept { return margin_top_; }
    [[nodiscard]] float marginRight()  const noexcept { return margin_right_; }
    [[nodiscard]] float marginBottom() const noexcept { return margin_bottom_; }
    [[nodiscard]] float marginLeft()   const noexcept { return margin_left_; }

protected:
    float padding_{0};
    Color backgroundColor_{0,0,0,0};
    bool hasBackground_{false};
    float borderRadius_{0};
    float fixedWidth_{0}, fixedHeight_{0};
    bool hasFixedSize_{false};
    Alignment alignment_{Alignment::Start};
    StackAnchor stack_anchor_{StackAnchor::Inherit};
    bool visible_{true};
    bool disabled_{false};
    CursorShape cursor_{CursorShape::Inherit};

    // Flex container
    float gap_{0.0f};
    MainAxisAlignment  main_axis_alignment_{MainAxisAlignment::Start};
    CrossAxisAlignment cross_axis_alignment_{CrossAxisAlignment::Start};

    // Flex child
    float flex_grow_{0.0f};
    float flex_shrink_{1.0f};
    float flex_basis_{-1.0f};

    // Margin
    float margin_top_{0.0f};
    float margin_right_{0.0f};
    float margin_bottom_{0.0f};
    float margin_left_{0.0f};

    // Accessibility
    std::string accessibility_label_;
    std::string accessibility_role_;
};

class Spacer : public Widget {
public:
    Spacer(float w, float h, bool is_flexible = false) noexcept 
        : width_(w), height_(h), flexible_(is_flexible) {}
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    float width() const { return width_; }
    float height() const { return height_; }
    bool flexible() const { return flexible_; }
private:
    float width_, height_;
    bool flexible_;
};

class ContainerWidget : public Widget {
public:
    ContainerWidget(std::shared_ptr<Widget> child = nullptr) noexcept : child_(std::move(child)) {}
    ContainerWidget(float width, float height, std::shared_ptr<Widget> child = nullptr) noexcept 
        : child_(std::move(child)) { setFixedSize(width, height); }

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] std::shared_ptr<Widget> child() const noexcept { return child_; }
private:
    std::shared_ptr<Widget> child_;
};

class FlexColumn : public Widget {
public:
    explicit FlexColumn(std::vector<std::shared_ptr<Widget>> children = {}) noexcept
        : children_(std::move(children)) {}
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] const std::vector<std::shared_ptr<Widget>>& children() const noexcept { return children_; }
    void addChild(std::shared_ptr<Widget> child) { children_.push_back(std::move(child)); }
    void clearChildren() {
        // Swap out first so children_ is empty before any child destructor runs.
        // This prevents re-entrancy if a child callback calls addChild/clearChildren.
        std::vector<std::shared_ptr<Widget>> tmp;
        tmp.swap(children_);
    }
    [[nodiscard]] size_t childCount() const noexcept { return children_.size(); }
    [[nodiscard]] std::shared_ptr<Widget> childAt(size_t i) const noexcept {
        return i < children_.size() ? children_[i] : nullptr;
    }
private:
    std::vector<std::shared_ptr<Widget>> children_;
};

class FlexRow : public Widget {
public:
    explicit FlexRow(std::vector<std::shared_ptr<Widget>> children = {}) noexcept
        : children_(std::move(children)) {}
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] const std::vector<std::shared_ptr<Widget>>& children() const noexcept { return children_; }
    void addChild(std::shared_ptr<Widget> child) { children_.push_back(std::move(child)); }
    void clearChildren() {
        std::vector<std::shared_ptr<Widget>> tmp;
        tmp.swap(children_);
    }
    [[nodiscard]] size_t childCount() const noexcept { return children_.size(); }
    [[nodiscard]] std::shared_ptr<Widget> childAt(size_t i) const noexcept {
        return i < children_.size() ? children_[i] : nullptr;
    }
private:
    std::vector<std::shared_ptr<Widget>> children_;
};

// Horizontal tab strip: a row of selectable headers, each with an optional close
// button. Presentational only — the app owns the tab state (see TabModel for the
// active-index mutation logic) and passes the titles + active index; taps invoke
// the callbacks with the tab index. Composes FlexRow/Button/Text, so it inherits
// their layout, rendering, hit-testing, and theming.
class TabBarWidget : public Widget {
public:
    using OnSelect = std::function<void(int)>;
    using OnClose  = std::function<void(int)>;

    TabBarWidget(std::vector<std::string> titles, int active,
                 OnSelect on_select = nullptr, OnClose on_close = nullptr) noexcept
        : titles_(std::move(titles)), active_(active),
          on_select_(std::move(on_select)), on_close_(std::move(on_close)) {}

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;

    void setClosable(bool c) noexcept { closable_ = c; }

    [[nodiscard]] const std::vector<std::string>& titles() const noexcept { return titles_; }
    [[nodiscard]] int  active()   const noexcept { return active_; }
    [[nodiscard]] bool closable() const noexcept { return closable_; }

private:
    std::vector<std::string> titles_;
    int      active_;
    OnSelect on_select_;
    OnClose  on_close_;
    bool     closable_{true};
};

// A select/dropdown control: a trigger showing the current selection that opens
// a popup list of options (via the overlay layer). Presentational + self-
// contained — the app passes the options + selected index and gets onChange(i)
// when the user picks one; the popup opens anchored to the trigger and dismisses
// on selection or outside click. Requires the OverlayHost (present on every app
// root via XApplication::Run).
class DropdownWidget : public Widget {
public:
    using OnChange = std::function<void(int)>;

    DropdownWidget(std::vector<std::string> options, int selected,
                   OnChange on_change = nullptr,
                   std::string placeholder = "Select…") noexcept
        : options_(std::move(options)), selected_(selected),
          on_change_(std::move(on_change)), placeholder_(std::move(placeholder)) {}

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;

    // Self-managed selection: the dropdown updates its own index when the user
    // picks (so its trigger reflects the choice live in a retained tree), then
    // notifies via onChange. The app reads selected() at use time.
    void setSelected(int i) noexcept { selected_ = i; }

    [[nodiscard]] const std::vector<std::string>& options() const noexcept { return options_; }
    [[nodiscard]] int              selected()   const noexcept { return selected_; }
    [[nodiscard]] const OnChange&  onChange()   const noexcept { return on_change_; }
    [[nodiscard]] std::string      selectedLabel() const noexcept {
        return (selected_ >= 0 && selected_ < static_cast<int>(options_.size()))
                   ? options_[static_cast<size_t>(selected_)] : placeholder_;
    }

private:
    std::vector<std::string> options_;
    int                      selected_;
    OnChange                 on_change_;
    std::string              placeholder_;
};

// A date field: a trigger showing the selected date that opens a month-grid
// calendar popup on the overlay layer (like Dropdown). Picking a day selects it
// and closes; the ‹ › header buttons page months. Self-managed — the trigger
// reflects the current value in a retained tree; read value() at use and
// setValue() to seed. Requires the OverlayHost (every app root has one via
// XApplication::Run). Date math lives in xkit/calendar.h.
class DatePickerWidget : public Widget {
public:
    using OnChangeCallback = std::function<void(Date)>;

    explicit DatePickerWidget(Date value = {}, OnChangeCallback on_change = nullptr) noexcept
        : value_(calendar::clampToMonth(value.year, value.month, value.day)),
          on_change_(std::move(on_change)) {}

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;

    [[nodiscard]] Date value() const noexcept { return value_; }
    void setValue(Date d) noexcept { value_ = calendar::clampToMonth(d.year, d.month, d.day); }

    void setOnChange(OnChangeCallback cb) noexcept { on_change_ = std::move(cb); }
    [[nodiscard]] const OnChangeCallback& onChange() const noexcept { return on_change_; }

    // Trigger label, default ISO "YYYY-MM-DD"; overridable.
    void setFormatter(std::function<std::string(Date)> f) noexcept { formatter_ = std::move(f); }
    [[nodiscard]] std::string formatValue() const;   // defined in widget.cc

private:
    Date value_;
    OnChangeCallback on_change_;
    std::function<std::string(Date)> formatter_;
};

// A data table: fixed-width columns with header cells over string-cell rows.
// Clicking a header sorts by that column (toggling direction) — self-managed,
// numeric-aware (see table::sortedOrder). Optional onRowTap receives the row's
// ORIGINAL index (stable across sorting). In-layout, no overlay.
class TableWidget : public Widget {
public:
    using OnRowTap = std::function<void(int)>;

    TableWidget(std::vector<TableColumn> columns,
                std::vector<std::vector<std::string>> rows,
                OnRowTap on_row_tap = nullptr) noexcept
        : columns_(std::move(columns)), rows_(std::move(rows)),
          on_row_tap_(std::move(on_row_tap)) {}

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;

    void setSortable(bool s) noexcept { sortable_ = s; }
    // Sort by column i, toggling direction if it's already the sort column.
    void toggleSort(int i) noexcept {
        if (sort_col_ == i) ascending_ = !ascending_;
        else { sort_col_ = i; ascending_ = true; }
    }

    // Highlight a row by its ORIGINAL index (-1 = none). A row tap sets this
    // itself (live highlight); apps can also seed/persist it across rebuilds.
    void setSelectedRow(int orig) noexcept { selected_ = orig; }
    [[nodiscard]] int selectedRow() const noexcept { return selected_; }

    [[nodiscard]] const std::vector<TableColumn>&              columns() const noexcept { return columns_; }
    [[nodiscard]] const std::vector<std::vector<std::string>>& rows()    const noexcept { return rows_; }
    [[nodiscard]] const OnRowTap& onRowTap()  const noexcept { return on_row_tap_; }
    [[nodiscard]] bool sortable()  const noexcept { return sortable_; }
    [[nodiscard]] int  sortCol()   const noexcept { return sort_col_; }
    [[nodiscard]] bool ascending() const noexcept { return ascending_; }

private:
    std::vector<TableColumn>              columns_;
    std::vector<std::vector<std::string>> rows_;
    OnRowTap                              on_row_tap_;
    bool                                  sortable_  = true;
    int                                   sort_col_  = -1;   // -1 = unsorted
    bool                                  ascending_ = true;
    int                                   selected_  = -1;   // highlighted row (original index)
};

// A connected row of mutually-exclusive segments (iOS-style). Self-managed like
// DropdownWidget: tapping a segment selects it and fires onChange; the highlight
// moves live. Segments are uniform fixed width. A compact in-layout alternative
// to a radio group for 2–5 short options.
class SegmentedControlWidget : public Widget {
public:
    using OnChange = std::function<void(int)>;

    SegmentedControlWidget(std::vector<std::string> segments, int selected,
                           OnChange on_change = nullptr, float segment_width = 90.0f) noexcept
        : segments_(std::move(segments)), selected_(selected),
          on_change_(std::move(on_change)), segment_width_(segment_width) {}

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;

    void setSelected(int i) noexcept { selected_ = i; }

    [[nodiscard]] const std::vector<std::string>& segments() const noexcept { return segments_; }
    [[nodiscard]] int             selected()     const noexcept { return selected_; }
    [[nodiscard]] const OnChange& onChange()     const noexcept { return on_change_; }
    [[nodiscard]] float           segmentWidth() const noexcept { return segment_width_; }
    [[nodiscard]] std::string     selectedLabel() const noexcept {
        return (selected_ >= 0 && selected_ < static_cast<int>(segments_.size()))
                   ? segments_[static_cast<size_t>(selected_)] : std::string{};
    }

private:
    std::vector<std::string> segments_;
    int                      selected_;
    OnChange                 on_change_;
    float                    segment_width_;
};

// Wraps a child and shows a small text tooltip after the pointer hovers it for
// a delay; hides on leave or click. The tooltip is a pointer-transparent overlay
// (never intercepts input). Requires the OverlayHost (every app root has one).
class TooltipWidget : public Widget {
public:
    TooltipWidget(std::shared_ptr<Widget> child, std::string text,
                  int delay_ms = 500) noexcept
        : child_(std::move(child)), text_(std::move(text)), delay_ms_(delay_ms) {}

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] std::shared_ptr<Widget> child() const noexcept { return child_; }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] int delayMs() const noexcept { return delay_ms_; }

    // Pure show decision, exposed for testing: show once the pointer has hovered
    // continuously past the delay, and only while not already shown or suppressed
    // (suppression clears on leave; set by a click).
    [[nodiscard]] static bool shouldShow(bool hovering, bool shown, bool suppressed,
                                         long elapsed_ms, long delay_ms) noexcept {
        return hovering && !shown && !suppressed && elapsed_ms >= delay_ms;
    }

private:
    std::shared_ptr<Widget> child_;
    std::string             text_;
    int                     delay_ms_;
};

class TextWidget : public Widget {
public:
    TextWidget(std::string text, float font_size) noexcept : text_(std::move(text)), font_size_(font_size) {}
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    void setText(std::string text) noexcept { text_ = std::move(text); }
    // Alias matching getText() convention used in some call sites
    [[nodiscard]] const std::string& getText() const noexcept { return text_; }
    void setColor(Color c) noexcept { color_ = c; hasColor_ = true; }
    void setColor(float r, float g, float b, float a) noexcept { setColor(Color{r,g,b,a}); }
    void setFontSize(float sz) noexcept { font_size_ = sz; }
    // Font selection — propagated to TextData.font_family_/font_weight_/font_style_
    void setFontFamily(std::string family) noexcept { font_family_ = std::move(family); }
    void setFontWeight(uint16_t weight) noexcept { font_weight_ = weight; } // CSS weight: 400=Regular, 700=Bold
    void setFontStyle(uint8_t style) noexcept { font_style_ = style; }     // 0=Normal, 1=Italic
    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] constexpr float font_size() const noexcept { return font_size_; }
    [[nodiscard]] Color color() const noexcept { return color_; }
    [[nodiscard]] bool hasColor() const noexcept { return hasColor_; }
    [[nodiscard]] const std::string& fontFamily() const noexcept { return font_family_; }
    [[nodiscard]] uint16_t fontWeight() const noexcept { return font_weight_; }
    [[nodiscard]] uint8_t  fontStyle()  const noexcept { return font_style_; }
private:
    std::string text_;
    float font_size_;
    Color color_{1,1,1,1};
    bool hasColor_{false};
    std::string font_family_;   // empty = default UI font
    uint16_t    font_weight_{0}; // 0 = inherit default (400)
    uint8_t     font_style_{0};  // 0 = Normal
};

class ButtonWidget : public Widget {
public:
    using OnPressedCallback = std::function<void()>;

    // Full constructor: child widget + optional callback
    ButtonWidget(std::shared_ptr<Widget> child, OnPressedCallback onPressed = nullptr) noexcept
        : child_(std::move(child)), onPressed_(std::move(onPressed)) {}

    // Convenience: string label shorthand — creates an internal TextWidget
    explicit ButtonWidget(std::string label, OnPressedCallback onPressed = nullptr) noexcept
        : child_(std::make_shared<TextWidget>(std::move(label), 13.0f)),
          onPressed_(std::move(onPressed)) {}

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] std::shared_ptr<Widget> child()      const noexcept { return child_; }
    // Alias used by call sites that expect getContent()
    [[nodiscard]] std::shared_ptr<Widget> getContent() const noexcept { return child_; }
    [[nodiscard]] const OnPressedCallback& onPressed() const noexcept { return onPressed_; }
    void setOnPressed(OnPressedCallback cb) noexcept { onPressed_ = std::move(cb); }

    // Fired on a right-click (secondary press) instead of onPressed — e.g. to
    // open a context menu. Left/middle/touch still fire onPressed.
    [[nodiscard]] const OnPressedCallback& onSecondaryPressed() const noexcept { return onSecondaryPressed_; }
    void setOnSecondaryPressed(OnPressedCallback cb) noexcept { onSecondaryPressed_ = std::move(cb); }

    // Convenience setters that forward to the internal TextWidget if present
    void setText(std::string text) noexcept {
        if (auto* tw = dynamic_cast<TextWidget*>(child_.get())) tw->setText(std::move(text));
    }
    [[nodiscard]] std::string getText() const noexcept {
        if (auto* tw = dynamic_cast<TextWidget*>(child_.get())) return tw->getText();
        return {};
    }
    void setFontSize(float sz) noexcept {
        if (auto* tw = dynamic_cast<TextWidget*>(child_.get())) tw->setFontSize(sz);
    }
    void setColor(float r, float g, float b, float a) noexcept {
        if (auto* tw = dynamic_cast<TextWidget*>(child_.get())) tw->setColor(r, g, b, a);
    }
    void setColor(Color c) noexcept {
        if (auto* tw = dynamic_cast<TextWidget*>(child_.get())) tw->setColor(c);
    }
    void onClick(OnPressedCallback cb) noexcept { onPressed_ = std::move(cb); }
    // Disabled state is inherited from Widget (setDisabled/isDisabled).

private:
    std::shared_ptr<Widget> child_;
    OnPressedCallback onPressed_;
    OnPressedCallback onSecondaryPressed_;
};

class ScrollViewWidget : public Widget {
public:
    explicit ScrollViewWidget(std::shared_ptr<Widget> child) noexcept
        : child_(std::move(child)) {}
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] std::shared_ptr<Widget> child() const noexcept { return child_; }
private:
    std::shared_ptr<Widget> child_;
};

class TextInputWidget : public Widget {
public:
    using OnChangedCallback = std::function<void(const std::string&)>;
    // Fired when the user presses Enter/Return, with the final text — for URL
    // bars, search fields and forms that act on commit rather than each keystroke.
    using OnSubmitCallback  = std::function<void(const std::string&)>;

    explicit TextInputWidget(std::string initial_text = "") noexcept
        : text_(std::move(initial_text)) {}
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] const std::string& text()    const noexcept { return text_; }
    [[nodiscard]] const std::string& getText() const noexcept { return text_; }

    void setOnChanged(OnChangedCallback cb) noexcept { onChange_ = std::move(cb); }
    [[nodiscard]] const OnChangedCallback& onChange() const noexcept { return onChange_; }
    void setOnSubmit(OnSubmitCallback cb) noexcept { onSubmit_ = std::move(cb); }
    [[nodiscard]] const OnSubmitCallback& onSubmit() const noexcept { return onSubmit_; }

    void setText(std::string text) noexcept { text_ = std::move(text); }
    void setPlaceholder(std::string hint) noexcept { placeholder_ = std::move(hint); }
    [[nodiscard]] const std::string& placeholder() const noexcept { return placeholder_; }

    // Password mode: the field stores the real text (text() returns it for auth)
    // but renders each character as '*' so it isn't shown on screen.
    void setPasswordMode(bool b) noexcept { password_ = b; }
    [[nodiscard]] bool passwordMode() const noexcept { return password_; }

    // Glyph size for the field's text/caret. Defaults to 16; raise it so the
    // text isn't dwarfed by a tall input box.
    void setFontSize(float f) noexcept { font_size_ = f; }
    [[nodiscard]] float fontSize() const noexcept { return font_size_; }

    // locale: 0=passthrough, 1=Japanese romaji→hiragana
    void setLocale(uint32_t locale) noexcept { locale_ = locale; }
    [[nodiscard]] uint32_t locale() const noexcept { return locale_; }

    // Autofocus: grab keyboard focus as soon as the element mounts, so a form's
    // first field is typeable without a click. Set before Run().
    void setAutofocus(bool b) noexcept { autofocus_ = b; }
    [[nodiscard]] bool autofocus() const noexcept { return autofocus_; }

    // Programmatic focus. The element installs a hook on mount; focus() then
    // routes keyboard input to this field — e.g. Enter on a username field can
    // advance focus to the password field.
    void setFocusHook(std::function<void()> f) noexcept { focus_hook_ = std::move(f); }
    void focus() const { if (focus_hook_) focus_hook_(); }

private:
    std::string text_;
    std::string placeholder_;
    OnChangedCallback onChange_;
    OnSubmitCallback  onSubmit_;
    uint32_t locale_{0};
    bool autofocus_{false};
    bool password_{false};
    float font_size_{16.0f};
    std::function<void()> focus_hook_;
};

// Multi-line text field. Like TextInputWidget, editing is append-at-end
// (typing appends, Backspace deletes the last character — including a newline,
// which merges lines); there is no mid-string caret. The difference is that
// Return inserts a newline instead of committing, and the content is rendered as
// a column of lines. Ctrl+Return fires onSubmit for forms that want an explicit
// "send". Wrap it in a ScrollViewWidget for long content.
class MultilineTextInputWidget : public Widget {
public:
    using OnChangedCallback = std::function<void(const std::string&)>;
    using OnSubmitCallback  = std::function<void(const std::string&)>;

    explicit MultilineTextInputWidget(std::string initial_text = "") noexcept
        : text_(std::move(initial_text)) {}
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] const std::string& text()    const noexcept { return text_; }
    [[nodiscard]] const std::string& getText() const noexcept { return text_; }

    void setOnChanged(OnChangedCallback cb) noexcept { onChange_ = std::move(cb); }
    [[nodiscard]] const OnChangedCallback& onChange() const noexcept { return onChange_; }
    // Fired on Ctrl+Return with the final text (Return alone inserts a newline).
    void setOnSubmit(OnSubmitCallback cb) noexcept { onSubmit_ = std::move(cb); }
    [[nodiscard]] const OnSubmitCallback& onSubmit() const noexcept { return onSubmit_; }

    void setText(std::string text) noexcept { text_ = std::move(text); }
    void setPlaceholder(std::string hint) noexcept { placeholder_ = std::move(hint); }
    [[nodiscard]] const std::string& placeholder() const noexcept { return placeholder_; }

    void setFontSize(float f) noexcept { font_size_ = f; }
    [[nodiscard]] float fontSize() const noexcept { return font_size_; }

    void setAutofocus(bool b) noexcept { autofocus_ = b; }
    [[nodiscard]] bool autofocus() const noexcept { return autofocus_; }

    void setFocusHook(std::function<void()> f) noexcept { focus_hook_ = std::move(f); }
    void focus() const { if (focus_hook_) focus_hook_(); }

private:
    std::string text_;
    std::string placeholder_;
    OnChangedCallback onChange_;
    OnSubmitCallback  onSubmit_;
    bool autofocus_{false};
    float font_size_{16.0f};
    std::function<void()> focus_hook_;
};

class ImageWidget : public Widget {
public:
    // Load from a file path (PNG, JPEG, etc.)
    explicit ImageWidget(std::string path) noexcept
        : path_(std::move(path)) {}

    // Raw RGBA pixel buffer (4 bytes per pixel, row-major).
    // Used by PDF viewer and other runtime-rendered surfaces.
    ImageWidget(std::vector<uint8_t> pixels, uint32_t w, uint32_t h) noexcept
        : pixels_(std::move(pixels)), pxw_(w), pxh_(h) {}

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] const std::string& path()       const noexcept { return path_; }
    [[nodiscard]] bool               hasPixels()  const noexcept { return !pixels_.empty(); }
    [[nodiscard]] const std::vector<uint8_t>& pixels() const noexcept { return pixels_; }
    [[nodiscard]] uint32_t pixelWidth()  const noexcept { return pxw_; }
    [[nodiscard]] uint32_t pixelHeight() const noexcept { return pxh_; }
    void setFitMode(ImageFitMode m) noexcept { fitMode_ = m; }
    [[nodiscard]] ImageFitMode fitMode() const noexcept { return fitMode_; }
private:
    std::string          path_;
    std::vector<uint8_t> pixels_;
    uint32_t             pxw_{0};
    uint32_t             pxh_{0};
    ImageFitMode         fitMode_{ImageFitMode::Stretch};
};

class WrapRowWidget : public Widget {
public:
    explicit WrapRowWidget(std::vector<std::shared_ptr<Widget>> children) noexcept
        : children_(std::move(children)) {}
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] const std::vector<std::shared_ptr<Widget>>& children() const noexcept { return children_; }
private:
    std::vector<std::shared_ptr<Widget>> children_;
};

class StackWidget : public Widget {
public:
    explicit StackWidget(std::vector<std::shared_ptr<Widget>> children) noexcept
        : children_(std::move(children)) {}
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] const std::vector<std::shared_ptr<Widget>>& children() const noexcept { return children_; }
private:
    std::vector<std::shared_ptr<Widget>> children_;
};

// AdaptiveWidget — responsive layout. Holds several alternatives, each with a
// minimum width; at layout time it shows the one with the largest min-width that
// still fits the available width (the first entry, conventionally min-width 0, is
// the compact fallback below every breakpoint). Lets a surface reflow for a narrow
// tablet/portrait screen vs a wide desktop. Only the chosen child is laid out,
// painted, hit-tested, and sent events.
//
//   auto layout = std::make_shared<AdaptiveWidget>(std::vector<AdaptiveWidget::Breakpoint>{
//       {0.0f,   compactColumn},   // narrow / tablet-portrait
//       {900.0f, wideTwoPane},     // desktop
//   });
class AdaptiveWidget : public Widget {
public:
    struct Breakpoint { float min_width; std::shared_ptr<Widget> child; };
    explicit AdaptiveWidget(std::vector<Breakpoint> breakpoints) noexcept {
        for (auto& b : breakpoints) { min_widths_.push_back(b.min_width); children_.push_back(std::move(b.child)); }
    }
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] const std::vector<std::shared_ptr<Widget>>& children() const noexcept { return children_; }
    [[nodiscard]] const std::vector<float>& minWidths() const noexcept { return min_widths_; }
private:
    std::vector<std::shared_ptr<Widget>> children_;
    std::vector<float> min_widths_;
};

class CheckboxWidget : public Widget {
public:
    CheckboxWidget(bool checked, std::function<void(bool)> on_changed = nullptr)
        : checked_(checked), on_changed_(std::move(on_changed)) {}
    bool checked() const { return checked_; }
    void setChecked(bool c) { checked_ = c; }
    const std::function<void(bool)>& onChanged() const { return on_changed_; }
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
private:
    bool checked_;
    std::function<void(bool)> on_changed_;
};

class SwitchWidget : public Widget {
public:
    SwitchWidget(bool value, std::function<void(bool)> on_changed = nullptr)
        : value_(value), on_changed_(std::move(on_changed)) {}
    bool value() const { return value_; }
    void setValue(bool v) { value_ = v; }
    void setOnChanged(std::function<void(bool)> cb) noexcept { on_changed_ = std::move(cb); }
    const std::function<void(bool)>& onChanged() const { return on_changed_; }
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
private:
    bool value_;
    std::function<void(bool)> on_changed_;
};

class SliderWidget : public Widget {
public:
    SliderWidget(float value, float min_val, float max_val,
                 std::function<void(float)> on_changed = nullptr)
        : min_val_(min_val),
          max_val_(max_val > min_val ? max_val : min_val), // guard inverted range
          on_changed_(std::move(on_changed))
    {
        value_ = std::clamp(value, min_val_, max_val_); // clamp initial value
    }
    float value() const { return value_; }
    float minVal() const { return min_val_; }
    float maxVal() const { return max_val_; }
    void setValue(float v) {
        // Guard: if range is degenerate (min==max), just pin to min.
        if (max_val_ <= min_val_) { value_ = min_val_; return; }
        value_ = std::clamp(v, min_val_, max_val_);
    }
    void setOnChanged(std::function<void(float)> cb) noexcept { on_changed_ = std::move(cb); }
    const std::function<void(float)>& onChanged() const { return on_changed_; }
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
private:
    float value_, min_val_, max_val_;
    std::function<void(float)> on_changed_;
};

// A compact numeric stepper: [−] value [+]. Clicking the minus/plus zones
// decrements/increments by step, clamped to [min,max]. Self-managed like
// Slider/Dropdown — the displayed value tracks state; read value() at use and
// setValue() to seed from a server. onChanged fires only when the value
// actually moves (a press at a bound that would overflow fires nothing).
//
// The control has a fixed overall width (default 140) so the ± hit-zones are
// deterministic: minus = [0, buttonWidth), plus = [width - buttonWidth, width).
class StepperWidget : public Widget {
public:
    StepperWidget(float value, float min_val, float max_val, float step = 1.0f,
                  std::function<void(float)> on_changed = nullptr)
        : min_val_(min_val),
          max_val_(max_val > min_val ? max_val : min_val),   // guard inverted range
          step_(step > 0.0f ? step : 1.0f),
          on_changed_(std::move(on_changed)) {
        value_ = std::clamp(value, min_val_, max_val_);
    }

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;

    [[nodiscard]] float value()  const noexcept { return value_; }
    [[nodiscard]] float minVal() const noexcept { return min_val_; }
    [[nodiscard]] float maxVal() const noexcept { return max_val_; }
    [[nodiscard]] float step()   const noexcept { return step_; }

    void setValue(float v) noexcept {
        value_ = (max_val_ <= min_val_) ? min_val_ : std::clamp(v, min_val_, max_val_);
    }
    void setStep(float s) noexcept { if (s > 0.0f) step_ = s; }

    // Pure state transitions (no callback) — return true if the value moved.
    bool stepUp()   noexcept { float o = value_; setValue(value_ + step_); return value_ != o; }
    bool stepDown() noexcept { float o = value_; setValue(value_ - step_); return value_ != o; }

    [[nodiscard]] bool atMin() const noexcept { return value_ <= min_val_; }
    [[nodiscard]] bool atMax() const noexcept { return value_ >= max_val_; }

    void setOnChanged(std::function<void(float)> cb) noexcept { on_changed_ = std::move(cb); }
    [[nodiscard]] const std::function<void(float)>& onChanged() const noexcept { return on_changed_; }

    // Overall control width + the width of each ± button hit-zone.
    void  setWidth(float w)        noexcept { width_ = w; }
    [[nodiscard]] float width()       const noexcept { return width_; }
    void  setButtonWidth(float w)  noexcept { button_width_ = w; }
    [[nodiscard]] float buttonWidth() const noexcept { return button_width_; }

    // Custom value→string; the default trims to an integer when integral, else %g.
    void setFormatter(std::function<std::string(float)> f) noexcept { formatter_ = std::move(f); }
    [[nodiscard]] std::string formatValue() const;   // defined in widget.cc

private:
    float value_, min_val_, max_val_, step_;
    float width_{140.0f};
    float button_width_{36.0f};
    std::function<void(float)> on_changed_;
    std::function<std::string(float)> formatter_;
};

class ProgressBarWidget : public Widget {
public:
    explicit ProgressBarWidget(float value) : value_(std::clamp(value, 0.0f, 1.0f)) {}
    float value() const { return value_; }
    void setValue(float v) { value_ = std::clamp(v, 0.0f, 1.0f); }
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
private:
    float value_;
};

// An indeterminate activity spinner (a ring of fading dots) that animates on its
// own from wall-clock time — drop it in while something loads. diameter in px,
// dot count, and seconds per full revolution; colour defaults to the theme
// accent. Unlike ProgressBar (determinate 0..1) this shows unknown-duration work.
class SpinnerWidget : public Widget {
public:
    explicit SpinnerWidget(float diameter = 32.0f, int dot_count = 8,
                           float period_seconds = 0.9f) noexcept
        : diameter_(diameter),
          dot_count_(dot_count < 1 ? 1 : dot_count),
          period_(period_seconds > 0.0f ? period_seconds : 0.9f) {}

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;

    [[nodiscard]] float diameter() const noexcept { return diameter_; }
    [[nodiscard]] int   dotCount() const noexcept { return dot_count_; }
    [[nodiscard]] float period()   const noexcept { return period_; }

    void setColor(Color c) noexcept { color_ = c; hasColor_ = true; }
    [[nodiscard]] bool  hasColor() const noexcept { return hasColor_; }
    [[nodiscard]] Color color()    const noexcept { return color_; }

private:
    float diameter_;
    int   dot_count_;
    float period_;
    Color color_{0, 0, 0, 1};
    bool  hasColor_{false};
};

class RadioGroupController {
public:
    static std::shared_ptr<RadioGroupController> create() {
        return std::make_shared<RadioGroupController>();
    }
    int selectedIndex() const { return selected_; }
    void select(int idx) {
        if (selected_ == idx) return;
        selected_ = idx;
        for (auto& cb : subscribers_) cb(idx);
    }
    void subscribe(std::function<void(int)> cb) { subscribers_.push_back(std::move(cb)); }
private:
    int selected_{-1};
    std::vector<std::function<void(int)>> subscribers_;
};

class RadioButtonWidget : public Widget {
public:
    RadioButtonWidget(std::shared_ptr<RadioGroupController> group, int index)
        : group_(std::move(group)), index_(index) {}
    const std::shared_ptr<RadioGroupController>& group() const { return group_; }
    int index() const { return index_; }
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
private:
    std::shared_ptr<RadioGroupController> group_;
    int index_;
};

class CardWidget : public Widget {
public:
    explicit CardWidget(std::shared_ptr<Widget> child) : child_(std::move(child)) {}
    const std::shared_ptr<Widget>& child() const { return child_; }
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
private:
    std::shared_ptr<Widget> child_;
};

// A collapsible disclosure section: a clickable header row (with a ▸/▾ caret)
// that shows or hides its content below. The staple of settings panels and
// sidebars. Collapsed by default; clicking anywhere on the header toggles.
//
// When collapsed the content is removed from the layout entirely — the section
// shrinks to the header's height and surrounding widgets reflow — rather than
// merely being hidden in place. onToggle fires with the new state on each click.
//
// Example:
//   auto ex = make_shared<ExpanderWidget>("Advanced", make_shared<FlexColumn>(...));
//   ex->setOnToggle([](bool open){ /* persist the pref */ });
class ExpanderWidget : public Widget {
public:
    using OnToggleCallback = std::function<void(bool expanded)>;

    ExpanderWidget(std::shared_ptr<Widget> header,
                   std::shared_ptr<Widget> content,
                   bool expanded = false) noexcept
        : header_(std::move(header)), content_(std::move(content)), expanded_(expanded) {}

    // Convenience: a plain title string becomes a 14pt TextWidget header.
    ExpanderWidget(std::string title,
                   std::shared_ptr<Widget> content,
                   bool expanded = false) noexcept
        : header_(std::make_shared<TextWidget>(std::move(title), 14.0f)),
          content_(std::move(content)), expanded_(expanded) {}

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;

    [[nodiscard]] const std::shared_ptr<Widget>& header()  const noexcept { return header_; }
    [[nodiscard]] const std::shared_ptr<Widget>& content() const noexcept { return content_; }

    [[nodiscard]] bool expanded() const noexcept { return expanded_; }
    void setExpanded(bool e) noexcept { expanded_ = e; }
    void toggle() noexcept { expanded_ = !expanded_; }

    void setOnToggle(OnToggleCallback cb) noexcept { onToggle_ = std::move(cb); }
    [[nodiscard]] const OnToggleCallback& onToggle() const noexcept { return onToggle_; }

    // Vertical gap between the header row and the content (when expanded).
    void setContentGap(float g) noexcept { contentGap_ = g; }
    [[nodiscard]] float contentGap() const noexcept { return contentGap_; }

private:
    std::shared_ptr<Widget> header_;
    std::shared_ptr<Widget> content_;
    bool  expanded_{false};
    float contentGap_{6.0f};
    OnToggleCallback onToggle_;
};

class DividerWidget : public Widget {
public:
    explicit DividerWidget(float thickness = 1.0f) : thickness_(thickness) {}
    float thickness() const { return thickness_; }
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
private:
    float thickness_;
};

// A splitter handle placed between two flex panes. It renders a thin bar and
// shows a resize cursor on hover — col-resize for a Vertical bar (between
// left/right panes), row-resize for a Horizontal bar (between top/bottom panes).
class SplitterWidget : public Widget {
public:
    enum class Orientation { Vertical, Horizontal };
    // Called during a drag with the split position as a fraction (0..1) of the
    // container along the split axis. The host adjusts its layout (e.g. sets the
    // two panes' flex-grow to ratio : 1-ratio).
    using OnDragCallback = std::function<void(float ratio)>;
    explicit SplitterWidget(Orientation o, float thickness = 6.0f) noexcept
        : orientation_(o), thickness_(thickness) {}
    [[nodiscard]] Orientation orientation() const noexcept { return orientation_; }
    [[nodiscard]] float thickness() const noexcept { return thickness_; }
    void onDrag(OnDragCallback cb) noexcept { on_drag_ = std::move(cb); }
    [[nodiscard]] const OnDragCallback& onDragCallback() const noexcept { return on_drag_; }
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
private:
    Orientation orientation_;
    float thickness_;
    OnDragCallback on_drag_;
};

class ListTileWidget : public Widget {
public:
    ListTileWidget() = default;
    ListTileWidget& setTitle(std::shared_ptr<Widget> w) { title_ = w; return *this; }
    ListTileWidget& setSubtitle(std::shared_ptr<Widget> w) { subtitle_ = w; return *this; }
    ListTileWidget& setLeading(std::shared_ptr<Widget> w) { leading_ = w; return *this; }
    ListTileWidget& setTrailing(std::shared_ptr<Widget> w) { trailing_ = w; return *this; }
    ListTileWidget& setOnTap(std::function<void()> cb) { on_tap_ = cb; return *this; }
    const std::shared_ptr<Widget>& title() const { return title_; }
    const std::shared_ptr<Widget>& subtitle() const { return subtitle_; }
    const std::shared_ptr<Widget>& leading() const { return leading_; }
    const std::shared_ptr<Widget>& trailing() const { return trailing_; }
    const std::function<void()>& onTap() const { return on_tap_; }
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
private:
    std::shared_ptr<Widget> title_, subtitle_, leading_, trailing_;
    std::function<void()> on_tap_;
};

// DragSourceWidget ─────────────────────────────────────────────────────────────
// Wraps any child widget and adds drag-and-drop source behaviour.
// When the user presses and moves the pointer more than kDragThreshold logical
// pixels, the data_provider callback is invoked to obtain the payload, and
// XApplication::startDrag() is called with the given MIME type.
// Short taps (< threshold) fall through to the child widget as normal.
class DragSourceWidget : public Widget {
public:
    DragSourceWidget(std::shared_ptr<Widget> child,
                     std::string mime_type,
                     std::function<std::string()> data_provider) noexcept
        : child_(std::move(child)),
          mime_type_(std::move(mime_type)),
          data_provider_(std::move(data_provider)) {}

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] const std::shared_ptr<Widget>& child() const noexcept { return child_; }
    [[nodiscard]] const std::string& mimeType() const noexcept { return mime_type_; }
    [[nodiscard]] const std::function<std::string()>& dataProvider() const noexcept {
        return data_provider_;
    }
private:
    std::shared_ptr<Widget> child_;
    std::string mime_type_;
    std::function<std::string()> data_provider_;
};

// GestureDetectorWidget ────────────────────────────────────────────────────────
// Wraps any child and recognises touch gestures:
//   onTap(x, y)           — finger pressed and released within kTapRadius
//   onPan(dx, dy)         — slot-0 finger dragged beyond the tap threshold
//   onPinch(scale, dx, dy)— two-finger pinch/spread (scale: 1.0 = unchanged)
class GestureDetectorWidget : public Widget {
public:
    using TapCallback   = std::function<void(float x, float y)>;
    using PanCallback   = std::function<void(float dx, float dy)>;
    using PinchCallback = std::function<void(float scale, float dx, float dy)>;

    GestureDetectorWidget(std::shared_ptr<Widget> child,
                          TapCallback   on_tap   = nullptr,
                          PanCallback   on_pan   = nullptr,
                          PinchCallback on_pinch = nullptr) noexcept
        : child_(std::move(child)),
          on_tap_(std::move(on_tap)),
          on_pan_(std::move(on_pan)),
          on_pinch_(std::move(on_pinch)) {}

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] const std::shared_ptr<Widget>& child()   const noexcept { return child_; }
    [[nodiscard]] const TapCallback&   onTap()   const noexcept { return on_tap_; }
    [[nodiscard]] const PanCallback&   onPan()   const noexcept { return on_pan_; }
    [[nodiscard]] const PinchCallback& onPinch() const noexcept { return on_pinch_; }
private:
    std::shared_ptr<Widget> child_;
    TapCallback   on_tap_;
    PanCallback   on_pan_;
    PinchCallback on_pinch_;
};

// DrawingSurfaceWidget ─────────────────────────────────────────────────────────
// Transparent overlay that receives stylus and finger-drawing input.
//   onStroke(x, y, pressure, tilt_x, tilt_y)
//       pressure: 0.0–1.0 (finger touches report 0.5)
//       tilt_x/y: degrees from vertical (stylus only; 0 for finger)
//   onLift() — stylus tip left surface / finger lifted
class DrawingSurfaceWidget : public Widget {
public:
    using StrokeCallback = std::function<void(float x, float y,
                                              float pressure,
                                              float tilt_x, float tilt_y)>;
    using LiftCallback   = std::function<void()>;

    DrawingSurfaceWidget(StrokeCallback on_stroke,
                         LiftCallback   on_lift = nullptr) noexcept
        : on_stroke_(std::move(on_stroke)), on_lift_(std::move(on_lift)) {}

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] const StrokeCallback& onStroke() const noexcept { return on_stroke_; }
    [[nodiscard]] const LiftCallback&   onLift()   const noexcept { return on_lift_; }
private:
    StrokeCallback on_stroke_;
    LiftCallback   on_lift_;
};

class BadgeWidget : public Widget {
public:
    BadgeWidget(std::shared_ptr<Widget> child, int count)
        : child_(std::move(child)), count_(count) {}
    const std::shared_ptr<Widget>& child() const { return child_; }
    int count() const { return count_; }
    void setCount(int c) { count_ = c; }
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
private:
    std::shared_ptr<Widget> child_;
    int count_;
};

// FlexibleWidget ───────────────────────────────────────────────────────────────
// Wraps any child widget and marks it as a flex child with a given grow factor.
// Inside a FlexColumn or FlexRow, Flexible children share the remaining space
// proportional to their grow values (CSS flex-grow semantics).
//
// Example — fill the remaining vertical space:
//   FlexColumn({
//       make_shared<TextWidget>("Header", 16),
//       make_shared<Flexible>(make_shared<ScrollViewWidget>(...)),  // grow=1
//   })
class FlexibleWidget : public Widget {
public:
    explicit FlexibleWidget(std::shared_ptr<Widget> child,
                            float grow   = 1.0f,
                            float shrink = 1.0f,
                            float basis  = -1.0f) noexcept
        : child_(std::move(child)), grow_(grow), shrink_(shrink), basis_(basis) {}

    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] const std::shared_ptr<Widget>& child() const noexcept { return child_; }
    [[nodiscard]] float grow()   const noexcept { return grow_; }
    [[nodiscard]] float shrink() const noexcept { return shrink_; }
    [[nodiscard]] float basis()  const noexcept { return basis_; }

private:
    std::shared_ptr<Widget> child_;
    float grow_{1.0f};
    float shrink_{1.0f};
    float basis_{-1.0f};
};

// Convenience alias (matches Flutter / common naming)
using Flexible = FlexibleWidget;
using Expanded  = FlexibleWidget; // grow=1, fills all available space

// Root layer that renders its content full-viewport and draws the process's
// active overlays (see OverlayManager) above it, positioned relative to their
// anchors. XApplication::Run wraps every app's root in one, so any widget can
// pop a dropdown / menu / dialog via OverlayManager without the app plumbing a
// layer itself. Transparent when no overlays are active: input passes straight
// through to the content.
class OverlayHostWidget : public Widget {
public:
    explicit OverlayHostWidget(std::shared_ptr<Widget> content) noexcept
        : content_(std::move(content)) {}
    [[nodiscard]] std::shared_ptr<Element> createElement() noexcept override;
    [[nodiscard]] std::shared_ptr<Widget> content() const noexcept { return content_; }
private:
    std::shared_ptr<Widget> content_;
};

} // namespace fbx::xkit
