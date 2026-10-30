#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include "xkit/text_edit.h"
#include "xkit/overlay.h"
#include "xkit/application.h"
#include "xkit/clipboard.h"
#include "xrender/image_decoder.h"
#include "shared/text_replace.h"
#include <iostream>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <unistd.h>
#include <climits>
#include <unordered_map>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

namespace fbx::xkit {

namespace {

// The user's text replacements (Settings → Text Replacement), re-read when the
// file changes; stat'd at most once a second since this runs per keystroke.
const std::vector<shared::textreplace::Entry>& textReplacements() {
    static std::vector<shared::textreplace::Entry> entries;
    static time_t mtime = -1;
    static auto last_check = std::chrono::steady_clock::time_point{};
    auto now = std::chrono::steady_clock::now();
    if (now - last_check < std::chrono::seconds(1)) return entries;
    last_check = now;
    const char* home = std::getenv("HOME");
    if (!home) return entries;
    std::string path = shared::textreplace::filePath(home);
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) { entries.clear(); mtime = -1; return entries; }
    if (st.st_mtime == mtime) return entries;
    mtime = st.st_mtime;
    std::ifstream f(path);
    std::stringstream body;
    body << f.rdbuf();
    entries = shared::textreplace::parse(body.str());
    return entries;
}

// Directory containing the running executable, or "" if it can't be resolved.
// Mirrors engine.cc's helper so asset lookups don't depend on the cwd — the
// compositor spawns clients with cwd=out/, but a client's own resources live in
// the source tree relative to the build dir, not the cwd.
std::string executable_dir() {
#if defined(__FreeBSD__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    char buf[PATH_MAX];
    size_t len = sizeof(buf);
    if (sysctl(mib, 4, buf, &len, nullptr, 0) == 0 && len > 0) {
        std::string p(buf);
        auto slash = p.find_last_of('/');
        if (slash != std::string::npos) return p.substr(0, slash);
    }
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string p(buf);
        auto slash = p.find_last_of('/');
        if (slash != std::string::npos) return p.substr(0, slash);
    }
#endif
    return {};
}

// Resolve an image asset path. Absolute paths and cwd-relative paths that
// already exist are returned unchanged; a relative path that doesn't resolve
// from the cwd is retried against the executable's directory and its parents,
// so bundled resources (e.g. "core/shell/Resources/...") load whether the
// client is launched from the build dir, the source root, or spawned by the
// compositor with an arbitrary cwd. Falls back to the original string so a
// genuinely-missing file still produces the same decode failure as before.
std::string resolve_asset_path(const std::string& path) {
    if (path.empty() || path[0] == '/') return path;          // absolute
    if (access(path.c_str(), R_OK) == 0) return path;         // cwd-relative hit
    const std::string exe = executable_dir();
    if (!exe.empty()) {
        for (const char* prefix : {"/", "/../", "/../../"}) {
            std::string cand = exe + prefix + path;
            if (access(cand.c_str(), R_OK) == 0) return cand;
        }
    }
    return path;
}

} // namespace

// Alpha multiplier applied to a disabled control's whole subtree so it reads as
// inactive (paired with the not-allowed cursor and swallowed input).
static constexpr float kDisabledOpacity = 0.45f;

void Element::syncState() noexcept {
    if (renderNode_ && widget_) {
        renderNode_->setVisibility(widget_->isVisible());
    }
}

void apply_widget_props(std::shared_ptr<RenderNode> node, std::shared_ptr<Widget> widget) {
    if (widget->hasBackground()) node->setBackgroundColor(widget->backgroundColor());
    node->setBorderRadius(widget->borderRadius());
    node->setPadding(widget->padding());
    if (widget->hasFixedSize()) node->setFixedSize(widget->fixedWidth(), widget->fixedHeight());
    node->setAlignment(widget->alignment());
    node->setStackAnchor(widget->stackAnchor());
    node->setMinTouchSize(widget->minTouchSize());

    // Flex container properties
    node->setGap(widget->gap());
    node->setMainAxisAlignment(widget->mainAxisAlignment());
    node->setCrossAxisAlignment(widget->crossAxisAlignment());

    // Flex child properties
    node->setFlexGrow(widget->flexGrow());
    node->setFlexShrink(widget->flexShrink());
    node->setFlexBasis(widget->flexBasis());

    // Margin
    node->setMargin(widget->marginTop(), widget->marginRight(),
                    widget->marginBottom(), widget->marginLeft());

    // Accessibility
    node->setAccessibilityLabel(widget->accessibilityLabel());
    node->setAccessibilityRole(widget->accessibilityRole());

    // A widget-level cursor override wins only when explicitly set; otherwise
    // leave whatever the element chose (e.g. TextInput's Text, Button's Pointer).
    if (widget->cursor() != CursorShape::Inherit) node->setCursor(widget->cursor());
}

class MultiChildElement : public Element {
public:
    MultiChildElement(std::shared_ptr<Widget> widget, std::shared_ptr<RenderNode> node) {
        widget_ = std::move(widget);
        renderNode_ = std::move(node);
        apply_widget_props(renderNode_, widget_);
    }

    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        
        std::vector<std::shared_ptr<Widget>> children;
        if (auto flex = std::dynamic_pointer_cast<FlexColumn>(widget_)) children = flex->children();
        else if (auto flexR = std::dynamic_pointer_cast<FlexRow>(widget_)) children = flexR->children();
        else if (auto wrap = std::dynamic_pointer_cast<WrapRowWidget>(widget_)) children = wrap->children();
        else if (auto stack = std::dynamic_pointer_cast<StackWidget>(widget_)) children = stack->children();
        else if (auto adapt = std::dynamic_pointer_cast<AdaptiveWidget>(widget_)) children = adapt->children();

        for (const auto& child_widget : children) {
            // Skip null children — containers commonly hold conditional entries
            // (e.g. `cond ? widget : nullptr`); dereferencing one here would
            // crash the whole app during mount.
            if (!child_widget) continue;
            auto child_element = child_widget->createElement();
            if (!child_element) continue;
            child_element->mount(shared_from_this());
            children_elements_.push_back(child_element);
            renderNode_->addChild(child_element->getRenderNode());
        }
    }

    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        unmount();
        mount(parent_.lock());
    }

    void unmount() noexcept override {
        for (auto& child : children_elements_) child->unmount();
        children_elements_.clear();
        renderNode_->clearChildren();
        parent_.reset();
    }

    void onThemeChanged(const Theme& theme) noexcept override {
        for (auto& child : children_elements_) child->onThemeChanged(theme);
    }

    void syncState() noexcept override {
        renderNode_->setVisibility(widget_->isVisible());
        apply_widget_props(renderNode_, widget_);

        // Gather current widget children for reconciliation
        std::vector<std::shared_ptr<Widget>> raw;
        if (auto flex = std::dynamic_pointer_cast<FlexColumn>(widget_))        raw = flex->children();
        else if (auto flexR = std::dynamic_pointer_cast<FlexRow>(widget_))     raw = flexR->children();
        else if (auto wrap  = std::dynamic_pointer_cast<WrapRowWidget>(widget_)) raw = wrap->children();
        else if (auto stack = std::dynamic_pointer_cast<StackWidget>(widget_)) raw = stack->children();
        else if (auto adapt = std::dynamic_pointer_cast<AdaptiveWidget>(widget_)) raw = adapt->children();

        // Drop null children so the comparison matches mount(), which skips them.
        // A stored null (e.g. a forward-declared widget slot never back-filled)
        // would otherwise make the size differ from children_elements_ on every
        // frame, forcing a full remount each frame and re-decoding every image.
        std::vector<std::shared_ptr<Widget>> new_children;
        new_children.reserve(raw.size());
        for (auto& w : raw) if (w) new_children.push_back(w);

        // Fast path: identical widget instances in the same positions — just
        // recurse. This is the steady state for a retained widget tree.
        bool same = new_children.size() == children_elements_.size();
        for (size_t i = 0; same && i < new_children.size(); ++i)
            if (children_elements_[i]->sourceWidget() != new_children[i]) same = false;
        if (same) {
            for (auto& child : children_elements_) child->syncState();
            return;
        }

        // Reconcile by widget identity: reuse the existing element for any
        // widget instance that is still present, create elements only for new
        // widgets, and unmount the ones that disappeared. Unaffected siblings
        // (and their decoded images) survive a child-list change instead of the
        // whole container being rebuilt.
        std::unordered_map<Widget*, std::shared_ptr<Element>> existing;
        existing.reserve(children_elements_.size());
        for (auto& e : children_elements_) existing.emplace(e->sourceWidget().get(), e);

        auto self = shared_from_this();
        std::vector<std::shared_ptr<Element>> rebuilt;
        rebuilt.reserve(new_children.size());
        renderNode_->clearChildren();
        for (const auto& cw : new_children) {
            std::shared_ptr<Element> ce;
            if (auto it = existing.find(cw.get()); it != existing.end()) {
                ce = it->second;
                existing.erase(it);
                ce->syncState();
            } else {
                ce = cw->createElement();
                if (!ce) continue;
                ce->mount(self);
            }
            rebuilt.push_back(ce);
            renderNode_->addChild(ce->getRenderNode());
        }
        for (auto& [ptr, e] : existing) e->unmount();
        children_elements_ = std::move(rebuilt);
    }

    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        // Spatial hit-test for mouse events: only forward to the child under the
        // cursor. Topmost first (last painted), and never a hidden child: in a
        // stack, a lower layer or a hidden popup at the same spot used to take
        // the click meant for the visible one on top. (Rows/columns don't
        // overlap, so the order changes nothing there.)
        if (event.type_ == shared::InputEventType::MouseMove ||
            event.type_ == shared::InputEventType::MouseButton) {
            float mx = static_cast<float>(event.value_a_);
            float my = static_cast<float>(event.value_b_);
            for (auto it = children_elements_.rbegin(); it != children_elements_.rend(); ++it) {
                auto& child = *it;
                auto node = child->getRenderNode();
                if (!node->isVisible()) continue;
                auto off  = node->getAbsoluteOffset();
                auto sz   = node->getSize();
                if (mx >= off.x_ && mx < off.x_ + sz.width_ &&
                    my >= off.y_ && my < off.y_ + sz.height_) {
                    if (child->handleEvent(event)) return true;
                }
            }
            return false;
        }
        // Non-mouse events (key, wheel, etc.) fan out to all children.
        for (auto& child : children_elements_) {
            if (child->handleEvent(event)) return true;
        }
        return false;
    }

    [[nodiscard]] SemanticNode getSemanticTree() const noexcept override {
        SemanticNode node = Element::getSemanticTree();
        if (node.role == "Container" || node.role.empty()) node.role = "Container";
        for (const auto& child : children_elements_) {
            node.children.push_back(child->getSemanticTree());
        }
        return node;
    }

private:
    std::vector<std::shared_ptr<Element>> children_elements_;
};

class SingleChildElement : public Element {
public:
    SingleChildElement(std::shared_ptr<Widget> widget, std::shared_ptr<RenderNode> node) {
        widget_ = std::move(widget);
        renderNode_ = std::move(node);
        apply_widget_props(renderNode_, widget_);
    }
    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        
        std::shared_ptr<Widget> child_widget = nullptr;
        if (auto container = std::dynamic_pointer_cast<ContainerWidget>(widget_)) child_widget = container->child();
        else if (auto scroll = std::dynamic_pointer_cast<ScrollViewWidget>(widget_)) child_widget = scroll->child();
        else if (auto button = std::dynamic_pointer_cast<ButtonWidget>(widget_)) child_widget = button->child();

        if (child_widget && child_widget->isVisible()) {
            child_element_ = child_widget->createElement();
            child_element_->mount(shared_from_this());
            renderNode_->addChild(child_element_->getRenderNode());
        }
    }
    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        if (child_element_) child_element_->unmount();
        mount(parent_.lock());
    }
    void unmount() noexcept override { if (child_element_) child_element_->unmount(); renderNode_->clearChildren(); parent_.reset(); }

    void onThemeChanged(const Theme& theme) noexcept override {
        if (child_element_) child_element_->onThemeChanged(theme);
    }
    void syncState() noexcept override {
        renderNode_->setVisibility(widget_->isVisible());
        if (child_element_) child_element_->syncState();
    }
    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        if (child_element_ && child_element_->handleEvent(event)) return true;
        return false;
    }

    [[nodiscard]] SemanticNode getSemanticTree() const noexcept override {
        SemanticNode node = Element::getSemanticTree();
        if (child_element_) {
            node.children.push_back(child_element_->getSemanticTree());
        }
        return node;
    }

protected:
    std::shared_ptr<Element> child_element_;
};

std::shared_ptr<Element> ContainerWidget::createElement() noexcept { return std::make_shared<SingleChildElement>(shared_from_this(), std::make_shared<RenderBox>(0, 0)); }
std::shared_ptr<Element> FlexColumn::createElement() noexcept { return std::make_shared<MultiChildElement>(shared_from_this(), std::make_shared<RenderFlexColumn>()); }
std::shared_ptr<Element> FlexRow::createElement() noexcept { return std::make_shared<MultiChildElement>(shared_from_this(), std::make_shared<RenderFlexRow>()); }
std::shared_ptr<Element> WrapRowWidget::createElement() noexcept { return std::make_shared<MultiChildElement>(shared_from_this(), std::make_shared<RenderWrapRow>()); }

// TabBarWidget composes a FlexRow of tab headers and delegates to it, so the
// strip inherits FlexRow's layout + the buttons' rendering/hit-testing. The
// active header gets a raised surface background + accent-coloured label.
std::shared_ptr<Element> TabBarWidget::createElement() noexcept {
    const Theme& th = ThemeManager::getInstance().getCurrentTheme();
    auto row = std::make_shared<FlexRow>();
    row->setGap(2.0f);
    row->setCrossAxisAlignment(CrossAxisAlignment::Center);

    for (int i = 0; i < static_cast<int>(titles_.size()); ++i) {
        const bool is_active = (i == active_);

        auto label = std::make_shared<ButtonWidget>(
            titles_[i], [cb = on_select_, i]() { if (cb) cb(i); });
        label->setFontSize(13.0f);
        label->setColor(is_active ? th.accent : th.text);
        label->setAccessibilityRole("Tab");
        label->setAccessibilityLabel(titles_[i]);

        std::vector<std::shared_ptr<Widget>> parts{label};
        if (closable_) {
            auto close = std::make_shared<ButtonWidget>(
                std::string("×"), [cb = on_close_, i]() { if (cb) cb(i); });  // ×
            close->setFontSize(13.0f);
            close->setColor(th.text);
            close->setAccessibilityLabel("Close tab");
            parts.push_back(std::move(close));
        }

        auto header = std::make_shared<FlexRow>(std::move(parts));
        header->setGap(4.0f);
        header->setPadding(6.0f);
        header->setBorderRadius(6.0f);
        header->setCrossAxisAlignment(CrossAxisAlignment::Center);
        if (is_active) header->setBackgroundColor(th.surface);
        row->addChild(std::move(header));
    }
    return row->createElement();
}
std::shared_ptr<Element> StackWidget::createElement() noexcept { return std::make_shared<MultiChildElement>(shared_from_this(), std::make_shared<RenderStack>()); }

std::shared_ptr<Element> AdaptiveWidget::createElement() noexcept {
    auto rn = std::make_shared<RenderAdaptive>();
    rn->setMinWidths(min_widths_);
    return std::make_shared<MultiChildElement>(shared_from_this(), std::move(rn));
}

// OverlayHostElement — child[0] is the app content; children[1..] are the
// process's active overlays (OverlayManager), reconciled each frame by content-
// widget identity. Mouse input is routed overlay-first (topmost under the
// cursor); a click outside a non-modal overlay dismisses it; a modal overlay
// blocks background input. Transparent (pure pass-through) when no overlays.
class OverlayHostElement : public Element {
public:
    explicit OverlayHostElement(std::shared_ptr<Widget> widget) {
        widget_ = std::move(widget);
        renderNode_ = std::make_shared<RenderOverlayHost>();
    }

    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        auto host = std::static_pointer_cast<OverlayHostWidget>(widget_);
        if (host->content()) {
            content_element_ = host->content()->createElement();
            if (content_element_) {
                content_element_->mount(shared_from_this());
                renderNode_->addChild(content_element_->getRenderNode());
            }
        }
    }

    void update(std::shared_ptr<Widget> newWidget) noexcept override { widget_ = std::move(newWidget); }

    void unmount() noexcept override {
        if (content_element_) content_element_->unmount();
        for (auto& e : overlay_elements_) e->unmount();
        overlay_elements_.clear();
        renderNode_->clearChildren();
        parent_.reset();
    }

    void onThemeChanged(const Theme& theme) noexcept override {
        if (content_element_) content_element_->onThemeChanged(theme);
        for (auto& e : overlay_elements_) e->onThemeChanged(theme);
    }

    void syncState() noexcept override {
        auto rn = std::static_pointer_cast<RenderOverlayHost>(renderNode_);
        auto& mgr = OverlayManager::instance();

        // Auto-dismiss expired overlays (toasts) before reconciling.
        auto now = std::chrono::steady_clock::now();
        std::vector<uint64_t> expired;
        for (const auto& s : mgr.overlays())
            if (s.expires != std::chrono::steady_clock::time_point{} && now >= s.expires)
                expired.push_back(s.id);
        for (uint64_t id : expired) mgr.dismiss(id);

        auto& specs = mgr.overlays();

        // Reconcile overlay elements by content-widget identity (reuse survivors).
        std::unordered_map<Widget*, std::shared_ptr<Element>> existing;
        for (auto& e : overlay_elements_)
            if (e->sourceWidget()) existing.emplace(e->sourceWidget().get(), e);

        rn->clearChildren();
        rn->placement_.clear();
        if (content_element_) rn->addChild(content_element_->getRenderNode());  // child 0

        auto self = shared_from_this();
        std::vector<std::shared_ptr<Element>> rebuilt;
        for (auto& s : specs) {
            if (!s.content) continue;
            std::shared_ptr<Element> ce;
            if (auto it = existing.find(s.content.get()); it != existing.end()) {
                ce = it->second; existing.erase(it); ce->syncState();
            } else {
                ce = s.content->createElement();
                if (!ce) continue;
                ce->mount(self);
            }
            rebuilt.push_back(ce);
            rn->addChild(ce->getRenderNode());
            rn->placement_.push_back({s.anchor, s.preferred, s.centered, s.bottom, s.modal});
        }
        for (auto& [ptr, e] : existing) e->unmount();
        overlay_elements_ = std::move(rebuilt);

        if (content_element_) content_element_->syncState();

        // Write back the previous frame's resolved placement so click routing
        // (topmostAt) has real bounds; a one-frame lag is harmless for input.
        for (size_t i = 0; i < overlay_elements_.size() && i < specs.size(); ++i)
            if (i < rn->resolved_.size()) specs[i].bounds = rn->resolved_[i];
    }

    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        auto& mgr = OverlayManager::instance();
        auto& specs = mgr.overlays();
        const bool is_mouse = event.type_ == shared::InputEventType::MouseButton ||
                              event.type_ == shared::InputEventType::MouseMove   ||
                              event.type_ == shared::InputEventType::MouseWheel;
        if (specs.empty() || !is_mouse)
            return content_element_ ? content_element_->handleEvent(event) : false;

        overlay::Point pt{static_cast<float>(event.value_a_), static_cast<float>(event.value_b_)};
        int idx = OverlayManager::topmostAt(specs, pt);   // interactive only
        if (idx >= 0 && idx < static_cast<int>(overlay_elements_.size()))
            return overlay_elements_[static_cast<size_t>(idx)]->handleEvent(event);

        // Pointer is outside every interactive overlay. Tooltips are transparent,
        // so act on the topmost interactive overlay (if any) — never a tooltip.
        int top = OverlayManager::topmostInteractive(specs);
        if (top < 0) return content_element_ ? content_element_->handleEvent(event) : false;
        if (specs[static_cast<size_t>(top)].modal) return true;   // modal swallows background input
        if (event.type_ == shared::InputEventType::MouseButton && event.char_value_ != 0) {
            mgr.dismiss(specs[static_cast<size_t>(top)].id);       // outside click closes the popup
            return true;
        }
        return content_element_ ? content_element_->handleEvent(event) : false;
    }

private:
    std::shared_ptr<Element> content_element_;
    std::vector<std::shared_ptr<Element>> overlay_elements_;
};

std::shared_ptr<Element> OverlayHostWidget::createElement() noexcept {
    return std::make_shared<OverlayHostElement>(shared_from_this());
}

// DropdownElement — a rounded-surface trigger showing the current selection; a
// press opens an options popup via OverlayManager, anchored to the trigger's
// on-screen rect. Selecting an option fires onChange and dismisses the popup;
// an outside click dismisses it (handled by the OverlayHost). Toggling closed by
// re-clicking the trigger also falls out of the overlay's outside-click dismiss.
class DropdownElement : public Element {
public:
    explicit DropdownElement(std::shared_ptr<DropdownWidget> w) {
        widget_ = std::move(w);
        auto box = std::make_shared<RenderBox>(140.0f, 32.0f);
        box->setButtonLayout(true);           // shrink-wrap + rounded surface
        renderNode_ = box;
        renderNode_->setCursor(CursorShape::Pointer);
    }

    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        buildTrigger();
    }
    void update(std::shared_ptr<Widget> nw) noexcept override {
        widget_ = std::move(nw);
        buildTrigger();
    }
    void unmount() noexcept override {
        if (trigger_element_) trigger_element_->unmount();
        renderNode_->clearChildren();
        parent_.reset();
    }
    void onThemeChanged(const Theme& t) noexcept override {
        style(t);
        if (trigger_element_) trigger_element_->onThemeChanged(t);
    }
    void syncState() noexcept override {
        // Rebuild the trigger label when the selection changed (e.g. the user
        // picked from the popup); otherwise just re-sync the existing child.
        auto w = std::static_pointer_cast<DropdownWidget>(widget_);
        if (w->selectedLabel() != shown_label_) {
            buildTrigger();
        } else {
            style(ThemeManager::getInstance().getCurrentTheme());
            if (trigger_element_) trigger_element_->syncState();
        }
    }

    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        if (event.type_ == shared::InputEventType::MouseButton && event.char_value_ != 0) {
            openPopup();
            return true;
        }
        return trigger_element_ ? trigger_element_->handleEvent(event) : false;
    }

    [[nodiscard]] SemanticNode getSemanticTree() const noexcept override {
        auto abs = renderNode_->getAbsoluteOffset();
        auto sz  = renderNode_->getSize();
        auto w = std::static_pointer_cast<DropdownWidget>(widget_);
        return SemanticNode{
            .id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF),
            .role = "Dropdown", .label = w->selectedLabel(), .value = w->selectedLabel(),
            .x = abs.x_, .y = abs.y_, .width = sz.width_, .height = sz.height_,
        };
    }

private:
    void style(const Theme& th) noexcept {
        renderNode_->setBackgroundColor(th.surface);
        renderNode_->setBorderRadius(8.0f);
        renderNode_->setPadding(8.0f);
    }
    void buildTrigger() noexcept {
        renderNode_->clearChildren();
        auto w = std::static_pointer_cast<DropdownWidget>(widget_);
        const Theme& th = ThemeManager::getInstance().getCurrentTheme();
        style(th);
        shown_label_ = w->selectedLabel();
        auto text = std::make_shared<TextWidget>(shown_label_ + "   ▾", 13.0f);  // ▾
        text->setColor(th.text);
        trigger_element_ = text->createElement();
        trigger_element_->mount(shared_from_this());
        renderNode_->addChild(trigger_element_->getRenderNode());
    }
    void openPopup() noexcept {
        auto w = std::static_pointer_cast<DropdownWidget>(widget_);
        auto abs = renderNode_->getAbsoluteOffset();
        auto sz  = renderNode_->getSize();
        const Theme& th = ThemeManager::getInstance().getCurrentTheme();

        auto id_holder = std::make_shared<uint64_t>(0);
        std::vector<std::shared_ptr<Widget>> rows;
        rows.reserve(w->options().size());
        for (int i = 0; i < static_cast<int>(w->options().size()); ++i) {
            auto on_change = w->onChange();
            auto opt = std::make_shared<ButtonWidget>(
                w->options()[static_cast<size_t>(i)],
                [w, on_change, i, id_holder]() {
                    w->setSelected(i);              // self-manage → trigger reflects it live
                    if (on_change) on_change(i);
                    OverlayManager::instance().dismiss(*id_holder);
                });
            opt->setFontSize(13.0f);
            opt->setColor(th.text);
            rows.push_back(std::move(opt));
        }
        auto col = std::make_shared<FlexColumn>(std::move(rows));
        col->setGap(2.0f);
        col->setPadding(6.0f);
        col->setBackgroundColor(th.surface);
        col->setBorderRadius(8.0f);
        col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);

        OverlaySpec spec;
        spec.content   = col;
        spec.anchor    = overlay::Rect{abs.x_, abs.y_, sz.width_, sz.height_};
        spec.preferred = overlay::Side::Below;
        *id_holder = OverlayManager::instance().show(std::move(spec));
    }

    std::shared_ptr<Element> trigger_element_;
    std::string shown_label_;   // label currently drawn on the trigger
};

std::shared_ptr<Element> DropdownWidget::createElement() noexcept {
    return std::make_shared<DropdownElement>(std::static_pointer_cast<DropdownWidget>(shared_from_this()));
}

// ─── DatePickerElement ──────────────────────────────────────────────────────────
// Mirrors DropdownElement: a self-managing trigger (shows the formatted date)
// that opens a month-grid calendar overlay on press. The overlay card is built
// from public widgets (ButtonWidget day cells, FlexRow weeks) + OverlayManager,
// exactly like Menu/Dropdown popups. ‹/› re-open the calendar on the adjacent
// month; a day cell sets the value, fires onChange, and dismisses. The calendar
// grid comes from xkit/calendar.h (Sunday-first).
std::string DatePickerWidget::formatValue() const {
    if (formatter_) return formatter_(value_);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", value_.year, value_.month, value_.day);
    return buf;
}

class DatePickerElement : public Element {
    static constexpr float kCellW = 34.0f, kCellH = 30.0f, kGap = 2.0f;
public:
    explicit DatePickerElement(std::shared_ptr<DatePickerWidget> w) {
        widget_ = std::move(w);
        auto box = std::make_shared<RenderBox>(140.0f, 32.0f);
        box->setButtonLayout(true);
        renderNode_ = box;
        renderNode_->setCursor(CursorShape::Pointer);
    }

    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        buildTrigger();
    }
    void update(std::shared_ptr<Widget> nw) noexcept override {
        widget_ = std::move(nw);
        buildTrigger();
    }
    void unmount() noexcept override {
        if (trigger_element_) trigger_element_->unmount();
        renderNode_->clearChildren();
        parent_.reset();
    }
    void onThemeChanged(const Theme& t) noexcept override {
        style(t);
        if (trigger_element_) trigger_element_->onThemeChanged(t);
    }
    void syncState() noexcept override {
        auto w = std::static_pointer_cast<DatePickerWidget>(widget_);
        if (w->formatValue() != shown_label_) {
            buildTrigger();
        } else {
            style(ThemeManager::getInstance().getCurrentTheme());
            if (trigger_element_) trigger_element_->syncState();
        }
    }

    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        if (event.type_ == shared::InputEventType::MouseButton && event.char_value_ != 0) {
            auto w = std::static_pointer_cast<DatePickerWidget>(widget_);
            openCalendar(w->value().year, w->value().month);
            return true;
        }
        return trigger_element_ ? trigger_element_->handleEvent(event) : false;
    }

    [[nodiscard]] SemanticNode getSemanticTree() const noexcept override {
        auto abs = renderNode_->getAbsoluteOffset();
        auto sz  = renderNode_->getSize();
        auto w = std::static_pointer_cast<DatePickerWidget>(widget_);
        return SemanticNode{
            .id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF),
            .role = "DatePicker", .label = w->formatValue(), .value = w->formatValue(),
            .x = abs.x_, .y = abs.y_, .width = sz.width_, .height = sz.height_,
        };
    }

private:
    void style(const Theme& th) noexcept {
        renderNode_->setBackgroundColor(th.surface);
        renderNode_->setBorderRadius(8.0f);
        renderNode_->setPadding(8.0f);
    }
    void buildTrigger() noexcept {
        renderNode_->clearChildren();
        auto w = std::static_pointer_cast<DatePickerWidget>(widget_);
        const Theme& th = ThemeManager::getInstance().getCurrentTheme();
        style(th);
        shown_label_ = w->formatValue();
        auto text = std::make_shared<TextWidget>(shown_label_ + "   \xE2\x96\xBE", 13.0f);  // ▾
        text->setColor(th.text);
        trigger_element_ = text->createElement();
        trigger_element_->mount(shared_from_this());
        renderNode_->addChild(trigger_element_->getRenderNode());
    }

    // One fixed-size grid cell: a day button, or an empty spacer for padding.
    std::shared_ptr<Widget> dayCell(int vy, int vm, int d, const Theme& th) noexcept {
        if (d <= 0) return std::make_shared<Spacer>(kCellW, kCellH);
        auto w = std::static_pointer_cast<DatePickerWidget>(widget_);
        auto on_change = w->onChange();
        auto pid = popup_id_;
        auto btn = std::make_shared<ButtonWidget>(std::to_string(d),
            [w, on_change, vy, vm, d, pid]() {
                w->setValue(Date{vy, vm, d});
                if (on_change) on_change(w->value());
                if (pid) OverlayManager::instance().dismiss(*pid);
            });
        btn->setFixedSize(kCellW, kCellH);
        btn->setFontSize(12.0f);
        // Highlight the currently-selected day.
        Date cur = w->value();
        if (cur.year == vy && cur.month == vm && cur.day == d) {
            Color a = th.accent; a.a = 0.30f;
            btn->setBackgroundColor(a);
            btn->setBorderRadius(6.0f);
        }
        btn->setColor(th.text);
        return btn;
    }

    void openCalendar(int vy, int vm) noexcept {
        if (!popup_id_) popup_id_ = std::make_shared<uint64_t>(0);
        if (*popup_id_) OverlayManager::instance().dismiss(*popup_id_);   // nav re-open

        const Theme& th = ThemeManager::getInstance().getCurrentTheme();
        auto self = std::static_pointer_cast<DatePickerElement>(shared_from_this());

        // Header: ‹  Month YYYY  ›
        auto prev = std::make_shared<ButtonWidget>(std::string("\xE2\x80\xB9"),  // ‹
            [self, vy, vm]() { Date p = calendar::addMonths(Date{vy, vm, 1}, -1);
                               self->openCalendar(p.year, p.month); });
        auto next = std::make_shared<ButtonWidget>(std::string("\xE2\x80\xBA"),  // ›
            [self, vy, vm]() { Date n = calendar::addMonths(Date{vy, vm, 1}, +1);
                               self->openCalendar(n.year, n.month); });
        prev->setColor(th.text); next->setColor(th.text);
        auto title = std::make_shared<TextWidget>(
            std::string(calendar::monthName(vm)) + " " + std::to_string(vy), 13.0f);
        title->setColor(th.text);
        auto header = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{prev, title, next});
        header->setMainAxisAlignment(MainAxisAlignment::SpaceBetween);
        header->setCrossAxisAlignment(CrossAxisAlignment::Center);
        header->setFixedSize(kCellW * 7 + kGap * 6, kCellH);

        // Weekday header row (Sunday-first, matching calendar::dayOfWeek).
        static const char* kWd[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
        std::vector<std::shared_ptr<Widget>> wd;
        for (int i = 0; i < 7; ++i) {
            auto t = std::make_shared<TextWidget>(kWd[i], 11.0f);
            t->setColor({0.55f, 0.55f, 0.6f, 1.0f});
            t->setFixedSize(kCellW, 16.0f);
            t->setAlignment(Alignment::Center);
            wd.push_back(std::move(t));
        }
        auto weekdayRow = std::make_shared<FlexRow>(std::move(wd));
        weekdayRow->setGap(kGap);

        // Day grid: leading blanks + days, padded to full weeks of 7.
        const int lead = calendar::firstWeekday(vy, vm);
        const int dim  = calendar::daysInMonth(vy, vm);
        std::vector<std::shared_ptr<Widget>> cols;
        cols.push_back(header);
        cols.push_back(weekdayRow);
        int cell = 0;
        std::vector<std::shared_ptr<Widget>> week;
        auto flushWeek = [&]() {
            auto row = std::make_shared<FlexRow>(std::move(week));
            row->setGap(kGap);
            cols.push_back(std::move(row));
            week.clear();
        };
        for (; cell < lead; ++cell)        week.push_back(dayCell(vy, vm, 0, th));
        for (int d = 1; d <= dim; ++d) {
            week.push_back(dayCell(vy, vm, d, th));
            if (++cell % 7 == 0) flushWeek();
        }
        if (!week.empty()) { while (cell++ % 7 != 0) week.push_back(dayCell(vy, vm, 0, th)); flushWeek(); }

        auto card = std::make_shared<FlexColumn>(std::move(cols));
        card->setGap(kGap);
        card->setPadding(8.0f);
        card->setBackgroundColor(th.surface);
        card->setBorderRadius(10.0f);

        auto abs = renderNode_->getAbsoluteOffset();
        auto sz  = renderNode_->getSize();
        OverlaySpec spec;
        spec.content   = card;
        spec.anchor    = overlay::Rect{abs.x_, abs.y_, sz.width_, sz.height_};
        spec.preferred = overlay::Side::Below;
        *popup_id_ = OverlayManager::instance().show(std::move(spec));
    }

    std::shared_ptr<Element>  trigger_element_;
    std::string               shown_label_;
    std::shared_ptr<uint64_t> popup_id_;
};

std::shared_ptr<Element> DatePickerWidget::createElement() noexcept {
    return std::make_shared<DatePickerElement>(std::static_pointer_cast<DatePickerWidget>(shared_from_this()));
}

// TooltipElement — wraps one child (shrink-wrapping via a RenderFlexColumn) and
// shows a text bubble once the pointer has hovered the child past the delay.
// Hover is polled from the global cursor each frame (works even when the pointer
// stops moving); the tooltip is a pointer-transparent overlay. A click hides it
// and suppresses re-show until the pointer leaves and returns.
class TooltipElement : public Element {
public:
    explicit TooltipElement(std::shared_ptr<TooltipWidget> w) {
        widget_ = std::move(w);
        renderNode_ = std::make_shared<RenderFlexColumn>();
    }

    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        auto w = std::static_pointer_cast<TooltipWidget>(widget_);
        if (w->child()) {
            child_element_ = w->child()->createElement();
            child_element_->mount(shared_from_this());
            renderNode_->addChild(child_element_->getRenderNode());
        }
    }
    void update(std::shared_ptr<Widget> nw) noexcept override { widget_ = std::move(nw); }
    void unmount() noexcept override {
        hide();
        if (child_element_) child_element_->unmount();
        renderNode_->clearChildren();
        parent_.reset();
    }
    void onThemeChanged(const Theme& t) noexcept override {
        if (child_element_) child_element_->onThemeChanged(t);
    }

    void syncState() noexcept override {
        auto w = std::static_pointer_cast<TooltipWidget>(widget_);
        float cx = 0.0f, cy = 0.0f;
        XApplication::cursorPosition(cx, cy);
        auto abs = renderNode_->getAbsoluteOffset();
        auto sz  = renderNode_->getSize();
        bool hovering = sz.width_ > 0 && sz.height_ > 0 &&
                        cx >= abs.x_ && cx < abs.x_ + sz.width_ &&
                        cy >= abs.y_ && cy < abs.y_ + sz.height_;

        if (hovering && !was_hovering_)
            enter_ = std::chrono::steady_clock::now();
        if (!hovering) { suppressed_ = false; hide(); }
        was_hovering_ = hovering;

        long elapsed = hovering
            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - enter_).count()
            : 0;
        if (TooltipWidget::shouldShow(hovering, shown_, suppressed_, elapsed, w->delayMs()))
            show(abs, sz);

        if (child_element_) child_element_->syncState();
    }

    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        if (event.type_ == shared::InputEventType::MouseButton && event.char_value_ != 0) {
            hide();
            suppressed_ = true;   // don't re-pop until the pointer leaves and returns
        }
        return child_element_ ? child_element_->handleEvent(event) : false;
    }

private:
    void show(fbx::xkit::Offset abs, Size sz) noexcept {
        const Theme& th = ThemeManager::getInstance().getCurrentTheme();
        auto w = std::static_pointer_cast<TooltipWidget>(widget_);
        auto label = std::make_shared<TextWidget>(w->text(), 12.0f);
        label->setColor(th.text);
        auto bubble = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{label});
        bubble->setPadding(6.0f);
        bubble->setBackgroundColor(th.surface);
        bubble->setBorderRadius(6.0f);

        OverlaySpec spec;
        spec.content     = std::move(bubble);
        spec.anchor      = overlay::Rect{abs.x_, abs.y_, sz.width_, sz.height_};
        spec.preferred   = overlay::Side::Below;
        spec.interactive = false;   // pointer-transparent
        id_ = OverlayManager::instance().show(std::move(spec));
        shown_ = true;
    }
    void hide() noexcept {
        if (shown_) { OverlayManager::instance().dismiss(id_); shown_ = false; id_ = 0; }
    }

    std::shared_ptr<Element> child_element_;
    std::chrono::steady_clock::time_point enter_{};
    uint64_t id_ = 0;
    bool shown_ = false, was_hovering_ = false, suppressed_ = false;
};

std::shared_ptr<Element> TooltipWidget::createElement() noexcept {
    return std::make_shared<TooltipElement>(std::static_pointer_cast<TooltipWidget>(shared_from_this()));
}

// SegmentedControlElement — a RenderFlexRow of uniform fixed-width segment
// buttons; the selected one is accent-filled. Self-managed: a tap sets the
// widget's selection + fires onChange, and syncState rebuilds so the highlight
// tracks the choice (same pattern as DropdownElement).
class SegmentedControlElement : public Element {
public:
    explicit SegmentedControlElement(std::shared_ptr<SegmentedControlWidget> w) {
        widget_ = std::move(w);
        renderNode_ = std::make_shared<RenderFlexRow>();
    }

    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        rebuild();
    }
    void update(std::shared_ptr<Widget> nw) noexcept override { widget_ = std::move(nw); rebuild(); }
    void unmount() noexcept override {
        for (auto& e : segment_elements_) e->unmount();
        segment_elements_.clear();
        renderNode_->clearChildren();
        parent_.reset();
    }
    void onThemeChanged(const Theme&) noexcept override { rebuild(); }

    void syncState() noexcept override {
        auto w = std::static_pointer_cast<SegmentedControlWidget>(widget_);
        if (w->selected() != shown_selected_) {
            rebuild();
        } else {
            for (auto& e : segment_elements_) e->syncState();
        }
    }

    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        if (event.type_ == shared::InputEventType::MouseMove ||
            event.type_ == shared::InputEventType::MouseButton) {
            float mx = static_cast<float>(event.value_a_);
            float my = static_cast<float>(event.value_b_);
            for (auto& e : segment_elements_) {
                auto n = e->getRenderNode();
                auto o = n->getAbsoluteOffset();
                auto s = n->getSize();
                if (mx >= o.x_ && mx < o.x_ + s.width_ && my >= o.y_ && my < o.y_ + s.height_)
                    return e->handleEvent(event);
            }
            return false;
        }
        for (auto& e : segment_elements_) if (e->handleEvent(event)) return true;
        return false;
    }

private:
    void rebuild() noexcept {
        for (auto& e : segment_elements_) e->unmount();
        segment_elements_.clear();
        renderNode_->clearChildren();

        auto w = std::static_pointer_cast<SegmentedControlWidget>(widget_);
        const Theme& th = ThemeManager::getInstance().getCurrentTheme();
        renderNode_->setBackgroundColor(th.surface);
        renderNode_->setBorderRadius(8.0f);

        auto self = shared_from_this();
        for (int i = 0; i < static_cast<int>(w->segments().size()); ++i) {
            const bool active = (i == w->selected());
            auto on_change = w->onChange();
            auto seg = std::make_shared<ButtonWidget>(
                w->segments()[static_cast<size_t>(i)],
                [w, on_change, i]() {
                    w->setSelected(i);
                    if (on_change) on_change(i);
                });
            seg->setFixedSize(w->segmentWidth(), 28.0f);
            seg->setFontSize(13.0f);
            if (active) {
                seg->setBackgroundColor(th.accent);
                seg->setBorderRadius(6.0f);
                seg->setColor(1.0f, 1.0f, 1.0f, 1.0f);   // light text on accent
            } else {
                seg->setColor(th.text);
            }
            auto el = seg->createElement();
            el->mount(self);
            renderNode_->addChild(el->getRenderNode());
            segment_elements_.push_back(std::move(el));
        }
        shown_selected_ = w->selected();
    }

    std::vector<std::shared_ptr<Element>> segment_elements_;
    int shown_selected_ = -2;
};

std::shared_ptr<Element> SegmentedControlWidget::createElement() noexcept {
    return std::make_shared<SegmentedControlElement>(
        std::static_pointer_cast<SegmentedControlWidget>(shared_from_this()));
}

// TableElement — a RenderFlexColumn of: a header FlexRow (sortable header cells),
// a divider, then one FlexRow per data row (sorted). Self-managed sort: a header
// tap toggles the sort column/direction on the widget and syncState rebuilds so
// the rows reorder. A data-row tap fires onRowTap with the row's ORIGINAL index.
class TableElement : public Element {
public:
    explicit TableElement(std::shared_ptr<TableWidget> w) {
        widget_ = std::move(w);
        // Root is a horizontal scroller wrapping the column of header + rows, so
        // a table wider than its pane can be scrolled sideways (Shift+wheel).
        auto hscroll = std::make_shared<RenderHScroll>();
        content_ = std::make_shared<RenderFlexColumn>();
        hscroll->addChild(content_);
        renderNode_ = hscroll;
    }

    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        rebuild();
    }
    void update(std::shared_ptr<Widget> nw) noexcept override { widget_ = std::move(nw); rebuild(); }
    void unmount() noexcept override {
        for (auto& e : row_elements_) e->unmount();
        row_elements_.clear();
        content_->clearChildren();
        parent_.reset();
    }
    void onThemeChanged(const Theme&) noexcept override { rebuild(); }

    void syncState() noexcept override {
        auto w = std::static_pointer_cast<TableWidget>(widget_);
        if (w->sortCol() != shown_sort_col_ || w->ascending() != shown_ascending_ ||
            w->selectedRow() != shown_selected_) {
            rebuild();
        } else {
            for (auto& e : row_elements_) e->syncState();
        }
    }

    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        // Shift + wheel scrolls the table horizontally.
        if (event.type_ == shared::InputEventType::MouseWheel && (event.modifiers_ & 1u)) {
            auto hs = std::static_pointer_cast<RenderHScroll>(renderNode_);
            hs->setScrollX(hs->getScrollX() - static_cast<float>(event.value_b_) * 40.0f);
            return true;
        }
        if (event.type_ == shared::InputEventType::MouseMove ||
            event.type_ == shared::InputEventType::MouseButton) {
            float mx = static_cast<float>(event.value_a_);
            float my = static_cast<float>(event.value_b_);
            for (auto& e : row_elements_) {
                auto n = e->getRenderNode();
                auto o = n->getAbsoluteOffset();
                auto s = n->getSize();
                if (mx >= o.x_ && mx < o.x_ + s.width_ && my >= o.y_ && my < o.y_ + s.height_)
                    return e->handleEvent(event);
            }
            return false;
        }
        for (auto& e : row_elements_) if (e->handleEvent(event)) return true;
        return false;
    }

private:
    static MainAxisAlignment mainAxisFor(CellAlign a) noexcept {
        switch (a) {
            case CellAlign::Center: return MainAxisAlignment::Center;
            case CellAlign::Right:  return MainAxisAlignment::End;
            default:                return MainAxisAlignment::Start;
        }
    }

    // A fixed-width cell holding text aligned per the column.
    static std::shared_ptr<Widget> cell(const std::string& text, float width,
                                        float height, Color color, CellAlign align) {
        auto t = std::make_shared<TextWidget>(text, 13.0f);
        t->setColor(color);
        auto box = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{t});
        box->setFixedSize(width, height);
        box->setPadding(6.0f);
        box->setCrossAxisAlignment(CrossAxisAlignment::Center);
        box->setMainAxisAlignment(mainAxisFor(align));
        return box;
    }

    void rebuild() noexcept {
        for (auto& e : row_elements_) e->unmount();
        row_elements_.clear();
        content_->clearChildren();

        auto w = std::static_pointer_cast<TableWidget>(widget_);
        const Theme& th = ThemeManager::getInstance().getCurrentTheme();
        auto self = shared_from_this();
        const auto& cols = w->columns();

        auto addRow = [&](const std::shared_ptr<Widget>& row_widget) {
            auto el = row_widget->createElement();
            el->mount(self);
            content_->addChild(el->getRenderNode());
            row_elements_.push_back(std::move(el));
        };

        // Header: one cell per column; tapping sorts (with a direction arrow).
        std::vector<std::shared_ptr<Widget>> header_cells;
        header_cells.reserve(cols.size());
        for (int c = 0; c < static_cast<int>(cols.size()); ++c) {
            std::string title = cols[static_cast<size_t>(c)].title;
            if (w->sortable() && w->sortCol() == c) title += w->ascending() ? "  ^" : "  v";
            auto label = std::make_shared<TextWidget>(title, 13.0f);
            label->setColor(th.accent);
            auto box = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{label});
            box->setFixedSize(cols[static_cast<size_t>(c)].width, 30.0f);
            box->setPadding(6.0f);
            box->setCrossAxisAlignment(CrossAxisAlignment::Center);
            box->setMainAxisAlignment(mainAxisFor(cols[static_cast<size_t>(c)].align));
            if (w->sortable()) {
                header_cells.push_back(std::make_shared<ButtonWidget>(
                    box, [w, c]() { w->toggleSort(c); }));
            } else {
                header_cells.push_back(box);
            }
        }
        addRow(std::make_shared<FlexRow>(std::move(header_cells)));
        addRow(std::make_shared<DividerWidget>(1.0f));

        // Data rows in sorted order; a row tap reports its original index.
        std::vector<size_t> order =
            table::sortedOrder(w->rows(), w->sortable() ? w->sortCol() : -1, w->ascending());
        for (size_t disp = 0; disp < order.size(); ++disp) {
            size_t orig = order[disp];
            const auto& r = w->rows()[orig];
            std::vector<std::shared_ptr<Widget>> cells;
            cells.reserve(cols.size());
            for (size_t c = 0; c < cols.size(); ++c) {
                std::string txt = (c < r.size()) ? r[c] : std::string{};
                cells.push_back(cell(txt, cols[c].width, 28.0f, th.text, cols[c].align));
            }
            auto row = std::make_shared<FlexRow>(std::move(cells));
            const bool sel = (static_cast<int>(orig) == w->selectedRow());
            if (w->onRowTap() || sel) {
                auto cb = w->onRowTap();
                int oi = static_cast<int>(orig);
                auto row_btn = std::make_shared<ButtonWidget>(row, [w, cb, oi]() {
                    w->setSelectedRow(oi);        // live highlight (self-managed)
                    if (cb) cb(oi);
                });
                if (sel) {
                    Color hl = th.accent; hl.a = 0.30f;   // subtle selection tint
                    row_btn->setBackgroundColor(hl);
                }
                addRow(row_btn);
            } else {
                addRow(row);
            }
        }
        shown_sort_col_  = w->sortCol();
        shown_ascending_ = w->ascending();
        shown_selected_  = w->selectedRow();
    }

    std::shared_ptr<RenderNode> content_;   // the FlexColumn inside the h-scroller
    std::vector<std::shared_ptr<Element>> row_elements_;
    int  shown_sort_col_  = -2;
    bool shown_ascending_ = true;
    int  shown_selected_  = -2;
};

std::shared_ptr<Element> TableWidget::createElement() noexcept {
    return std::make_shared<TableElement>(std::static_pointer_cast<TableWidget>(shared_from_this()));
}

// FlexibleElement — mounts a RenderFlexible that wraps the child's render node.
class FlexibleElement : public SingleChildElement {
public:
    explicit FlexibleElement(std::shared_ptr<Widget> widget)
        : SingleChildElement(widget, [&]() -> std::shared_ptr<RenderNode> {
            auto fw = std::static_pointer_cast<FlexibleWidget>(widget);
            auto node = std::make_shared<RenderFlexible>(fw->grow(), fw->shrink(), fw->basis());
            apply_widget_props(node, widget);
            return node;
          }())
    {}

    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        auto fw = std::static_pointer_cast<FlexibleWidget>(widget_);
        if (fw->child()) {
            child_element_ = fw->child()->createElement();
            child_element_->mount(shared_from_this());
            renderNode_->addChild(child_element_->getRenderNode());
        }
    }

    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = newWidget;
        auto fw = std::static_pointer_cast<FlexibleWidget>(widget_);
        auto node = std::static_pointer_cast<RenderFlexible>(renderNode_);
        node->setFlexGrow(fw->grow());
        node->setFlexShrink(fw->shrink());
        node->setFlexBasis(fw->basis());
        apply_widget_props(renderNode_, widget_);
        if (child_element_) {
            if (fw->child()) child_element_->update(fw->child());
        } else if (fw->child()) {
            child_element_ = fw->child()->createElement();
            child_element_->mount(shared_from_this());
            renderNode_->addChild(child_element_->getRenderNode());
        }
    }

    void syncState() noexcept override {
        auto fw = std::static_pointer_cast<FlexibleWidget>(widget_);
        auto node = std::static_pointer_cast<RenderFlexible>(renderNode_);
        node->setFlexGrow(fw->grow());
        node->setFlexShrink(fw->shrink());
        node->setFlexBasis(fw->basis());
        apply_widget_props(renderNode_, widget_);
        if (child_element_) child_element_->syncState();
    }
};

std::shared_ptr<Element> FlexibleWidget::createElement() noexcept {
    return std::make_shared<FlexibleElement>(shared_from_this());
}

class TextElement : public Element {
public:
    explicit TextElement(std::shared_ptr<Widget> widget) {
        widget_ = std::move(widget);
        auto text_widget = std::static_pointer_cast<TextWidget>(widget_);
        auto node = std::make_shared<RenderText>(text_widget->text(), text_widget->font_size());
        if (text_widget->hasColor()) node->setColor(text_widget->color());
        renderNode_ = node;
        apply_widget_props(renderNode_, widget_);
    }
    void mount(std::shared_ptr<Element> parent) noexcept override { parent_ = parent; renderNode_->setOwnerElement(shared_from_this()); }
    void update(std::shared_ptr<Widget> newWidget) noexcept override { 
        widget_ = std::move(newWidget); 
        auto text_widget = std::static_pointer_cast<TextWidget>(widget_);
        auto text_node = std::static_pointer_cast<RenderText>(renderNode_);
        text_node->setText(text_widget->text());
        if (text_widget->hasColor()) text_node->setColor(text_widget->color());
        apply_widget_props(renderNode_, widget_);
    }
    void unmount() noexcept override { parent_.reset(); }

    // A retained label changed with setText()/setColor() after mounting (a
    // clock, a status line, a dialog message): carry it to the render node.
    // Without this the node kept the text it was created with.
    void syncState() noexcept override {
        Element::syncState();
        auto text_widget = std::static_pointer_cast<TextWidget>(widget_);
        auto text_node = std::static_pointer_cast<RenderText>(renderNode_);
        if (text_node->text() != text_widget->text()) text_node->setText(text_widget->text());
        if (text_widget->hasColor()) text_node->setColor(text_widget->color());
    }

    void onThemeChanged(const Theme& theme) noexcept override {
        auto text_widget = std::static_pointer_cast<TextWidget>(widget_);
        if (!text_widget->hasColor()) {
            std::static_pointer_cast<RenderText>(renderNode_)->setColor(theme.text);
        }
    }

    [[nodiscard]] SemanticNode getSemanticTree() const noexcept override {
        auto tw = std::static_pointer_cast<const TextWidget>(widget_);
        auto abs = renderNode_->getAbsoluteOffset();
        auto sz  = renderNode_->getSize();
        std::string lbl = renderNode_->accessibilityLabel().empty()
                              ? tw->text()
                              : renderNode_->accessibilityLabel();
        return SemanticNode{
            .id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF),
            .role = "Label", .label = lbl,
            .x = abs.x_, .y = abs.y_, .width = sz.width_, .height = sz.height_
        };
    }
};

std::shared_ptr<Element> TextWidget::createElement() noexcept { return std::make_shared<TextElement>(shared_from_this()); }

class ButtonElement : public SingleChildElement {
public:
    explicit ButtonElement(std::shared_ptr<ButtonWidget> widget)
        : SingleChildElement(widget, std::make_shared<RenderBox>(100, 40)) {
        renderNode_->setCursor(CursorShape::Pointer);
        // Shrink-wrap to the label, centre it, and drop the shadow when the
        // button is transparent (see RenderBox::setButtonLayout).
        std::static_pointer_cast<RenderBox>(renderNode_)->setButtonLayout(true);
    }

    void syncState() noexcept override {
        // Reflect the (possibly changed) disabled state into the hover cursor
        // each frame: not-allowed when disabled, the hand otherwise.
        bool disabled = std::static_pointer_cast<ButtonWidget>(widget_)->isDisabled();
        renderNode_->setCursor(disabled ? CursorShape::NotAllowed
                                        : CursorShape::Pointer);
        renderNode_->setOpacity(disabled ? kDisabledOpacity : 1.0f);
        SingleChildElement::syncState();
    }

    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        auto button = std::static_pointer_cast<ButtonWidget>(widget_);
        if (event.type_ == shared::InputEventType::MouseMove) {
            auto pos = renderNode_->getAbsoluteOffset();
            auto size = renderNode_->getSize();
            float mx = static_cast<float>(event.value_a_);
            float my = static_cast<float>(event.value_b_);
            if (mx >= pos.x_ && mx <= pos.x_ + size.width_ && my >= pos.y_ && my <= pos.y_ + size.height_) {
                // Tactile feedback logic...
            }
        }
        // Fire once, on press (char_value_ is the real button state; value_b_ holds
        // the injected cursor position for mouse buttons).
        if (event.type_ == shared::InputEventType::MouseButton && event.char_value_ != 0) {
            // A disabled button swallows the press and fires nothing.
            if (button->isDisabled()) return true;
            // Right-click → secondary action (context menu); left/middle/touch →
            // the primary action. An un-encoded button (Unknown) is treated as
            // primary so synthetic presses still activate.
            if (XApplication::pointerButton(event) == XApplication::PointerButton::Right) {
                if (button->onSecondaryPressed()) { button->onSecondaryPressed()(); return true; }
                return false;   // no secondary handler → leave the right-click unconsumed
            }
            if (button->onPressed()) { button->onPressed()(); return true; }
        }
        return Element::handleEvent(event);
    }

    [[nodiscard]] SemanticNode getSemanticTree() const noexcept override {
        auto abs = renderNode_->getAbsoluteOffset();
        auto sz  = renderNode_->getSize();
        // Try to extract label from child (usually a TextElement).
        std::string lbl = renderNode_->accessibilityLabel();
        if (lbl.empty() && child_element_) {
            auto child_tree = child_element_->getSemanticTree();
            lbl = child_tree.label;
        }
        return SemanticNode{
            .id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF),
            .role = "Button", .label = lbl,
            .x = abs.x_, .y = abs.y_, .width = sz.width_, .height = sz.height_
        };
    }
};

std::shared_ptr<Element> ButtonWidget::createElement() noexcept { return std::make_shared<ButtonElement>(std::static_pointer_cast<ButtonWidget>(shared_from_this())); }

class ScrollViewElement : public SingleChildElement {
public:
    explicit ScrollViewElement(std::shared_ptr<ScrollViewWidget> widget)
        : SingleChildElement(widget, std::make_shared<RenderScrollView>()) {}

    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        if (event.type_ == shared::InputEventType::MouseWheel) {
            // Add impulse; physics runs in syncState() each frame.
            velocity_y_ += event.value_b_ * 60.0f;
            return true;
        }
        // Forward all other events to the scrollable content.
        if (child_element_ && child_element_->handleEvent(event)) return true;
        return false;
    }

    void syncState() noexcept override {
        // Momentum: apply velocity then decay (exponential, ~0.88 per frame @ 60fps)
        if (std::abs(velocity_y_) > 0.5f) {
            auto scroll_node = std::static_pointer_cast<RenderScrollView>(renderNode_);
            scroll_node->setScrollY(scroll_node->getScrollY() + velocity_y_);
            velocity_y_ *= kDecay;
        } else {
            velocity_y_ = 0.0f;
        }
        SingleChildElement::syncState();
    }

private:
    static constexpr float kDecay = 0.88f;
    float velocity_y_{0.0f};
};

std::shared_ptr<Element> ScrollViewWidget::createElement() noexcept { return std::make_shared<ScrollViewElement>(std::static_pointer_cast<ScrollViewWidget>(shared_from_this())); }

class SpacerElement : public Element {
public:
    explicit SpacerElement(std::shared_ptr<Spacer> widget) {
        widget_ = widget;
        renderNode_ = std::make_shared<RenderSpacer>(widget->width(), widget->height());
        // A flexible spacer must actually flex — otherwise flex containers can't
        // expand it (e.g. to push a taskbar/dock to the bottom of the screen).
        if (widget->flexible()) renderNode_->setFlexGrow(1.0f);
    }
    void mount(std::shared_ptr<Element> p) noexcept override { parent_ = p; renderNode_->setOwnerElement(shared_from_this()); }
    void update(std::shared_ptr<Widget> w) noexcept override { widget_ = w; }
    void unmount() noexcept override { parent_.reset(); }
};
std::shared_ptr<Element> Spacer::createElement() noexcept { return std::make_shared<SpacerElement>(std::static_pointer_cast<Spacer>(shared_from_this())); }

class RenderCursorBox : public RenderBox {
public:
    using RenderBox::RenderBox;
    void setColor(const Color& color) {
        backgroundColor_ = color;
    }
};

// Byte index of the next UTF-8 codepoint boundary after i.
static std::size_t cpNext(const std::string& s, std::size_t i) noexcept {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}
// Map a horizontal offset x (px, relative to the text's left edge) to the nearest
// codepoint boundary in `disp`, by measuring prefixes. Clamps to [0, size].
static std::size_t indexFromX(const std::string& disp, float x, float fs) noexcept {
    if (x <= 0.0f || disp.empty()) return 0;
    std::size_t i = 0;
    float prev = 0.0f;
    while (i < disp.size()) {
        std::size_t j = cpNext(disp, i);
        float w = fbx::xrender::FontManager::measureText(disp.substr(0, j), fs);
        if (w >= x) return (x - prev) <= (w - x) ? i : j;   // nearer boundary wins
        prev = w; i = j;
    }
    return disp.size();
}

// Record a pre-edit checkpoint at the start of each undo group: a new group begins
// when the edit kind changes or on an explicit boundary (space, newline, paste…).
static void undoCheckpoint(UndoHistory& h, const TextEditState& e,
                           EditKind k, EditKind& last) {
    if (k != last || k == EditKind::Boundary) h.push({e.text, e.cursor, e.anchor});
    last = k;
}

// Typing a boundary right after a text-replacement shortcut swaps the shortcut
// for its phrase, before the boundary goes in. Called before the keystroke's
// own checkpoint, so the first undo drops the boundary and the second restores
// the shortcut. Callers skip password fields.
// A Unicode codepoint as UTF-8 (key events carry the layout's character).
static std::string utf8Of(uint32_t cp) {
    std::string s;
    if (cp < 0x80) s += static_cast<char>(cp);
    else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

// Focused text fields activate the compositor IME; locale 0 means "the system
// default input method" (Settings → Input Method). Password fields never get
// one (they deactivate instead).
static void imeDeactivate() noexcept {
    auto* bus = XApplication::getBus();
    if (!bus) return;
    bus->Call("com.freebsdx.IME", static_cast<uint32_t>(shared::IMEOpcode::Deactivate), {},
              [](const shared::XBusMessage&) {});
}
static void imeActivate(uint32_t locale, bool password) noexcept {
    auto* bus = XApplication::getBus();
    if (!bus) return;
    if (password) { imeDeactivate(); return; }
    shared::IMEActivateRequest req{};
    req.locale_ = locale;
    bus->Call("com.freebsdx.IME", static_cast<uint32_t>(shared::IMEOpcode::Activate),
              {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, [](const shared::XBusMessage&) {});
}

static void expandReplacement(TextEditState& e, UndoHistory& h, EditKind& last, char boundary) {
    if (!shared::textreplace::isBoundary(boundary) || e.hasSelection()) return;
    std::size_t begin = 0;
    const std::string* phrase = shared::textreplace::match(textReplacements(), e.text, e.cursor, begin);
    if (!phrase) return;
    undoCheckpoint(h, e, EditKind::Boundary, last);
    e.anchor = begin;          // select the shortcut, then replace it
    e.insert(*phrase);
}
static bool applyUndo(UndoHistory& h, TextEditState& e) {
    EditSnapshot cur{e.text, e.cursor, e.anchor}, out;
    if (!h.undo(cur, out)) return false;
    e.text = out.text; e.cursor = out.cursor; e.anchor = out.anchor; return true;
}
static bool applyRedo(UndoHistory& h, TextEditState& e) {
    EditSnapshot cur{e.text, e.cursor, e.anchor}, out;
    if (!h.redo(cur, out)) return false;
    e.text = out.text; e.cursor = out.cursor; e.anchor = out.anchor; return true;
}
// A press-drag selects by character (single click), word (double), or line (triple).
enum class DragMode { Char, Word, Line };

// Ctrl+Z (undo) / Ctrl+Y or Ctrl+Shift+Z (redo).
static bool isUndoKey(bool ctrl, bool shift, int kc) { return ctrl && !shift && kc == 44; }
static bool isRedoKey(bool ctrl, bool shift, int kc) {
    return (ctrl && kc == 21) || (ctrl && shift && kc == 44);
}

// Paints a single line of editable text: selection highlight, the text (split
// around the cursor with any IME preedit inserted), and a caret bar. All x
// offsets come from measuring substrings, so the caret sits at the real insertion
// point rather than always at the end.
class RenderTextField : public RenderNode {
public:
    explicit RenderTextField(float fontSize) noexcept : fontSize_(fontSize) {}
    void setDisplay(const std::string& d) noexcept { display_ = d; }
    void setPreedit(const std::string& p) noexcept { preedit_ = p; }
    void setCursorIndex(std::size_t c) noexcept { cursor_ = c; }
    void setSelection(std::size_t b, std::size_t e) noexcept { sel_b_ = b; sel_e_ = e; }
    void setFocused(bool f) noexcept { focused_ = f; }
    void setColors(Color text, Color caret, Color sel) noexcept {
        text_c_ = text; caret_c_ = caret; sel_c_ = sel;
    }
    [[nodiscard]] Size performLayout(const Constraints& c) noexcept override {
        return c.clamp({measure(display_) + measure(preedit_) + 2.0f, fontSize_});
    }
    void paintSelf(fbx::xrender::DisplayList& dl, Offset o) noexcept override {
        std::size_t cur = std::min(cursor_, display_.size());
        if (sel_e_ > sel_b_ && sel_e_ <= display_.size()) {
            float x0 = measure(display_.substr(0, sel_b_));
            float x1 = measure(display_.substr(0, sel_e_));
            rect(dl, o.x_ + x0, o.y_, x1 - x0, fontSize_, sel_c_);
        }
        std::string before = display_.substr(0, cur);
        std::string after  = display_.substr(cur);
        float x = o.x_;
        glyphs(dl, x, o.y_, before, text_c_);
        x += measure(before);
        if (!preedit_.empty()) { glyphs(dl, x, o.y_, preedit_, caret_c_); x += measure(preedit_); }
        if (focused_) rect(dl, x, o.y_, 1.5f, fontSize_, caret_c_);
        glyphs(dl, x, o.y_, after, text_c_);
    }
private:
    [[nodiscard]] float measure(const std::string& s) const noexcept {
        return s.empty() ? 0.0f : fbx::xrender::FontManager::measureText(s, fontSize_);
    }
    static void rect(fbx::xrender::DisplayList& dl, float x, float y, float w, float h, Color c) {
        fbx::shared::RectData r{};
        r.x_ = x; r.y_ = y; r.width_ = w; r.height_ = h;
        r.r_ = c.r; r.g_ = c.g; r.b_ = c.b; r.a_ = c.a;
        dl.addCommand(fbx::shared::CommandType::DrawRect,
                      {reinterpret_cast<const uint8_t*>(&r), sizeof(r)});
    }
    void glyphs(fbx::xrender::DisplayList& dl, float x, float y, const std::string& s, Color c) {
        if (s.empty()) return;
        fbx::shared::TextData td{};
        td.x_ = x; td.y_ = y; td.font_size_ = fontSize_;
        td.r_ = c.r; td.g_ = c.g; td.b_ = c.b; td.a_ = c.a;
        td.string_length_ = static_cast<uint32_t>(s.size());
        std::vector<uint8_t> p(sizeof(td) + s.size());
        std::memcpy(p.data(), &td, sizeof(td));
        std::memcpy(p.data() + sizeof(td), s.data(), s.size());
        dl.addCommand(fbx::shared::CommandType::DrawText, p);
    }
    std::string display_, preedit_;
    std::size_t cursor_{0}, sel_b_{0}, sel_e_{0};
    bool  focused_{false};
    float fontSize_;
    Color text_c_{1, 1, 1, 1}, caret_c_{1, 1, 1, 1}, sel_c_{0.3f, 0.5f, 0.9f, 0.5f};
};

class TextInputElement : public Element {
public:
    explicit TextInputElement(std::shared_ptr<TextInputWidget> widget) {
        widget_ = std::static_pointer_cast<Widget>(widget);
        edit_.text = widget->text();
        edit_.cursor = edit_.anchor = edit_.text.size();   // start at the end
        renderNode_ = std::make_shared<RenderFlexRow>();
        field_node_ = std::make_shared<RenderTextField>(widget->fontSize());
        renderNode_->addChild(field_node_);
        renderNode_->setCursor(CursorShape::Text);
        // Vertically centre the text within the field's box and give it a small
        // inset, so a fixed-height input (e.g. a 40px login field) doesn't jam
        // its text against the top-left corner.
        std::static_pointer_cast<RenderFlexRow>(renderNode_)
            ->setCrossAxisAlignment(CrossAxisAlignment::Center);
        std::static_pointer_cast<RenderFlexRow>(renderNode_)->setPadding(10.0f);
        apply_widget_props(renderNode_, widget_);
        refreshField(/*force=*/true);
    }
    void syncState() noexcept override {
        Element::syncState();
        // Keep the caret/selection in step with focus changes even when no
        // keystroke redrew this frame. Change-detected (sig_) so a steady field
        // doesn't re-emit and re-damage every frame (that caused flicker).
        refreshField();
    }
    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        // Let the widget move focus here programmatically, and honour autofocus.
        auto tw = std::static_pointer_cast<TextInputWidget>(widget_);
        std::weak_ptr<Element> weak = shared_from_this();
        tw->setFocusHook([weak]() {
            if (auto e = weak.lock()) XApplication::requestFocus(e);
        });
        if (tw->autofocus()) XApplication::requestFocus(shared_from_this());
    }
    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        auto text_widget = std::static_pointer_cast<TextInputWidget>(widget_);
        // Adopt an externally-set value (e.g. a URL bar rewritten by code) but keep
        // the caret valid.
        if (text_widget->text() != edit_.text) {
            edit_.text = text_widget->text();
            edit_.cursor = edit_.anchor = edit_.text.size();
        }
        std::weak_ptr<Element> weak = shared_from_this();
        text_widget->setFocusHook([weak]() {
            if (auto e = weak.lock()) XApplication::requestFocus(e);
        });
        apply_widget_props(renderNode_, widget_);
        refreshField(/*force=*/true);
    }
    void unmount() noexcept override {
        sendImeDeactivate();
        renderNode_->clearChildren();
        parent_.reset();
    }
    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        if (event.type_ == shared::InputEventType::MouseButton && event.char_value_ != 0) {
            XApplication::requestFocus(shared_from_this());
            sendImeActivate();
            bool shift = (event.modifiers_ & 1);
            std::size_t idx = indexAtX(static_cast<float>(event.value_a_));
            int clicks = countClicks(idx);
            if (clicks == 2) {          // double-click → select word; drag by word
                auto r = edit_.wordBounds(idx);
                edit_.anchor = r.begin; edit_.cursor = r.end;
                sel_lo_ = r.begin; sel_hi_ = r.end; drag_mode_ = DragMode::Word;
            } else if (clicks >= 3) {   // triple-click → select line; drag by line
                std::size_t a = edit_.lineStart(idx), b = edit_.lineEnd(idx);
                edit_.anchor = a; edit_.cursor = b;
                sel_lo_ = a; sel_hi_ = b; drag_mode_ = DragMode::Line;
            } else {                    // single click → caret; drag by character
                edit_.setCursor(idx, shift); drag_mode_ = DragMode::Char;
            }
            XApplication::capturePointer(shared_from_this());   // capture for any drag
            last_kind_ = EditKind::Boundary;   // a click ends the current undo group
            refreshField();
            return true;
        } else if (event.type_ == shared::InputEventType::MouseMove) {
            // Only while we own the capture (a drag we started) — otherwise this is
            // a hover and should fall through to cursor-shape handling.
            if (XApplication::getPointerCapture().get() == this) {
                std::size_t idx = indexAtX(static_cast<float>(event.value_a_));
                if (drag_mode_ == DragMode::Char) edit_.setCursor(idx, /*extend=*/true);
                else                              extendByUnit(idx);
                refreshField();
                return true;
            }
            return Element::handleEvent(event);
        } else if (event.type_ == shared::InputEventType::ImePreedit) {
            uint32_t len = std::min(event.modifiers_, 8u);
            std::string preedit(len, '\0');
            if (len > 0) std::memcpy(preedit.data(), &event.value_a_, std::min(len, 4u));
            if (len > 4) std::memcpy(preedit.data() + 4, &event.value_b_, len - 4u);
            preedit_ = preedit;
            refreshField();
            return true;
        } else if (event.type_ == shared::InputEventType::ImeCommit) {
            preedit_.clear();
            uint32_t len = std::min(event.modifiers_, 8u);
            if (len > 0) {
                std::string committed(len, '\0');
                std::memcpy(committed.data(), &event.value_a_, std::min(len, 4u));
                if (len > 4) std::memcpy(committed.data() + 4, &event.value_b_, len - 4u);
                undoCheckpoint(history_, edit_, EditKind::Boundary, last_kind_);
                edit_.insert(committed);
                commit();
            }
            return true;
        } else if (event.type_ == shared::InputEventType::KeyPress && event.value_b_ != 0) {
            const bool ctrl  = (event.modifiers_ & 2);
            const bool shift = (event.modifiers_ & 1);
            const int  kc    = event.value_a_;   // Linux evdev keycode
            if (isUndoKey(ctrl, shift, kc)) {
                if (applyUndo(history_, edit_)) commit();
                last_kind_ = EditKind::Boundary; return true;
            } else if (isRedoKey(ctrl, shift, kc)) {
                if (applyRedo(history_, edit_)) commit();
                last_kind_ = EditKind::Boundary; return true;
            } else if (ctrl && kc == 46) {       // Ctrl+C — copy selection (or all)
                if (auto bus = XApplication::getBus())
                    Clipboard::setString(*bus, edit_.hasSelection() ? edit_.selectedText() : edit_.text);
                return true;
            } else if (ctrl && kc == 45) {       // Ctrl+X — cut selection
                if (edit_.hasSelection()) {
                    if (auto bus = XApplication::getBus()) Clipboard::setString(*bus, edit_.selectedText());
                    undoCheckpoint(history_, edit_, EditKind::Boundary, last_kind_);
                    edit_.deleteSelection();
                    commit();
                }
                return true;
            } else if (ctrl && kc == 47) {       // Ctrl+V — paste (replaces selection)
                undoCheckpoint(history_, edit_, EditKind::Boundary, last_kind_);
                if (auto bus = XApplication::getBus())
                    Clipboard::getString(*bus, [this](const std::string& pasted) {
                        edit_.insert(pasted); commit();
                    });
                return true;
            } else if (ctrl && kc == 30) {       // Ctrl+A — select all
                edit_.selectAll(); last_kind_ = EditKind::Boundary; refreshField(); return true;
            } else if (kc == 28 || kc == 96) {   // Return / KP-Enter — submit
                auto tw = std::static_pointer_cast<TextInputWidget>(widget_);
                if (const auto& cb = tw->onSubmit()) cb(edit_.text);
                return true;
            } else if (kc == 105) { edit_.moveLeft(shift);  last_kind_ = EditKind::Boundary; refreshField(); return true; }  // ←
            else if   (kc == 106) { edit_.moveRight(shift); last_kind_ = EditKind::Boundary; refreshField(); return true; }  // →
            else if   (kc == 102) { edit_.moveHome(shift);  last_kind_ = EditKind::Boundary; refreshField(); return true; }  // Home
            else if   (kc == 107) { edit_.moveEnd(shift);   last_kind_ = EditKind::Boundary; refreshField(); return true; }  // End
            else if   (kc == 14)  { undoCheckpoint(history_, edit_, EditKind::Delete, last_kind_);
                                    edit_.backspace();      commit();       return true; }  // Backspace
            else if   (kc == 111) { undoCheckpoint(history_, edit_, EditKind::Delete, last_kind_);
                                    edit_.deleteForward();  commit();       return true; }  // Delete
            else if   (event.char_value_ != 0) {                                            // Printable
                // char_value_ is a Unicode codepoint (the keyboard layout's
                // character): insert it as UTF-8, not as one truncated byte.
                const uint32_t cp = static_cast<uint32_t>(event.char_value_);
                const char ch = cp < 0x80 ? static_cast<char>(cp) : '\0';
                EditKind k = (ch == ' ' || ch == '\t') ? EditKind::Boundary : EditKind::Insert;
                if (auto tw = std::static_pointer_cast<TextInputWidget>(widget_); !tw || !tw->passwordMode())
                    expandReplacement(edit_, history_, last_kind_, ch);
                undoCheckpoint(history_, edit_, k, last_kind_);
                edit_.insert(utf8Of(cp));
                commit();
                return true;
            }
            return true;
        }
        return Element::handleEvent(event);
    }

    void onThemeChanged(const Theme& theme) noexcept override {
        text_color_        = theme.text;
        caret_color_       = theme.text;
        selection_color_   = Color{theme.accent.r, theme.accent.g, theme.accent.b, 0.4f};
        placeholder_color_ = Color{theme.text.r, theme.text.g, theme.text.b, 0.4f};
        if (!widget_->hasBackground()) renderNode_->setBackgroundColor(theme.surface);
        refreshField(/*force=*/true);
    }

    [[nodiscard]] SemanticNode getSemanticTree() const noexcept override {
        auto abs = renderNode_->getAbsoluteOffset();
        auto sz  = renderNode_->getSize();
        std::string lbl = renderNode_->accessibilityLabel().empty()
                              ? "Text field"
                              : renderNode_->accessibilityLabel();
        bool is_focused = (XApplication::getFocusedElement().get() == this);
        return SemanticNode{
            .id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF),
            .role = "TextInput", .label = lbl, .value = edit_.text,
            .focused = is_focused,
            .x = abs.x_, .y = abs.y_, .width = sz.width_, .height = sz.height_
        };
    }

private:
    void sendImeActivate() noexcept {
        auto tw = std::static_pointer_cast<TextInputWidget>(widget_);
        imeActivate(tw ? tw->locale() : 0, tw && tw->passwordMode());
    }
    void sendImeDeactivate() noexcept { imeDeactivate(); }

    // The rendered form of the buffer (masked per byte in password mode).
    [[nodiscard]] std::string displayString() const {
        auto tw = std::static_pointer_cast<TextInputWidget>(widget_);
        return (tw && tw->passwordMode()) ? std::string(edit_.text.size(), '*') : edit_.text;
    }
    // Map a window-space x to a caret byte index (the text node is persistent, so
    // its offset is always valid).
    [[nodiscard]] std::size_t indexAtX(float win_x) const {
        return indexFromX(displayString(), win_x - field_node_->getAbsoluteOffset().x_,
                          widgetFontSize());
    }
    // Returns the consecutive-click count (1/2/3+) for a press at byte index `idx`:
    // increments while presses land at the same spot within the double-click window,
    // else resets to 1. Wraps back to 1 after a triple so a 4th click starts over.
    int countClicks(std::size_t idx) noexcept {
        auto now = std::chrono::steady_clock::now();
        bool quick = (now - last_click_) < std::chrono::milliseconds(400);
        bool same  = (idx == last_click_idx_);
        click_count_ = (quick && same && click_count_ < 3) ? click_count_ + 1 : 1;
        last_click_ = now; last_click_idx_ = idx;
        return click_count_;
    }
    // Word/line drag: grow the selection from the initially-clicked unit
    // (sel_lo_..sel_hi_) to the unit under the pointer, snapping to unit bounds.
    void extendByUnit(std::size_t idx) noexcept {
        std::size_t b, e;
        if (drag_mode_ == DragMode::Word) { auto r = edit_.wordBounds(idx); b = r.begin; e = r.end; }
        else { b = edit_.lineStart(idx); e = edit_.lineEnd(idx); }
        if (b >= sel_hi_)      { edit_.anchor = sel_lo_; edit_.cursor = e; }   // forward
        else if (e <= sel_lo_) { edit_.anchor = sel_hi_; edit_.cursor = b; }   // backward
        else                   { edit_.anchor = sel_lo_; edit_.cursor = sel_hi_; }
    }
    [[nodiscard]] float widgetFontSize() const {
        auto tw = std::static_pointer_cast<TextInputWidget>(widget_);
        return tw ? tw->fontSize() : 16.0f;
    }
    // Push the edited buffer to the widget (so text() reflects what was typed),
    // refresh the visuals, and notify the change observer.
    void commit() noexcept {
        if (auto tw = std::static_pointer_cast<TextInputWidget>(widget_)) {
            tw->setText(edit_.text);
            refreshField();
            if (const auto& cb = tw->onChange()) cb(edit_.text);
        }
    }
    // Recompute what the field node should draw. Change-detected via sig_ so a
    // steady field emits no per-frame change (avoids repaint flicker).
    void refreshField(bool force = false) noexcept {
        auto tw = std::static_pointer_cast<TextInputWidget>(widget_);
        bool focused  = (XApplication::getFocusedElement().get() == this);
        bool password = tw && tw->passwordMode();
        bool empty    = edit_.text.empty() && preedit_.empty();
        std::string disp;
        bool ph = false;
        std::size_t cur = edit_.cursor, sb = edit_.selBegin(), se = edit_.selEnd();
        if (empty && !focused && tw && !tw->placeholder().empty()) {
            disp = tw->placeholder(); ph = true; cur = 0; sb = se = 0;
        } else {
            disp = displayString();
        }
        (void)password;
        std::string sig = disp + '\x01' + std::to_string(cur) + ':' + std::to_string(sb) + ':'
                        + std::to_string(se) + (focused ? "F" : "f") + (ph ? "P" : "p") + preedit_;
        if (!force && sig == sig_) return;
        sig_ = sig;
        field_node_->setDisplay(disp);
        field_node_->setPreedit(ph ? std::string{} : preedit_);
        field_node_->setCursorIndex(cur);
        field_node_->setSelection(sb, se);
        field_node_->setFocused(focused && !ph);
        field_node_->setColors(ph ? placeholder_color_ : text_color_, caret_color_, selection_color_);
    }

    TextEditState edit_;
    std::string   preedit_;   // in-progress IME composition
    std::string   sig_;       // last-rendered signature (change-detection)
    UndoHistory   history_;
    EditKind      last_kind_{EditKind::Boundary};
    // Multi-click (double/triple) detection + press-drag unit selection.
    std::chrono::steady_clock::time_point last_click_{};
    std::size_t   last_click_idx_{static_cast<std::size_t>(-1)};
    int           click_count_{0};
    DragMode      drag_mode_{DragMode::Char};
    std::size_t   sel_lo_{0}, sel_hi_{0};   // initially-clicked unit bounds (word/line drag)
    std::shared_ptr<RenderTextField> field_node_;
    Color text_color_{1, 1, 1, 1};
    Color caret_color_{1, 1, 1, 1};
    Color selection_color_{0.3f, 0.5f, 0.9f, 0.4f};
    Color placeholder_color_{0.6f, 0.6f, 0.6f, 1.0f};
};
std::shared_ptr<Element> TextInputWidget::createElement() noexcept { return std::make_shared<TextInputElement>(std::static_pointer_cast<TextInputWidget>(shared_from_this())); }

// ── Multi-line text field ───────────────────────────────────────────────────────
// Content is a RenderFlexColumn of one RenderTextField per line (the text engine
// shapes a single run and does not break on '\n', so lines are split here). Shares
// the single-line cursor/selection model (TextEditState): the caret and any
// selection are rendered on the correct line at the correct column. Return inserts
// a newline; Ctrl+Return submits.
class MultilineTextInputElement : public Element {
public:
    explicit MultilineTextInputElement(std::shared_ptr<MultilineTextInputWidget> widget) {
        widget_ = std::static_pointer_cast<Widget>(widget);
        edit_.text = widget->text();
        edit_.cursor = edit_.anchor = edit_.text.size();
        fs_   = widget->fontSize();
        renderNode_ = std::make_shared<RenderFlexColumn>();
        auto col = std::static_pointer_cast<RenderFlexColumn>(renderNode_);
        col->setCrossAxisAlignment(CrossAxisAlignment::Start);
        col->setPadding(10.0f);
        col->setGap(2.0f);
        renderNode_->setCursor(CursorShape::Text);
        apply_widget_props(renderNode_, widget_);
        refresh(/*force=*/true);
    }
    void syncState() noexcept override {
        Element::syncState();
        refresh();   // change-detected: keeps the caret in step with focus
    }
    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        auto w = std::static_pointer_cast<MultilineTextInputWidget>(widget_);
        std::weak_ptr<Element> weak = shared_from_this();
        w->setFocusHook([weak]() { if (auto e = weak.lock()) XApplication::requestFocus(e); });
        if (w->autofocus()) XApplication::requestFocus(shared_from_this());
    }
    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        auto w = std::static_pointer_cast<MultilineTextInputWidget>(widget_);
        if (w->text() != edit_.text) {
            edit_.text = w->text();
            edit_.cursor = edit_.anchor = edit_.text.size();
        }
        std::weak_ptr<Element> weak = shared_from_this();
        w->setFocusHook([weak]() { if (auto e = weak.lock()) XApplication::requestFocus(e); });
        apply_widget_props(renderNode_, widget_);
        refresh(/*force=*/true);
    }
    void unmount() noexcept override { imeDeactivate(); renderNode_->clearChildren(); parent_.reset(); }

    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        if (event.type_ == shared::InputEventType::MouseButton && event.char_value_ != 0) {
            XApplication::requestFocus(shared_from_this());
            imeActivate(0, false);   // the system default input method
            goal_valid_ = false;   // a click sets a new horizontal goal
            last_kind_ = EditKind::Boundary;   // and ends the current undo group
            // Snapshot the line geometry now (nodes still carry valid offsets); a
            // press-drag reuses the snapshot so it survives the rebuilds in refresh().
            snapshotLineGeometry();
            std::size_t idx = indexFromClick(static_cast<float>(event.value_a_),
                                             static_cast<float>(event.value_b_));
            int clicks = countClicks(idx);
            if (clicks == 2) {          // double-click → select word; drag by word
                auto r = edit_.wordBounds(idx);
                edit_.anchor = r.begin; edit_.cursor = r.end;
                sel_lo_ = r.begin; sel_hi_ = r.end; drag_mode_ = DragMode::Word;
            } else if (clicks >= 3) {   // triple-click → select line; drag by line
                std::size_t a = edit_.lineStart(idx), b = edit_.lineEnd(idx);
                edit_.anchor = a; edit_.cursor = b;
                sel_lo_ = a; sel_hi_ = b; drag_mode_ = DragMode::Line;
            } else {                    // single click → caret; drag by character
                edit_.setCursor(idx, (event.modifiers_ & 1)); drag_mode_ = DragMode::Char;
            }
            XApplication::capturePointer(shared_from_this());   // capture for any drag
            refresh();
            return true;
        } else if (event.type_ == shared::InputEventType::MouseMove) {
            if (XApplication::getPointerCapture().get() == this) {
                goal_valid_ = false;
                last_kind_ = EditKind::Boundary;
                std::size_t idx = indexFromClick(static_cast<float>(event.value_a_),
                                                 static_cast<float>(event.value_b_));
                if (drag_mode_ == DragMode::Char) edit_.setCursor(idx, /*extend=*/true);
                else                              extendByUnit(idx);
                refresh();
                return true;
            }
            return Element::handleEvent(event);
        } else if (event.type_ == shared::InputEventType::ImePreedit) {
            uint32_t len = std::min(event.modifiers_, 8u);
            std::string preedit(len, '\0');
            if (len > 0) std::memcpy(preedit.data(), &event.value_a_, std::min(len, 4u));
            if (len > 4) std::memcpy(preedit.data() + 4, &event.value_b_, len - 4u);
            preedit_ = preedit;
            refresh();
            return true;
        } else if (event.type_ == shared::InputEventType::ImeCommit) {
            preedit_.clear();
            uint32_t len = std::min(event.modifiers_, 8u);
            if (len > 0) {
                std::string committed(len, '\0');
                std::memcpy(committed.data(), &event.value_a_, std::min(len, 4u));
                if (len > 4) std::memcpy(committed.data() + 4, &event.value_b_, len - 4u);
                undoCheckpoint(history_, edit_, EditKind::Boundary, last_kind_);
                edit_.insert(committed);
                commitEdit();
            }
            return true;
        } else if (event.type_ == shared::InputEventType::KeyPress && event.value_b_ != 0) {
            const bool ctrl  = (event.modifiers_ & 2);
            const bool shift = (event.modifiers_ & 1);
            const int  kc    = event.value_a_;   // Linux evdev keycode
            if (kc != 103 && kc != 108) goal_valid_ = false;  // any non-vertical key drops the goal x
            if (isUndoKey(ctrl, shift, kc)) {
                if (applyUndo(history_, edit_)) commitEdit();
                last_kind_ = EditKind::Boundary; return true;
            } else if (isRedoKey(ctrl, shift, kc)) {
                if (applyRedo(history_, edit_)) commitEdit();
                last_kind_ = EditKind::Boundary; return true;
            } else if (ctrl && kc == 46) {       // Ctrl+C
                if (auto bus = XApplication::getBus())
                    Clipboard::setString(*bus, edit_.hasSelection() ? edit_.selectedText() : edit_.text);
                return true;
            } else if (ctrl && kc == 45) {       // Ctrl+X
                if (edit_.hasSelection()) {
                    if (auto bus = XApplication::getBus()) Clipboard::setString(*bus, edit_.selectedText());
                    undoCheckpoint(history_, edit_, EditKind::Boundary, last_kind_);
                    edit_.deleteSelection();
                    commitEdit();
                }
                return true;
            } else if (ctrl && kc == 47) {       // Ctrl+V
                undoCheckpoint(history_, edit_, EditKind::Boundary, last_kind_);
                if (auto bus = XApplication::getBus())
                    Clipboard::getString(*bus, [this](const std::string& pasted) {
                        edit_.insert(pasted); commitEdit();
                    });
                return true;
            } else if (ctrl && kc == 30) {       // Ctrl+A
                edit_.selectAll(); last_kind_ = EditKind::Boundary; refresh(); return true;
            } else if (kc == 28 || kc == 96) {   // Return / KP-Enter
                if (ctrl) {   // Ctrl+Return submits; plain Return inserts a newline
                    auto w = std::static_pointer_cast<MultilineTextInputWidget>(widget_);
                    if (const auto& cb = w->onSubmit()) cb(edit_.text);
                } else {
                    undoCheckpoint(history_, edit_, EditKind::Boundary, last_kind_);
                    edit_.insert('\n');
                    commitEdit();
                }
                return true;
            } else if (kc == 105) { edit_.moveLeft(shift);  last_kind_ = EditKind::Boundary; refresh(); return true; }  // ←
            else if   (kc == 106) { edit_.moveRight(shift); last_kind_ = EditKind::Boundary; refresh(); return true; }  // →
            else if   (kc == 103) { visualMove(-1, shift);  last_kind_ = EditKind::Boundary; refresh(); return true; }  // ↑
            else if   (kc == 108) { visualMove(+1, shift);  last_kind_ = EditKind::Boundary; refresh(); return true; }  // ↓
            else if   (kc == 102) { visualHome(shift);      last_kind_ = EditKind::Boundary; refresh(); return true; }  // Home
            else if   (kc == 107) { visualEnd(shift);       last_kind_ = EditKind::Boundary; refresh(); return true; }  // End
            else if   (kc == 14)  { undoCheckpoint(history_, edit_, EditKind::Delete, last_kind_);
                                    edit_.backspace();      commitEdit(); return true; }  // Backspace
            else if   (kc == 111) { undoCheckpoint(history_, edit_, EditKind::Delete, last_kind_);
                                    edit_.deleteForward();  commitEdit(); return true; }  // Delete
            else if   (event.char_value_ != 0) {                                          // Printable
                const uint32_t cp = static_cast<uint32_t>(event.char_value_);
                const char ch = cp < 0x80 ? static_cast<char>(cp) : '\0';
                EditKind k = (ch == ' ' || ch == '\t') ? EditKind::Boundary : EditKind::Insert;
                expandReplacement(edit_, history_, last_kind_, ch);
                undoCheckpoint(history_, edit_, k, last_kind_);
                edit_.insert(utf8Of(cp));
                commitEdit();
                return true;
            }
            return true;
        }
        return Element::handleEvent(event);
    }

    void onThemeChanged(const Theme& theme) noexcept override {
        text_color_        = theme.text;
        caret_color_       = theme.text;
        selection_color_   = Color{theme.accent.r, theme.accent.g, theme.accent.b, 0.4f};
        placeholder_color_ = Color{theme.text.r, theme.text.g, theme.text.b, 0.4f};
        if (!widget_->hasBackground()) renderNode_->setBackgroundColor(theme.surface);
        refresh(/*force=*/true);
    }

    [[nodiscard]] SemanticNode getSemanticTree() const noexcept override {
        auto abs = renderNode_->getAbsoluteOffset();
        auto sz  = renderNode_->getSize();
        bool is_focused = (XApplication::getFocusedElement().get() == this);
        return SemanticNode{
            .id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF),
            .role = "TextArea", .label = "Text area", .value = edit_.text,
            .focused = is_focused,
            .x = abs.x_, .y = abs.y_, .width = sz.width_, .height = sz.height_
        };
    }

private:
    void commitEdit() noexcept {
        goal_valid_ = false;   // a text edit moves the caret horizontally
        if (auto w = std::static_pointer_cast<MultilineTextInputWidget>(widget_)) {
            w->setText(edit_.text);
            refresh();
            if (const auto& cb = w->onChange()) cb(edit_.text);
        }
    }
    // Freeze the current line nodes' painted geometry (window x/y + byte range) so
    // a drag can hit-test against it even as refresh() rebuilds the nodes.
    void snapshotLineGeometry() noexcept {
        drag_geom_.clear();
        for (std::size_t i = 0; i < lines_.size() && i < segments_.size(); ++i) {
            auto off = lines_[i].first->getAbsoluteOffset();
            drag_geom_.push_back({off.x_, off.y_, segments_[i].start, segments_[i].end});
        }
    }
    // Byte index at (cx,cy) using the geometry snapshot: pick the line whose y-band
    // contains cy (clamped), then map cx within it.
    [[nodiscard]] std::size_t indexFromClick(float cx, float cy) const noexcept {
        if (drag_geom_.empty()) return 0;
        std::size_t best = 0;
        for (std::size_t i = 0; i < drag_geom_.size(); ++i)
            if (cy >= drag_geom_[i].y) best = i;
        const auto& g = drag_geom_[best];
        std::string lineText = edit_.text.substr(g.start, g.end - g.start);
        return g.start + indexFromX(lineText, cx - g.x, fs_);
    }
    void mapCaret(float cx, float cy, bool extend) noexcept {
        edit_.setCursor(indexFromClick(cx, cy), extend);
    }
    int countClicks(std::size_t idx) noexcept {
        auto now = std::chrono::steady_clock::now();
        bool quick = (now - last_click_) < std::chrono::milliseconds(400);
        bool same  = (idx == last_click_idx_);
        click_count_ = (quick && same && click_count_ < 3) ? click_count_ + 1 : 1;
        last_click_ = now; last_click_idx_ = idx;
        return click_count_;
    }
    // Word/line drag: grow the selection from the initially-clicked unit to the unit
    // under the pointer, snapping to unit bounds.
    void extendByUnit(std::size_t idx) noexcept {
        std::size_t b, e;
        if (drag_mode_ == DragMode::Word) { auto r = edit_.wordBounds(idx); b = r.begin; e = r.end; }
        else { b = edit_.lineStart(idx); e = edit_.lineEnd(idx); }
        if (b >= sel_hi_)      { edit_.anchor = sel_lo_; edit_.cursor = e; }
        else if (e <= sel_lo_) { edit_.anchor = sel_hi_; edit_.cursor = b; }
        else                   { edit_.anchor = sel_lo_; edit_.cursor = sel_hi_; }
    }
    // Rebuild the per-line RenderTextField nodes when anything visible changed,
    // placing the caret/selection on the right line. Change-detected (sig_) so a
    // steady field emits no per-frame damage (avoids flicker).
    void refresh(bool force = false) noexcept {
        bool focused = (XApplication::getFocusedElement().get() == this);
        bool empty   = edit_.text.empty() && preedit_.empty();
        std::size_t c = edit_.cursor, sb = edit_.selBegin(), se = edit_.selEnd();
        // Content width also affects the wrap, so it's part of the signature — a
        // resize re-wraps even when the text/caret are unchanged.
        int wrap_w = static_cast<int>(renderNode_->getSize().width_);

        std::string sig = (empty && !focused ? "P" : "p")
                        + edit_.text + '\x01' + preedit_ + '\x01'
                        + std::to_string(c) + ':' + std::to_string(sb) + ':' + std::to_string(se)
                        + (focused ? "F" : "f") + '\x01' + std::to_string(wrap_w);
        if (!force && sig == sig_) return;
        sig_ = sig;

        renderNode_->clearChildren();
        lines_.clear();
        segments_.clear();

        if (empty && !focused) {
            auto w = std::static_pointer_cast<MultilineTextInputWidget>(widget_);
            std::string ph = w ? w->placeholder() : std::string{};
            auto tf = std::make_shared<RenderTextField>(fs_);
            tf->setDisplay(ph);
            tf->setColors(placeholder_color_, caret_color_, selection_color_);
            renderNode_->addChild(tf);
            lines_.emplace_back(tf, 0);
            segments_.push_back({0, 0, false});
            return;
        }

        const std::string& t = edit_.text;
        // Soft-wrap to the field's laid-out content width (from the previous frame;
        // one-frame lag on resize is fine). <=0 (not yet laid out) disables wrapping.
        float avail = renderNode_->getSize().width_ - 20.0f;   // padding 10 each side
        float fs = fs_;
        segments_ = wrapText(t, avail, [fs](const std::string& s) {
            return fbx::xrender::FontManager::measureText(s, fs);
        });
        for (std::size_t i = 0; i < segments_.size(); ++i) {
            const WrapSegment& sg = segments_[i];
            auto tf = std::make_shared<RenderTextField>(fs_);
            tf->setDisplay(t.substr(sg.start, sg.end - sg.start));
            bool caret_here = focused && caretOnSegment(c, i);
            tf->setFocused(caret_here);
            tf->setCursorIndex(caret_here ? (c - sg.start) : 0);
            tf->setPreedit(caret_here ? preedit_ : std::string{});
            std::size_t lo = std::max(sb, sg.start), hi = std::min(se, sg.end);
            if (hi > lo) tf->setSelection(lo - sg.start, hi - sg.start);
            else         tf->setSelection(0, 0);
            tf->setColors(text_color_, caret_color_, selection_color_);
            renderNode_->addChild(tf);
            lines_.emplace_back(tf, sg.start);
        }
    }

    // Which visual segment the caret at byte c belongs to. At a segment's end the
    // caret stays on that segment only if it is newline-terminated or the last
    // segment; a soft-wrap boundary hands it to the next segment (column 0).
    [[nodiscard]] bool caretOnSegment(std::size_t c, std::size_t i) const noexcept {
        const WrapSegment& sg = segments_[i];
        if (c >= sg.start && c < sg.end) return true;
        if (c == sg.end) return sg.newline || (i + 1 == segments_.size());
        return false;
    }
    [[nodiscard]] std::size_t caretSegmentIndex(std::size_t c) const noexcept {
        for (std::size_t i = 0; i < segments_.size(); ++i)
            if (caretOnSegment(c, i)) return i;
        return segments_.empty() ? 0 : segments_.size() - 1;
    }
    // The caret's x within its visual line (px from the line's left edge).
    [[nodiscard]] float caretX() const noexcept {
        std::size_t c = edit_.cursor;
        const WrapSegment& s = segments_[caretSegmentIndex(c)];
        return fbx::xrender::FontManager::measureText(
            edit_.text.substr(s.start, c - s.start), fs_);
    }
    // Move the caret one visual line up/down, preserving the horizontal *pixel*
    // position. A sticky goal_x_ survives a run of Up/Down so passing through a
    // short line and back returns to the original column.
    void visualMove(int dir, bool extend) noexcept {
        if (segments_.empty()) return;
        if (!goal_valid_) { goal_x_ = caretX(); goal_valid_ = true; }
        long cur = static_cast<long>(caretSegmentIndex(edit_.cursor));
        long tgt = cur + dir;
        if (tgt < 0) { edit_.setCursor(0, extend); return; }
        if (tgt >= static_cast<long>(segments_.size())) {
            edit_.setCursor(edit_.text.size(), extend); return;
        }
        const WrapSegment& d = segments_[static_cast<std::size_t>(tgt)];
        std::string lineText = edit_.text.substr(d.start, d.end - d.start);
        edit_.setCursor(d.start + indexFromX(lineText, goal_x_, fs_), extend);
    }
    // Home/End act on the current *visual* line (soft-wrap aware).
    void visualHome(bool extend) noexcept {
        if (segments_.empty()) { edit_.moveHome(extend); return; }
        edit_.setCursor(segments_[caretSegmentIndex(edit_.cursor)].start, extend);
    }
    void visualEnd(bool extend) noexcept {
        if (segments_.empty()) { edit_.moveEnd(extend); return; }
        edit_.setCursor(segments_[caretSegmentIndex(edit_.cursor)].end, extend);
    }

    TextEditState edit_;
    std::string   preedit_;
    std::string   sig_;
    float         fs_{16.0f};
    // Visual segments from soft-wrap + their per-line render nodes, rebuilt each
    // refresh (parallel: segments_[i] ↔ lines_[i]).
    std::vector<WrapSegment> segments_;
    std::vector<std::pair<std::shared_ptr<RenderTextField>, std::size_t>> lines_;
    // Frozen line geometry for the in-progress drag (survives node rebuilds).
    struct LineGeom { float x, y; std::size_t start, end; };
    std::vector<LineGeom> drag_geom_;
    // Multi-click (double/triple) detection + press-drag unit selection.
    std::chrono::steady_clock::time_point last_click_{};
    std::size_t   last_click_idx_{static_cast<std::size_t>(-1)};
    int           click_count_{0};
    DragMode      drag_mode_{DragMode::Char};
    std::size_t   sel_lo_{0}, sel_hi_{0};   // initially-clicked unit bounds (word/line drag)
    // Sticky horizontal goal (px) for Up/Down; dropped by any horizontal move.
    bool          goal_valid_{false};
    float         goal_x_{0.0f};
    UndoHistory   history_;
    EditKind      last_kind_{EditKind::Boundary};
    Color         text_color_{1.0f, 1.0f, 1.0f, 1.0f};
    Color         caret_color_{1.0f, 1.0f, 1.0f, 1.0f};
    Color         selection_color_{0.3f, 0.5f, 0.9f, 0.4f};
    Color         placeholder_color_{0.6f, 0.6f, 0.6f, 1.0f};
};
std::shared_ptr<Element> MultilineTextInputWidget::createElement() noexcept { return std::make_shared<MultilineTextInputElement>(std::static_pointer_cast<MultilineTextInputWidget>(shared_from_this())); }

class ImageElement : public Element {
public:
    explicit ImageElement(std::shared_ptr<ImageWidget> widget) {
        widget_ = std::static_pointer_cast<Widget>(widget);
        auto img_widget = std::static_pointer_cast<ImageWidget>(widget_);

        if (img_widget->hasPixels()) {
            // Pixel-data path: caller already decoded/rendered to RGBA buffer.
            float disp_w = img_widget->hasFixedSize()
                ? img_widget->fixedWidth()
                : static_cast<float>(img_widget->pixelWidth());
            float disp_h = img_widget->hasFixedSize()
                ? img_widget->fixedHeight()
                : static_cast<float>(img_widget->pixelHeight());
            renderNode_ = std::make_shared<RenderImage>(
                img_widget->pixels(),           // copy — widget may be replaced
                img_widget->pixelWidth(),
                img_widget->pixelHeight(),
                disp_w, disp_h, img_widget->fitMode());
        } else {
            uint32_t tw = 64, th = 64;
            if (img_widget->hasFixedSize()) {
                tw = static_cast<uint32_t>(img_widget->fixedWidth());
                th = static_cast<uint32_t>(img_widget->fixedHeight());
            }
            auto image_res = fbx::xrender::ImageDecoder::decode(
                resolve_asset_path(img_widget->path()), tw, th);
            if (image_res) {
                auto& img_data = image_res.value();
                // Use the widget's fixed size as display size (or the decoded size if no fixed size).
                float disp_w = img_widget->hasFixedSize()
                    ? img_widget->fixedWidth()
                    : static_cast<float>(img_data.width_);
                float disp_h = img_widget->hasFixedSize()
                    ? img_widget->fixedHeight()
                    : static_cast<float>(img_data.height_);
                renderNode_ = std::make_shared<RenderImage>(
                    std::move(img_data.pixels_), img_data.width_, img_data.height_,
                    disp_w, disp_h, img_widget->fitMode());
            } else {
                renderNode_ = std::make_shared<RenderBox>(static_cast<float>(tw), static_cast<float>(th));
            }
        }
        apply_widget_props(renderNode_, widget_);
    }
    void mount(std::shared_ptr<Element> p) noexcept override { parent_ = p; renderNode_->setOwnerElement(shared_from_this()); }
    void update(std::shared_ptr<Widget> w) noexcept override { widget_ = w; apply_widget_props(renderNode_, widget_); }
    void unmount() noexcept override { parent_.reset(); }
};
std::shared_ptr<Element> ImageWidget::createElement() noexcept { return std::make_shared<ImageElement>(std::static_pointer_cast<ImageWidget>(shared_from_this())); }

// ─── CheckboxElement ──────────────────────────────────────────────────────────

class CheckboxElement : public Element {
public:
    explicit CheckboxElement(std::shared_ptr<CheckboxWidget> widget) {
        widget_ = std::static_pointer_cast<Widget>(widget);
        auto node = std::make_shared<RenderCheckbox>();
        node->setChecked(widget->checked());
        renderNode_ = node;
        apply_widget_props(renderNode_, widget_);
    }
    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
    }
    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        auto w = std::static_pointer_cast<CheckboxWidget>(widget_);
        std::static_pointer_cast<RenderCheckbox>(renderNode_)->setChecked(w->checked());
        apply_widget_props(renderNode_, widget_);
    }
    void unmount() noexcept override { parent_.reset(); }
    void syncState() noexcept override {
        bool disabled = widget_->isDisabled();
        renderNode_->setCursor(disabled ? CursorShape::NotAllowed
                                        : CursorShape::Pointer);
        renderNode_->setOpacity(disabled ? kDisabledOpacity : 1.0f);
        Element::syncState();
    }
    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        if (event.type_ == shared::InputEventType::MouseButton && event.char_value_ != 0) {
            if (widget_->isDisabled()) return true;  // swallow; no toggle
            auto w = std::static_pointer_cast<CheckboxWidget>(widget_);
            w->setChecked(!w->checked());
            std::static_pointer_cast<RenderCheckbox>(renderNode_)->setChecked(w->checked());
            if (w->onChanged()) w->onChanged()(w->checked());
            return true;
        }
        return Element::handleEvent(event);
    }

    [[nodiscard]] SemanticNode getSemanticTree() const noexcept override {
        auto w   = std::static_pointer_cast<const CheckboxWidget>(widget_);
        auto abs = renderNode_->getAbsoluteOffset();
        auto sz  = renderNode_->getSize();
        std::string lbl = renderNode_->accessibilityLabel().empty()
                              ? "Checkbox" : renderNode_->accessibilityLabel();
        return SemanticNode{
            .id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF),
            .role = "Checkbox", .label = lbl,
            .value = w->checked() ? "checked" : "unchecked",
            .x = abs.x_, .y = abs.y_, .width = sz.width_, .height = sz.height_
        };
    }
};

std::shared_ptr<Element> CheckboxWidget::createElement() noexcept {
    return std::make_shared<CheckboxElement>(std::static_pointer_cast<CheckboxWidget>(shared_from_this()));
}

// ─── SwitchElement ────────────────────────────────────────────────────────────

class SwitchElement : public Element {
public:
    explicit SwitchElement(std::shared_ptr<SwitchWidget> widget) {
        widget_ = std::static_pointer_cast<Widget>(widget);
        auto node = std::make_shared<RenderSwitch>(widget->value());
        renderNode_ = node;
        apply_widget_props(renderNode_, widget_);
    }
    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
    }
    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        auto w = std::static_pointer_cast<SwitchWidget>(widget_);
        std::static_pointer_cast<RenderSwitch>(renderNode_)->setValue(w->value());
        apply_widget_props(renderNode_, widget_);
    }
    void unmount() noexcept override { parent_.reset(); }
    void syncState() noexcept override {
        bool disabled = widget_->isDisabled();
        renderNode_->setCursor(disabled ? CursorShape::NotAllowed
                                        : CursorShape::Pointer);
        renderNode_->setOpacity(disabled ? kDisabledOpacity : 1.0f);
        Element::syncState();
    }
    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        if (event.type_ == shared::InputEventType::MouseButton && event.char_value_ != 0) {
            if (widget_->isDisabled()) return true;  // swallow; no toggle
            auto w = std::static_pointer_cast<SwitchWidget>(widget_);
            w->setValue(!w->value());
            auto node = std::static_pointer_cast<RenderSwitch>(renderNode_);
            node->toggle();
            if (w->onChanged()) w->onChanged()(w->value());
            return true;
        }
        return Element::handleEvent(event);
    }

    [[nodiscard]] SemanticNode getSemanticTree() const noexcept override {
        auto w   = std::static_pointer_cast<const SwitchWidget>(widget_);
        auto abs = renderNode_->getAbsoluteOffset();
        auto sz  = renderNode_->getSize();
        std::string lbl = renderNode_->accessibilityLabel().empty()
                              ? "Switch" : renderNode_->accessibilityLabel();
        return SemanticNode{
            .id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF),
            .role = "Switch", .label = lbl,
            .value = w->value() ? "on" : "off",
            .x = abs.x_, .y = abs.y_, .width = sz.width_, .height = sz.height_
        };
    }
};

std::shared_ptr<Element> SwitchWidget::createElement() noexcept {
    return std::make_shared<SwitchElement>(std::static_pointer_cast<SwitchWidget>(shared_from_this()));
}

// ─── SliderElement ────────────────────────────────────────────────────────────

class SliderElement : public Element {
public:
    explicit SliderElement(std::shared_ptr<SliderWidget> widget) {
        widget_ = std::static_pointer_cast<Widget>(widget);
        auto node = std::make_shared<RenderSlider>(widget->value(), widget->minVal(), widget->maxVal());
        renderNode_ = node;
        apply_widget_props(renderNode_, widget_);
    }
    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
    }
    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        auto w = std::static_pointer_cast<SliderWidget>(widget_);
        std::static_pointer_cast<RenderSlider>(renderNode_)->setValue(w->value());
        apply_widget_props(renderNode_, widget_);
    }
    void unmount() noexcept override { parent_.reset(); }
    void syncState() noexcept override {
        bool disabled = widget_->isDisabled();
        renderNode_->setCursor(disabled ? CursorShape::NotAllowed
                                        : CursorShape::Pointer);
        renderNode_->setOpacity(disabled ? kDisabledOpacity : 1.0f);
        Element::syncState();
    }
    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        auto node = std::static_pointer_cast<RenderSlider>(renderNode_);
        auto w = std::static_pointer_cast<SliderWidget>(widget_);
        if (widget_->isDisabled()) {
            // Swallow presses/drags on a disabled slider; leave it inert.
            if (event.type_ == shared::InputEventType::MouseButton) return true;
            return Element::handleEvent(event);
        }
        if (event.type_ == shared::InputEventType::MouseButton) {
            // Button state is in char_value_ (value_a_/value_b_ hold the cursor).
            if (event.char_value_ != 0) {   // press → grab the pointer for the drag
                XApplication::capturePointer(shared_from_this());
                node->startDrag();
                float local_x = static_cast<float>(event.value_a_) - renderNode_->getAbsoluteOffset().x_;
                float new_val = node->computeValue(local_x);
                node->setValue(new_val);
                w->setValue(new_val);
                if (w->onChanged()) w->onChanged()(new_val);
                return true;
            } else {                        // release → end drag, drop the capture
                node->endDrag();
                XApplication::releasePointer();
                return true;
            }
        }
        if (event.type_ == shared::InputEventType::MouseMove && node->dragging()) {
            float local_x = static_cast<float>(event.value_a_) - renderNode_->getAbsoluteOffset().x_;
            float new_val = node->computeValue(local_x);
            node->setValue(new_val);
            w->setValue(new_val);
            if (w->onChanged()) w->onChanged()(new_val);
            return true;
        }
        return Element::handleEvent(event);
    }

    [[nodiscard]] SemanticNode getSemanticTree() const noexcept override {
        auto w   = std::static_pointer_cast<const SliderWidget>(widget_);
        auto abs = renderNode_->getAbsoluteOffset();
        auto sz  = renderNode_->getSize();
        std::string lbl = renderNode_->accessibilityLabel().empty()
                              ? "Slider" : renderNode_->accessibilityLabel();
        // Express as percentage when range is sensible.
        std::string val;
        float range = w->maxVal() - w->minVal();
        if (range > 0.0f) {
            int pct = static_cast<int>((w->value() - w->minVal()) / range * 100.0f + 0.5f);
            val = std::to_string(pct) + "%";
        } else {
            val = std::to_string(static_cast<int>(w->value()));
        }
        return SemanticNode{
            .id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF),
            .role = "Slider", .label = lbl, .value = val,
            .x = abs.x_, .y = abs.y_, .width = sz.width_, .height = sz.height_
        };
    }
};

std::shared_ptr<Element> SliderWidget::createElement() noexcept {
    return std::make_shared<SliderElement>(std::static_pointer_cast<SliderWidget>(shared_from_this()));
}

// ─── ProgressBarElement ───────────────────────────────────────────────────────

class ProgressBarElement : public Element {
public:
    explicit ProgressBarElement(std::shared_ptr<ProgressBarWidget> widget) {
        widget_ = std::static_pointer_cast<Widget>(widget);
        renderNode_ = std::make_shared<RenderProgressBar>(widget->value());
        apply_widget_props(renderNode_, widget_);
    }
    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
    }
    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        auto w = std::static_pointer_cast<ProgressBarWidget>(widget_);
        std::static_pointer_cast<RenderProgressBar>(renderNode_)->setValue(w->value());
        apply_widget_props(renderNode_, widget_);
    }
    void unmount() noexcept override { parent_.reset(); }
};

std::shared_ptr<Element> ProgressBarWidget::createElement() noexcept {
    return std::make_shared<ProgressBarElement>(std::static_pointer_cast<ProgressBarWidget>(shared_from_this()));
}

// ─── RadioButtonElement ───────────────────────────────────────────────────────

class RadioButtonElement : public Element {
public:
    explicit RadioButtonElement(std::shared_ptr<RadioButtonWidget> widget) {
        widget_ = std::static_pointer_cast<Widget>(widget);
        auto w = std::static_pointer_cast<RadioButtonWidget>(widget_);
        auto node = std::make_shared<RenderRadioButton>();
        node->setSelected(w->group()->selectedIndex() == w->index());
        renderNode_ = node;
        apply_widget_props(renderNode_, widget_);
    }
    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        // Subscribe to group updates
        auto w = std::static_pointer_cast<RadioButtonWidget>(widget_);
        auto node = std::static_pointer_cast<RenderRadioButton>(renderNode_);
        int my_index = w->index();
        w->group()->subscribe([node, my_index](int selected_idx) {
            node->setSelected(selected_idx == my_index);
        });
    }
    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        auto w = std::static_pointer_cast<RadioButtonWidget>(widget_);
        std::static_pointer_cast<RenderRadioButton>(renderNode_)->setSelected(
            w->group()->selectedIndex() == w->index());
        apply_widget_props(renderNode_, widget_);
    }
    void unmount() noexcept override { parent_.reset(); }
    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        if (event.type_ == shared::InputEventType::MouseButton && event.char_value_ != 0) {
            auto w = std::static_pointer_cast<RadioButtonWidget>(widget_);
            w->group()->select(w->index());
            return true;
        }
        return Element::handleEvent(event);
    }

    [[nodiscard]] SemanticNode getSemanticTree() const noexcept override {
        auto w   = std::static_pointer_cast<const RadioButtonWidget>(widget_);
        auto abs = renderNode_->getAbsoluteOffset();
        auto sz  = renderNode_->getSize();
        std::string lbl = renderNode_->accessibilityLabel().empty()
                              ? "Radio button" : renderNode_->accessibilityLabel();
        bool selected = w->group() && w->group()->selectedIndex() == static_cast<int>(w->index());
        return SemanticNode{
            .id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF),
            .role = "Radio", .label = lbl,
            .value = selected ? "selected" : "unselected",
            .x = abs.x_, .y = abs.y_, .width = sz.width_, .height = sz.height_
        };
    }
};

std::shared_ptr<Element> RadioButtonWidget::createElement() noexcept {
    return std::make_shared<RadioButtonElement>(std::static_pointer_cast<RadioButtonWidget>(shared_from_this()));
}

// ─── CardElement ──────────────────────────────────────────────────────────────

class CardElement : public Element {
public:
    explicit CardElement(std::shared_ptr<CardWidget> widget) {
        widget_ = std::static_pointer_cast<Widget>(widget);
        renderNode_ = std::make_shared<RenderCard>();
        apply_widget_props(renderNode_, widget_);
    }
    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        auto w = std::static_pointer_cast<CardWidget>(widget_);
        if (w->child()) {
            child_element_ = w->child()->createElement();
            child_element_->mount(shared_from_this());
            renderNode_->addChild(child_element_->getRenderNode());
        }
    }
    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        if (child_element_) child_element_->unmount();
        renderNode_->clearChildren();
        child_element_.reset();
        mount(parent_.lock());
    }
    void unmount() noexcept override {
        if (child_element_) child_element_->unmount();
        renderNode_->clearChildren();
        parent_.reset();
    }
    void onThemeChanged(const Theme& theme) noexcept override {
        (void)theme;
        if (child_element_) child_element_->onThemeChanged(theme);
    }
    void syncState() noexcept override {
        renderNode_->setVisibility(widget_->isVisible());
        if (child_element_) child_element_->syncState();
    }
    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        if (child_element_ && child_element_->handleEvent(event)) return true;
        return false;
    }
private:
    std::shared_ptr<Element> child_element_;
};

std::shared_ptr<Element> CardWidget::createElement() noexcept {
    return std::make_shared<CardElement>(std::static_pointer_cast<CardWidget>(shared_from_this()));
}

// ─── ExpanderElement ────────────────────────────────────────────────────────────
// A FlexColumn whose children are [header-row] when collapsed and
// [header-row, content] when expanded. Toggling rebuilds the child list so the
// column's measured height reflects the state (collapse actually removes the
// content from layout, it is not merely hidden). The header row is a FlexRow of
// a disclosure caret + the user's header widget; a press anywhere inside it
// toggles.
class ExpanderElement : public Element {
public:
    explicit ExpanderElement(std::shared_ptr<ExpanderWidget> widget) {
        widget_ = std::static_pointer_cast<Widget>(widget);
        auto col = std::make_shared<RenderFlexColumn>();
        col->setGap(widget->contentGap());
        renderNode_ = col;
        apply_widget_props(renderNode_, widget_);
    }

    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        rebuild();
    }

    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        rebuild();
    }

    void unmount() noexcept override {
        teardownChildren();
        parent_.reset();
    }

    void onThemeChanged(const Theme& theme) noexcept override {
        if (header_element_)  header_element_->onThemeChanged(theme);
        if (content_element_) content_element_->onThemeChanged(theme);
    }

    void syncState() noexcept override {
        renderNode_->setVisibility(widget_->isVisible());
        if (header_element_)  header_element_->syncState();
        if (content_element_) content_element_->syncState();
    }

    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        // A press inside the header row toggles the section.
        if (event.type_ == shared::InputEventType::MouseButton && event.char_value_ != 0
            && header_element_) {
            auto hn  = header_element_->getRenderNode();
            auto pos = hn->getAbsoluteOffset();
            auto sz  = hn->getSize();
            float mx = static_cast<float>(event.value_a_);
            float my = static_cast<float>(event.value_b_);
            if (mx >= pos.x_ && mx <= pos.x_ + sz.width_ &&
                my >= pos.y_ && my <= pos.y_ + sz.height_) {
                auto w = std::static_pointer_cast<ExpanderWidget>(widget_);
                w->toggle();
                rebuild();
                if (w->onToggle()) w->onToggle()(w->expanded());
                return true;
            }
        }
        // Otherwise let an expanded content subtree handle it (interactive children).
        if (content_element_ && content_element_->handleEvent(event)) return true;
        return false;
    }

    [[nodiscard]] SemanticNode getSemanticTree() const noexcept override {
        auto w   = std::static_pointer_cast<ExpanderWidget>(widget_);
        auto abs = renderNode_->getAbsoluteOffset();
        auto sz  = renderNode_->getSize();
        SemanticNode node{
            .id      = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF),
            .role    = w->accessibilityRole().empty() ? "Disclosure" : w->accessibilityRole(),
            .label   = w->accessibilityLabel(),
            .value   = w->expanded() ? "expanded" : "collapsed",
            .focused = false,
            .x = abs.x_, .y = abs.y_, .width = sz.width_, .height = sz.height_,
            .children = {}
        };
        if (header_element_) node.children.push_back(header_element_->getSemanticTree());
        if (w->expanded() && content_element_)
            node.children.push_back(content_element_->getSemanticTree());
        return node;
    }

private:
    void teardownChildren() noexcept {
        if (header_element_)  header_element_->unmount();
        if (content_element_) content_element_->unmount();
        header_element_.reset();
        content_element_.reset();
        renderNode_->clearChildren();
    }

    // (Re)build the header row, and the content below it when expanded.
    void rebuild() noexcept {
        teardownChildren();
        auto w = std::static_pointer_cast<ExpanderWidget>(widget_);

        // Header row = caret + the user's header widget; the whole row is clickable.
        auto caret = std::make_shared<TextWidget>(
            w->expanded() ? std::string("\xE2\x96\xBE")   // ▾
                          : std::string("\xE2\x96\xB8"),   // ▸
            12.0f);
        std::vector<std::shared_ptr<Widget>> row_children{caret};
        if (w->header()) row_children.push_back(w->header());
        auto headerRow = std::make_shared<FlexRow>(std::move(row_children));
        headerRow->setGap(6.0f);
        headerRow->setCrossAxisAlignment(CrossAxisAlignment::Center);
        headerRow->setCursor(CursorShape::Pointer);

        header_element_ = headerRow->createElement();
        header_element_->mount(shared_from_this());
        renderNode_->addChild(header_element_->getRenderNode());

        if (w->expanded() && w->content()) {
            content_element_ = w->content()->createElement();
            content_element_->mount(shared_from_this());
            renderNode_->addChild(content_element_->getRenderNode());
        }
    }

    std::shared_ptr<Element> header_element_;
    std::shared_ptr<Element> content_element_;
};

std::shared_ptr<Element> ExpanderWidget::createElement() noexcept {
    return std::make_shared<ExpanderElement>(std::static_pointer_cast<ExpanderWidget>(shared_from_this()));
}

// ─── StepperElement ─────────────────────────────────────────────────────────────
// A fixed-width RenderFlexRow laid out [− button] value [+ button] with
// SpaceBetween. A press is routed by x-zone against the control's own width
// (deterministic, like SegmentedControl/Table) rather than the buttons' own
// hit-testing: left buttonWidth px → stepDown, right buttonWidth px → stepUp.
// The value label tracks state in syncState (setText on its RenderText), so no
// subtree rebuild is needed on a step — and no element is torn down mid-click.
std::string StepperWidget::formatValue() const {
    if (formatter_) return formatter_(value_);
    char buf[32];
    if (value_ == std::floor(value_)) std::snprintf(buf, sizeof(buf), "%.0f", value_);
    else                              std::snprintf(buf, sizeof(buf), "%g",   value_);
    return buf;
}

class StepperElement : public Element {
    static constexpr float kHeight = 32.0f;
public:
    explicit StepperElement(std::shared_ptr<StepperWidget> widget) {
        widget_ = std::static_pointer_cast<Widget>(widget);
        auto row = std::make_shared<RenderFlexRow>();
        renderNode_ = row;
        apply_widget_props(renderNode_, widget_);
        // Assert our geometry/alignment AFTER apply_widget_props (which would
        // otherwise reset alignment to the widget's Start defaults).
        row->setFixedSize(widget->width(), kHeight);
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        row->setMainAxisAlignment(MainAxisAlignment::SpaceBetween);
    }

    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        auto w = std::static_pointer_cast<StepperWidget>(widget_);

        auto makeBtn = [&](const char* glyph) {
            auto b = std::make_shared<ButtonWidget>(std::string(glyph));
            b->setFixedSize(w->buttonWidth(), kHeight - 4.0f);
            return b;
        };
        auto minusBtn = makeBtn("\xE2\x88\x92");          // U+2212 minus sign
        auto valueTxt = std::make_shared<TextWidget>(w->formatValue(), 14.0f);
        auto plusBtn  = makeBtn("+");

        for (auto& cw : {std::static_pointer_cast<Widget>(minusBtn),
                         std::static_pointer_cast<Widget>(valueTxt),
                         std::static_pointer_cast<Widget>(plusBtn)}) {
            auto el = cw->createElement();
            el->mount(shared_from_this());
            renderNode_->addChild(el->getRenderNode());
            child_elements_.push_back(el);
        }
        // Remember the value label's RenderText so syncState can retitle it live.
        value_render_ = std::dynamic_pointer_cast<RenderText>(child_elements_[1]->getRenderNode());
    }

    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        for (auto& e : child_elements_) e->unmount();
        child_elements_.clear();
        renderNode_->clearChildren();
        value_render_.reset();
        mount(parent_.lock());
    }

    void unmount() noexcept override {
        for (auto& e : child_elements_) e->unmount();
        child_elements_.clear();
        renderNode_->clearChildren();
        value_render_.reset();
        parent_.reset();
    }

    void onThemeChanged(const Theme& theme) noexcept override {
        for (auto& e : child_elements_) e->onThemeChanged(theme);
    }

    void syncState() noexcept override {
        renderNode_->setVisibility(widget_->isVisible());
        auto w = std::static_pointer_cast<StepperWidget>(widget_);
        if (value_render_) value_render_->setText(w->formatValue());
        for (auto& e : child_elements_) e->syncState();
    }

    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        if (event.type_ == shared::InputEventType::MouseButton && event.char_value_ != 0) {
            auto w   = std::static_pointer_cast<StepperWidget>(widget_);
            auto pos = renderNode_->getAbsoluteOffset();
            auto sz  = renderNode_->getSize();
            float lx = static_cast<float>(event.value_a_) - pos.x_;
            float ly = static_cast<float>(event.value_b_) - pos.y_;
            if (ly < 0 || ly > sz.height_ || lx < 0 || lx > sz.width_)
                return false;                           // outside the control
            const float bw = w->buttonWidth();
            bool moved = false;
            if (lx < bw)                 moved = w->stepDown();   // left zone
            else if (lx > sz.width_ - bw) moved = w->stepUp();     // right zone
            else return false;                          // middle (the value) — ignore
            if (moved && w->onChanged()) w->onChanged()(w->value());
            return true;                                // a ± zone consumed the press
        }
        return false;
    }

    [[nodiscard]] SemanticNode getSemanticTree() const noexcept override {
        auto w   = std::static_pointer_cast<StepperWidget>(widget_);
        auto abs = renderNode_->getAbsoluteOffset();
        auto sz  = renderNode_->getSize();
        return SemanticNode{
            .id      = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) & 0xFFFFFFFF),
            .role    = w->accessibilityRole().empty() ? "Stepper" : w->accessibilityRole(),
            .label   = w->accessibilityLabel(),
            .value   = w->formatValue(),
            .focused = false,
            .x = abs.x_, .y = abs.y_, .width = sz.width_, .height = sz.height_,
            .children = {}
        };
    }

private:
    std::vector<std::shared_ptr<Element>> child_elements_;
    std::shared_ptr<RenderText> value_render_;
};

std::shared_ptr<Element> StepperWidget::createElement() noexcept {
    return std::make_shared<StepperElement>(std::static_pointer_cast<StepperWidget>(shared_from_this()));
}

// ─── SpinnerElement ─────────────────────────────────────────────────────────────
// Drives a RenderSpinner: each syncState (per frame) it advances the ring's head
// position from the wall-clock elapsed since construction. It never "finishes" —
// the scene keeps changing, so the compositor keeps it on screen. No child tree.
class SpinnerElement : public Element {
public:
    explicit SpinnerElement(std::shared_ptr<SpinnerWidget> w) {
        widget_ = std::move(w);
        auto sw = std::static_pointer_cast<SpinnerWidget>(widget_);
        const Theme& th = ThemeManager::getInstance().getCurrentTheme();
        Color c = sw->hasColor() ? sw->color() : th.accent;
        renderNode_ = std::make_shared<RenderSpinner>(sw->diameter(), sw->dotCount(), c);
        apply_widget_props(renderNode_, widget_);
        start_ = std::chrono::steady_clock::now();
    }
    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
    }
    void update(std::shared_ptr<Widget> nw) noexcept override { widget_ = std::move(nw); }
    void unmount() noexcept override { parent_.reset(); }
    void onThemeChanged(const Theme& th) noexcept override {
        auto sw = std::static_pointer_cast<SpinnerWidget>(widget_);
        if (!sw->hasColor())
            std::static_pointer_cast<RenderSpinner>(renderNode_)->setColor(th.accent);
    }
    void syncState() noexcept override {
        renderNode_->setVisibility(widget_->isVisible());
        auto sw = std::static_pointer_cast<SpinnerWidget>(widget_);
        float elapsed = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - start_).count();
        float head = std::fmod(elapsed / sw->period(), 1.0f) * static_cast<float>(sw->dotCount());
        std::static_pointer_cast<RenderSpinner>(renderNode_)->setPhase(head);
    }
private:
    std::chrono::steady_clock::time_point start_;
};

std::shared_ptr<Element> SpinnerWidget::createElement() noexcept {
    return std::make_shared<SpinnerElement>(std::static_pointer_cast<SpinnerWidget>(shared_from_this()));
}

// ─── DividerElement ───────────────────────────────────────────────────────────

class DividerElement : public Element {
public:
    explicit DividerElement(std::shared_ptr<DividerWidget> widget) {
        widget_ = std::static_pointer_cast<Widget>(widget);
        renderNode_ = std::make_shared<RenderDivider>(widget->thickness());
        apply_widget_props(renderNode_, widget_);
    }
    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
    }
    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        apply_widget_props(renderNode_, widget_);
    }
    void unmount() noexcept override { parent_.reset(); }
};

std::shared_ptr<Element> DividerWidget::createElement() noexcept {
    return std::make_shared<DividerElement>(std::static_pointer_cast<DividerWidget>(shared_from_this()));
}

// ─── SplitterElement ──────────────────────────────────────────────────────────

class SplitterElement : public Element {
    bool vertical_{true};
    bool dragging_{false};
public:
    explicit SplitterElement(std::shared_ptr<SplitterWidget> widget) {
        vertical_ = widget->orientation() == SplitterWidget::Orientation::Vertical;
        widget_ = std::static_pointer_cast<Widget>(widget);
        renderNode_ = std::make_shared<RenderSplitter>(vertical_, widget->thickness());
        // A vertical bar resizes left/right (col-resize ↔); a horizontal bar
        // resizes top/bottom (row-resize ↕).
        renderNode_->setCursor(vertical_ ? CursorShape::ColResize
                                         : CursorShape::RowResize);
        apply_widget_props(renderNode_, widget_);
    }
    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
    }
    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        apply_widget_props(renderNode_, widget_);
    }
    void unmount() noexcept override { parent_.reset(); }

    [[nodiscard]] bool handleEvent(const shared::InputMessage& ev) noexcept override {
        if (ev.type_ == shared::InputEventType::MouseButton) {
            if (ev.char_value_ != 0) {           // button down → begin drag
                dragging_ = true;
                XApplication::capturePointer(shared_from_this());
                reportRatio(ev);
            } else {                              // button up → end drag
                dragging_ = false;               // capture auto-released by the app
            }
            return true;
        }
        if (ev.type_ == shared::InputEventType::MouseMove && dragging_) {
            reportRatio(ev);
            return true;
        }
        return Element::handleEvent(ev);
    }

private:
    // Express the cursor's position along the split axis as a fraction of the
    // container and hand it to the host's onDrag callback.
    void reportRatio(const shared::InputMessage& ev) noexcept {
        auto w = std::static_pointer_cast<SplitterWidget>(widget_);
        const auto& cb = w->onDragCallback();
        if (!cb) return;
        Size   psize = renderNode_->parentSize();
        Offset poff  = renderNode_->parentAbsoluteOffset();
        float ratio;
        if (vertical_) {
            if (psize.width_ <= 1.0f) return;
            ratio = (static_cast<float>(ev.value_a_) - poff.x_) / psize.width_;
        } else {
            if (psize.height_ <= 1.0f) return;
            ratio = (static_cast<float>(ev.value_b_) - poff.y_) / psize.height_;
        }
        ratio = ratio < 0.1f ? 0.1f : (ratio > 0.9f ? 0.9f : ratio);
        cb(ratio);
    }
};

std::shared_ptr<Element> SplitterWidget::createElement() noexcept {
    return std::make_shared<SplitterElement>(std::static_pointer_cast<SplitterWidget>(shared_from_this()));
}

// ─── ListTileElement ──────────────────────────────────────────────────────────

class ListTileElement : public Element {
public:
    explicit ListTileElement(std::shared_ptr<ListTileWidget> widget) {
        widget_ = std::static_pointer_cast<Widget>(widget);
        auto node = std::make_shared<RenderListTile>();
        auto w = std::static_pointer_cast<ListTileWidget>(widget_);
        node->setHasLeading(w->leading() != nullptr);
        node->setHasTitle(w->title() != nullptr);
        node->setHasSubtitle(w->subtitle() != nullptr);
        node->setHasTrailing(w->trailing() != nullptr);
        renderNode_ = node;
        apply_widget_props(renderNode_, widget_);
    }
    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        auto w = std::static_pointer_cast<ListTileWidget>(widget_);
        // Order: leading, title, subtitle, trailing
        for (auto* sw : {&w->leading(), &w->title(), &w->subtitle(), &w->trailing()}) {
            if (*sw) {
                auto el = (*sw)->createElement();
                el->mount(shared_from_this());
                child_elements_.push_back(el);
                renderNode_->addChild(el->getRenderNode());
            }
        }
    }
    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        for (auto& el : child_elements_) el->unmount();
        child_elements_.clear();
        renderNode_->clearChildren();
        mount(parent_.lock());
    }
    void unmount() noexcept override {
        for (auto& el : child_elements_) el->unmount();
        child_elements_.clear();
        renderNode_->clearChildren();
        parent_.reset();
    }
    void onThemeChanged(const Theme& theme) noexcept override {
        for (auto& el : child_elements_) el->onThemeChanged(theme);
    }
    void syncState() noexcept override {
        renderNode_->setVisibility(widget_->isVisible());
        for (auto& el : child_elements_) el->syncState();
    }
    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        // Route mouse events to the specific child element under the cursor first
        // (e.g. a SwitchWidget in the trailing slot should receive the click, not just onTap).
        if (event.type_ == shared::InputEventType::MouseMove ||
            event.type_ == shared::InputEventType::MouseButton) {
            float mx = static_cast<float>(event.value_a_);
            float my = static_cast<float>(event.value_b_);
            for (auto& child : child_elements_) {
                auto node = child->getRenderNode();
                auto off  = node->getAbsoluteOffset();
                auto sz   = node->getSize();
                if (mx >= off.x_ && mx < off.x_ + sz.width_ &&
                    my >= off.y_ && my < off.y_ + sz.height_) {
                    if (child->handleEvent(event)) return true;
                }
            }
        }
        // Tile-level press / tap fallback.
        if (event.type_ == shared::InputEventType::MouseButton) {
            auto node = std::static_pointer_cast<RenderListTile>(renderNode_);
            if (event.char_value_ != 0) {
                node->setPressed(true);
                auto w = std::static_pointer_cast<ListTileWidget>(widget_);
                if (w->onTap()) w->onTap()();
                return true;
            } else {
                node->setPressed(false);
                return true;
            }
        }
        return false;
    }
private:
    std::vector<std::shared_ptr<Element>> child_elements_;
};

std::shared_ptr<Element> ListTileWidget::createElement() noexcept {
    return std::make_shared<ListTileElement>(std::static_pointer_cast<ListTileWidget>(shared_from_this()));
}

// ─── BadgeElement ─────────────────────────────────────────────────────────────

class BadgeElement : public Element {
public:
    explicit BadgeElement(std::shared_ptr<BadgeWidget> widget) {
        widget_ = std::static_pointer_cast<Widget>(widget);
        auto node = std::make_shared<RenderBadge>(widget->count());
        renderNode_ = node;
        apply_widget_props(renderNode_, widget_);
    }
    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        renderNode_->setOwnerElement(shared_from_this());
        auto w = std::static_pointer_cast<BadgeWidget>(widget_);
        if (w->child()) {
            child_element_ = w->child()->createElement();
            child_element_->mount(shared_from_this());
            renderNode_->addChild(child_element_->getRenderNode());
        }
    }
    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        widget_ = std::move(newWidget);
        auto w = std::static_pointer_cast<BadgeWidget>(widget_);
        std::static_pointer_cast<RenderBadge>(renderNode_)->setCount(w->count());
        if (child_element_) child_element_->unmount();
        renderNode_->clearChildren();
        child_element_.reset();
        mount(parent_.lock());
    }
    void unmount() noexcept override {
        if (child_element_) child_element_->unmount();
        renderNode_->clearChildren();
        parent_.reset();
    }
    void onThemeChanged(const Theme& theme) noexcept override {
        if (child_element_) child_element_->onThemeChanged(theme);
    }
    void syncState() noexcept override {
        renderNode_->setVisibility(widget_->isVisible());
        if (child_element_) child_element_->syncState();
    }
private:
    std::shared_ptr<Element> child_element_;
};

std::shared_ptr<Element> BadgeWidget::createElement() noexcept {
    return std::make_shared<BadgeElement>(std::static_pointer_cast<BadgeWidget>(shared_from_this()));
}

// ─── DragSourceWidget ─────────────────────────────────────────────────────────

class DragSourceElement : public SingleChildElement {
    bool  watching_{false};      // user pressed; watching for drag threshold
    float press_x_{0};
    float press_y_{0};
    static constexpr float kDragThreshold = 8.0f;
public:
    explicit DragSourceElement(std::shared_ptr<DragSourceWidget> w)
        : SingleChildElement(w, std::make_shared<RenderBox>(0, 0)) {
        // Open hand while hovering; the compositor swaps to the closed fist
        // (grabbing) for the duration of an active drag.
        renderNode_->setCursor(CursorShape::Grab);
    }

    [[nodiscard]] bool handleEvent(const shared::InputMessage& event) noexcept override {
        auto widget = std::static_pointer_cast<DragSourceWidget>(widget_);
        auto pos  = renderNode_->getAbsoluteOffset();
        auto size = renderNode_->getSize();

        if (event.type_ == shared::InputEventType::MouseButton) {
            if (event.char_value_ != 0) {  // press
                float mx = static_cast<float>(event.value_a_);
                float my = static_cast<float>(event.value_b_);
                bool in_bounds = mx >= pos.x_ && mx <= pos.x_ + size.width_ &&
                                 my >= pos.y_ && my <= pos.y_ + size.height_;
                if (in_bounds) {
                    press_x_  = mx;
                    press_y_  = my;
                    watching_ = true;
                    // Consume the press so the child doesn't act until we know
                    // whether this becomes a drag.
                    return true;
                }
                watching_ = false;
            } else {  // release — stop watching so a later hover can't start a drag
                watching_ = false;
            }
        }

        if (event.type_ == shared::InputEventType::MouseMove && watching_) {
            float mx = static_cast<float>(event.value_a_);
            float my = static_cast<float>(event.value_b_);
            float dx = mx - press_x_;
            float dy = my - press_y_;
            if (dx * dx + dy * dy > kDragThreshold * kDragThreshold) {
                watching_ = false;
                const auto& provider = widget->dataProvider();
                if (provider) {
                    XApplication::startDrag(widget->mimeType(), provider());
                }
                return true;
            }
        }

        // Below drag threshold: forward to child (tap / normal interaction).
        return child_element_ ? child_element_->handleEvent(event) : false;
    }
};

std::shared_ptr<Element> DragSourceWidget::createElement() noexcept {
    return std::make_shared<DragSourceElement>(
        std::static_pointer_cast<DragSourceWidget>(shared_from_this()));
}

// ─── GestureDetectorElement ───────────────────────────────────────────────────

class GestureDetectorElement : public SingleChildElement {
    // Slot-0 touch state for tap / pan detection.
    bool    touching_{false};
    float   press_x_{0}, press_y_{0};
    float   last_x_{0},  last_y_{0};
    static constexpr float kTapRadius    = 12.0f;
    static constexpr float kPanThreshold =  8.0f;
    bool    panning_{false};

public:
    explicit GestureDetectorElement(std::shared_ptr<GestureDetectorWidget> w)
        : SingleChildElement(w, std::make_shared<RenderBox>(0, 0)) {}

    [[nodiscard]] bool handleEvent(const shared::InputMessage& ev) noexcept override {
        auto w = std::static_pointer_cast<GestureDetectorWidget>(widget_);

        // ── Pinch (compositor-synthesised) ───────────────────────────────────
        if (ev.type_ == shared::InputEventType::GesturePinchUpdate) {
            if (w->onPinch()) {
                float scale = static_cast<float>(ev.modifiers_) / 1000.0f;
                float dx    = static_cast<float>(ev.value_a_) / 100.0f;
                float dy    = static_cast<float>(ev.value_b_) / 100.0f;
                w->onPinch()(scale, dx, dy);
            }
            return true;
        }
        if (ev.type_ == shared::InputEventType::GesturePinchBegin ||
            ev.type_ == shared::InputEventType::GesturePinchEnd)
            return true;

        // ── Single-finger tap / pan (slot 0 only) ────────────────────────────
        if (ev.type_ == shared::InputEventType::TouchDown && ev.modifiers_ == 0) {
            press_x_  = static_cast<float>(ev.value_a_);
            press_y_  = static_cast<float>(ev.value_b_);
            last_x_   = press_x_;
            last_y_   = press_y_;
            touching_ = true;
            panning_  = false;
            return true;
        }
        if (ev.type_ == shared::InputEventType::TouchMove && ev.modifiers_ == 0 && touching_) {
            float cx = static_cast<float>(ev.value_a_);
            float cy = static_cast<float>(ev.value_b_);
            float dx = cx - last_x_;
            float dy = cy - last_y_;
            float dist_from_press = std::hypot(cx - press_x_, cy - press_y_);
            if (!panning_ && dist_from_press > kPanThreshold) panning_ = true;
            if (panning_ && w->onPan()) w->onPan()(dx, dy);
            last_x_ = cx;
            last_y_ = cy;
            return true;
        }
        if (ev.type_ == shared::InputEventType::TouchUp && ev.modifiers_ == 0 && touching_) {
            touching_ = false;
            if (!panning_ && w->onTap()) w->onTap()(press_x_, press_y_);
            panning_ = false;
            return true;
        }

        // All other events fall through to the child.
        return child_element_ ? child_element_->handleEvent(ev) : false;
    }
};

std::shared_ptr<Element> GestureDetectorWidget::createElement() noexcept {
    return std::make_shared<GestureDetectorElement>(
        std::static_pointer_cast<GestureDetectorWidget>(shared_from_this()));
}

// ─── DrawingSurfaceElement ────────────────────────────────────────────────────

class DrawingSurfaceElement : public Element {
    bool stylus_down_{false};
    bool finger_down_{false};
public:
    explicit DrawingSurfaceElement(std::shared_ptr<DrawingSurfaceWidget> w) {
        widget_     = std::static_pointer_cast<Widget>(w);
        renderNode_ = std::make_shared<RenderBox>(0, 0);
        apply_widget_props(renderNode_, widget_);
    }
    void mount(std::shared_ptr<Element> p) noexcept override {
        parent_ = p; renderNode_->setOwnerElement(shared_from_this());
    }
    void update(std::shared_ptr<Widget> nw) noexcept override { widget_ = std::move(nw); }
    void unmount() noexcept override { parent_.reset(); }

    [[nodiscard]] bool handleEvent(const shared::InputMessage& ev) noexcept override {
        auto w = std::static_pointer_cast<DrawingSurfaceWidget>(widget_);

        // ── Stylus ───────────────────────────────────────────────────────────
        if (ev.type_ == shared::InputEventType::StylusDown ||
            ev.type_ == shared::InputEventType::StylusMove) {
            stylus_down_ = true;
            if (w->onStroke()) {
                float x    = static_cast<float>(ev.value_a_);
                float y    = static_cast<float>(ev.value_b_);
                float prs  = static_cast<float>(ev.modifiers_) / 65535.0f;
                // Decode packed tilt: high-16 = tilt_x*10, low-16 = tilt_y*10
                int16_t raw_tx = static_cast<int16_t>((ev.char_value_ >> 16) & 0xFFFF);
                int16_t raw_ty = static_cast<int16_t>(ev.char_value_ & 0xFFFF);
                float tilt_x = static_cast<float>(raw_tx) / 10.0f;
                float tilt_y = static_cast<float>(raw_ty) / 10.0f;
                w->onStroke()(x, y, prs, tilt_x, tilt_y);
            }
            return true;
        }
        if (ev.type_ == shared::InputEventType::StylusUp) {
            stylus_down_ = false;
            if (w->onLift()) w->onLift()();
            return true;
        }

        // ── Finger drawing (slot 0 only; stylus takes priority) ──────────────
        if (!stylus_down_) {
            if (ev.type_ == shared::InputEventType::TouchDown && ev.modifiers_ == 0) {
                finger_down_ = true;
                if (w->onStroke())
                    w->onStroke()(static_cast<float>(ev.value_a_),
                                  static_cast<float>(ev.value_b_),
                                  0.5f, 0.0f, 0.0f);
                return true;
            }
            if (ev.type_ == shared::InputEventType::TouchMove && ev.modifiers_ == 0 && finger_down_) {
                if (w->onStroke())
                    w->onStroke()(static_cast<float>(ev.value_a_),
                                  static_cast<float>(ev.value_b_),
                                  0.5f, 0.0f, 0.0f);
                return true;
            }
            if (ev.type_ == shared::InputEventType::TouchUp && ev.modifiers_ == 0 && finger_down_) {
                finger_down_ = false;
                if (w->onLift()) w->onLift()();
                return true;
            }
        }

        return false;
    }
};

std::shared_ptr<Element> DrawingSurfaceWidget::createElement() noexcept {
    return std::make_shared<DrawingSurfaceElement>(
        std::static_pointer_cast<DrawingSurfaceWidget>(shared_from_this()));
}

} // namespace fbx::xkit
