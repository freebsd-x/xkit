#pragma once

#include "xkit/layout.h"
#include "xkit/theme.h"
#include "xkit/cursor.h"
#include "xkit/overlay_layout.h"
#include "xrender/display_list.h"
#include "xrender/font_manager.h"
#include "shared/protocol.h"
#include <vector>
#include <memory>
#include <iostream>
#include <algorithm>
#include <span>
#include <cstring>
#include <cmath>

namespace fbx::xkit {

class Element;

enum class ImageFitMode : uint8_t {
    Stretch, // fill display area, ignore aspect ratio
    Cover,   // scale to fill, crop excess from center
    Contain, // scale to fit within area, letterbox/pillarbox
    Center,  // natural size, no scaling
};

class RenderNode : public std::enable_shared_from_this<RenderNode> {
public:
    virtual ~RenderNode() noexcept = default;

    [[nodiscard]] Size layout(const Constraints& constraints) noexcept {
        if (hasFixedSize_ && (fixedWidth_ > 0.0f) != (fixedHeight_ > 0.0f)) {
            // One axis fixed, the other 0: "size to content on that axis"
            // (setFixedSize(0, 26) = auto width, 26 px tall). Taken literally,
            // a 0-wide toolbar or button collapsed and its contents piled up.
            Constraints c = constraints;
            if (fixedWidth_ > 0.0f) {
                c.min_width_ = c.max_width_ = std::clamp(fixedWidth_, constraints.min_width_, constraints.max_width_);
            } else {
                c.min_height_ = c.max_height_ = std::clamp(fixedHeight_, constraints.min_height_, constraints.max_height_);
            }
            size_ = c.clamp(performLayout(c));
        } else if (hasFixedSize_) {
            // Both fixed (setFixedSize(0, 0) collapses a hidden panel on purpose).
            size_ = constraints.clamp({fixedWidth_, fixedHeight_});
            // Fixed size already decided; performLayout runs for its side effects
            // (laying out children), so its returned size is intentionally dropped.
            (void)performLayout(Constraints::tight(size_));
        } else {
            size_ = performLayout(constraints);
        }
        return size_;
    }

    [[nodiscard]] virtual Size performLayout(const Constraints& constraints) noexcept = 0;

    virtual void paint(fbx::xrender::DisplayList& displayList, Offset parentOffset = {0,0}) noexcept {
        if (!visible_) return;
        Offset globalOffset = {parentOffset.x_ + offset_.x_, parentOffset.y_ + offset_.y_};

        // Group opacity: remember where this subtree's commands begin so their
        // alpha can be scaled down together once painted (e.g. dimming a
        // disabled control, including its children).
        const size_t opacity_start = displayList.commandCount();

        if (hasBackground_) {
            fbx::shared::RectData bg = {
                .x_ = globalOffset.x_, .y_ = globalOffset.y_,
                .width_ = size_.width_, .height_ = size_.height_,
                .r_ = backgroundColor_.r, .g_ = backgroundColor_.g, .b_ = backgroundColor_.b, .a_ = backgroundColor_.a,
                .border_radius_ = borderRadius_, .shadow_blur_ = 0, .shadow_alpha_ = 0
            };
            displayList.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&bg), sizeof(bg)});
        }

        paintSelf(displayList, globalOffset);
        for (const auto& child : children_) {
            child->paint(displayList, globalOffset);
        }

        if (opacity_ < 1.0f) {
            displayList.scaleAlphaFrom(opacity_start, opacity_);
        }
    }

    virtual void paintSelf(fbx::xrender::DisplayList& displayList, Offset globalOffset) noexcept = 0;

    [[nodiscard]] virtual std::shared_ptr<RenderNode> hitTest(Offset localPosition) noexcept {
        if (localPosition.x_ < 0 || localPosition.x_ > size_.width_ ||
            localPosition.y_ < 0 || localPosition.y_ > size_.height_) {
            return nullptr;
        }
        for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
            auto& child = *it;
            Offset childLocal = {localPosition.x_ - child->getOffset().x_, localPosition.y_ - child->getOffset().y_};
            if (auto hit = child->hitTest(childLocal)) return hit;
        }
        return shared_from_this();
    }

    void addChild(std::shared_ptr<RenderNode> child) noexcept {
        child->parent_ = shared_from_this();
        children_.push_back(std::move(child));
    }
    void clearChildren() noexcept { children_.clear(); }
    [[nodiscard]] const std::vector<std::shared_ptr<RenderNode>>& children() const noexcept { return children_; }

    constexpr void setOffset(Offset offset) noexcept { offset_ = offset; }
    [[nodiscard]] constexpr Offset getOffset() const noexcept { return offset_; }
    [[nodiscard]] constexpr Size getSize() const noexcept { return size_; }

    [[nodiscard]] Offset getAbsoluteOffset() const noexcept {
        Offset abs = offset_;
        auto p = parent_.lock();
        while (p) {
            abs.x_ += p->offset_.x_;
            abs.y_ += p->offset_.y_;
            p = p->parent_.lock();
        }
        return abs;
    }

    // Geometry of the parent (the flex container this node sits in). Used by a
    // splitter to express the drag position as a fraction of its container.
    [[nodiscard]] Size parentSize() const noexcept {
        auto p = parent_.lock();
        return p ? p->size_ : Size{0, 0};
    }
    [[nodiscard]] Offset parentAbsoluteOffset() const noexcept {
        auto p = parent_.lock();
        return p ? p->getAbsoluteOffset() : Offset{0, 0};
    }

    void setOwnerElement(std::weak_ptr<Element> element) noexcept { ownerElement_ = element; }
    [[nodiscard]] std::shared_ptr<Element> getOwnerElement() const noexcept { return ownerElement_.lock(); }

    // ── Cursor shape ────────────────────────────────────────────────────────────
    // A node may declare the cursor shown while the pointer hovers it (or any
    // descendant that doesn't override it). Unset nodes inherit from their parent.
    void setCursor(CursorShape c) noexcept { cursor_ = c; }
    [[nodiscard]] CursorShape cursor() const noexcept { return cursor_; }

    // Group opacity in [0,1]; scales the alpha of everything this node and its
    // children paint. 1.0 = fully opaque (the default, no effect).
    void setOpacity(float o) noexcept { opacity_ = o < 0.0f ? 0.0f : (o > 1.0f ? 1.0f : o); }
    [[nodiscard]] float opacity() const noexcept { return opacity_; }

    // Resolve the effective cursor at a local position: hit-test to the deepest
    // node under the point, then walk up to the nearest ancestor that set a real
    // shape. Returns CursorShape::Default (arrow) if none did.
    [[nodiscard]] CursorShape resolveCursorAt(Offset localPosition) noexcept {
        auto hit = hitTest(localPosition);
        for (RenderNode* n = hit.get(); n; n = n->parent_.lock().get()) {
            if (n->cursor_ != CursorShape::Inherit) return n->cursor_;
        }
        return CursorShape::Default;
    }

    void setPadding(float p) noexcept { padding_ = p; }
    void setBackgroundColor(Color c) noexcept { backgroundColor_ = c; hasBackground_ = true; }
    void setFixedSize(float w, float h) noexcept { fixedWidth_ = w; fixedHeight_ = h; hasFixedSize_ = true; }
    void setBorderRadius(float r) noexcept { borderRadius_ = r; }
    void setAlignment(Alignment a) noexcept { alignment_ = a; }
    void setStackAnchor(StackAnchor a) noexcept { stack_anchor_ = a; }
    [[nodiscard]] StackAnchor stackAnchor() const noexcept { return stack_anchor_; }
    void setMinTouchSize(float px) noexcept { min_touch_ = px; }
    [[nodiscard]] float minTouchSize() const noexcept { return min_touch_; }
    void setVisibility(bool v) noexcept { visible_ = v; }
    [[nodiscard]] bool isVisible() const noexcept { return visible_; }
    [[nodiscard]] bool paintsBackground() const noexcept { return hasBackground_ && backgroundColor_.a > 0.0f; }

    // ── Flex container properties (used by RenderFlexColumn / RenderFlexRow) ──
    void setGap(float g) noexcept { gap_ = g; }
    void setMainAxisAlignment(MainAxisAlignment a) noexcept { main_axis_alignment_ = a; }
    void setCrossAxisAlignment(CrossAxisAlignment a) noexcept { cross_axis_alignment_ = a; }
    [[nodiscard]] float gap() const noexcept { return gap_; }
    [[nodiscard]] MainAxisAlignment mainAxisAlignment() const noexcept { return main_axis_alignment_; }
    [[nodiscard]] CrossAxisAlignment crossAxisAlignment() const noexcept { return cross_axis_alignment_; }

    // ── Flex child properties (set on child nodes; read by flex containers) ──
    void setFlexGrow(float g) noexcept { flex_grow_ = g; }
    void setFlexShrink(float s) noexcept { flex_shrink_ = s; }
    void setFlexBasis(float b) noexcept { flex_basis_ = b; }
    [[nodiscard]] float flexGrow()   const noexcept { return flex_grow_; }
    [[nodiscard]] float flexShrink() const noexcept { return flex_shrink_; }
    [[nodiscard]] float flexBasis()  const noexcept { return flex_basis_; }

    // ── Margin (space outside this node, consumed by the flex container) ──
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

    // ── Accessibility ─────────────────────────────────────────────────────────
    void setAccessibilityLabel(const std::string& l) noexcept { accessibility_label_ = l; }
    void setAccessibilityRole(const std::string& r)  noexcept { accessibility_role_  = r; }
    [[nodiscard]] const std::string& accessibilityLabel() const noexcept { return accessibility_label_; }
    [[nodiscard]] const std::string& accessibilityRole()  const noexcept { return accessibility_role_;  }

    // flex_grow_ > 0 → this node participates in free-space distribution.
    [[nodiscard]] virtual bool isFlexible() const noexcept { return flex_grow_ > 0.0f; }

    std::vector<std::shared_ptr<RenderNode>> children_;

protected:
    std::weak_ptr<RenderNode> parent_;
    Size size_{0, 0};
    Offset offset_{0, 0};
    std::weak_ptr<Element> ownerElement_;

    CursorShape cursor_{CursorShape::Inherit};
    float opacity_{1.0f};

    float padding_{0};
    Color backgroundColor_{0,0,0,0};
    bool hasBackground_{false};
    float fixedWidth_{0}, fixedHeight_{0};
    bool hasFixedSize_{false};
    float borderRadius_{0};
    Alignment alignment_{Alignment::Start};
    StackAnchor stack_anchor_{StackAnchor::Inherit};
    float min_touch_{0.0f};
    bool visible_{true};

    // Flex container properties
    float gap_{0.0f};
    MainAxisAlignment  main_axis_alignment_{MainAxisAlignment::Start};
    CrossAxisAlignment cross_axis_alignment_{CrossAxisAlignment::Start};

    // Flex child properties
    float flex_grow_{0.0f};
    float flex_shrink_{1.0f};
    float flex_basis_{-1.0f}; // -1 = auto

    // Accessibility
    std::string accessibility_label_;
    std::string accessibility_role_;

    // Margin (CSS box model — space outside the border, inside the flex gap)
    float margin_top_{0.0f};
    float margin_right_{0.0f};
    float margin_bottom_{0.0f};
    float margin_left_{0.0f};

    // ── Shared flexbox layout engine ──────────────────────────────────────────
    // axis == 0 → Row  (main = X, cross = Y)
    // axis == 1 → Column (main = Y, cross = X)
    static Size flexLayout(
        std::vector<std::shared_ptr<RenderNode>>& children,
        const Constraints& constraints,
        int   axis,
        float padding,
        float gap,
        MainAxisAlignment  main_align,
        CrossAxisAlignment cross_align
    ) noexcept {
        float max_main  = axis == 0 ? constraints.max_width_  : constraints.max_height_;
        float max_cross = axis == 0 ? constraints.max_height_ : constraints.max_width_;
        float avail_main  = max_main  - padding * 2.0f;
        float avail_cross = max_cross - padding * 2.0f;

        const int n = static_cast<int>(children.size());
        if (n == 0) {
            if (axis == 0) return constraints.clamp({padding * 2.0f, padding * 2.0f});
            else           return constraints.clamp({padding * 2.0f, padding * 2.0f});
        }

        // Accessors for main/cross axis values per-child
        auto mainOf  = [axis](const Size& s) { return axis == 0 ? s.width_  : s.height_; };
        auto crossOf = [axis](const Size& s) { return axis == 0 ? s.height_ : s.width_;  };
        auto mms  = [axis](const RenderNode* c) { return axis == 0 ? c->margin_left_   : c->margin_top_;    };
        auto mme  = [axis](const RenderNode* c) { return axis == 0 ? c->margin_right_  : c->margin_bottom_; };
        auto mcs  = [axis](const RenderNode* c) { return axis == 0 ? c->margin_top_    : c->margin_left_;   };
        auto mce  = [axis](const RenderNode* c) { return axis == 0 ? c->margin_bottom_ : c->margin_right_;  };

        // ── Pass 1: layout non-flexible children ─────────────────────────────
        float total_flex_grow = 0.0f;
        float total_static_main = 0.0f; // sum of non-flexible main sizes + all margins
        std::vector<Size> sizes(n);
        // Children that filled the unbounded main axis they were measured with
        // (a slider, divider or progress bar in a row): they want to fill, so
        // they share the free space like flex_grow 1 — instead of 100000 px
        // that pushed their siblings (a "100%" label, a button) off-screen.
        // Only when this container's own main axis is bounded.
        std::vector<char> auto_flex(n, 0);
        const bool main_bounded = avail_main < 50000.0f;

        for (int i = 0; i < n; ++i) {
            auto* c = children[i].get();
            if (c->flex_grow_ > 0.0f) {
                total_flex_grow += c->flex_grow_;
                // Reserve space for margins; content measured in pass 2
                total_static_main += mms(c) + mme(c);
            } else {
                // The cross axis is bounded by the container's own extent: a
                // child may be narrower, never wider. (It used to get 100000
                // unless the container stretched, so a row holding a flexible
                // spacer grew to 100000 px and everything right of the spacer —
                // toggles, values, buttons — was laid out off-screen.)
                float cross_max = std::max(0.0f, avail_cross - mcs(c) - mce(c));
                Constraints cc;
                if (c->flex_basis_ >= 0.0f) {
                    float b = c->flex_basis_;
                    cc = (axis == 0)
                       ? Constraints{b, b, 0.0f, cross_max}
                       : Constraints{0.0f, cross_max, b, b};
                } else {
                    cc = (axis == 0)
                       ? Constraints{0.0f, 100000.0f, 0.0f, cross_max}
                       : Constraints{0.0f, cross_max, 0.0f, 100000.0f};
                }
                sizes[i] = c->layout(cc);
                if (main_bounded && c->flex_basis_ < 0.0f && mainOf(sizes[i]) >= 50000.0f) {
                    auto_flex[i] = 1;
                    total_flex_grow += 1.0f;
                    total_static_main += mms(c) + mme(c);
                } else {
                    total_static_main += mainOf(sizes[i]) + mms(c) + mme(c);
                }
            }
        }

        // ── Pass 2: distribute free space to flexible children ────────────────
        float gaps_total = gap * static_cast<float>(n - 1);
        float free_space = std::max(0.0f, avail_main - gaps_total - total_static_main);

        for (int i = 0; i < n; ++i) {
            auto* c = children[i].get();
            const float grow = auto_flex[i] ? 1.0f : c->flex_grow_;
            if (grow <= 0.0f) continue;

            float basis = c->flex_basis_ >= 0.0f ? c->flex_basis_ : 0.0f;
            float alloc = basis + (total_flex_grow > 0.0f
                ? (free_space / total_flex_grow) * grow
                : 0.0f);
            alloc = std::max(0.0f, alloc);

            float cross_max = std::max(0.0f, avail_cross - mcs(c) - mce(c));   // see pass 1

            Constraints cc = (axis == 0)
                ? Constraints{alloc, alloc, 0.0f, cross_max}
                : Constraints{0.0f, cross_max, alloc, alloc};
            sizes[i] = c->layout(cc);
        }

        // ── Pass 3: compute total main consumed and justify-content offsets ───
        float total_main_used = gaps_total;
        for (int i = 0; i < n; ++i) {
            auto* c = children[i].get();
            total_main_used += mainOf(sizes[i]) + mms(c) + mme(c);
        }
        float actual_free = std::max(0.0f, avail_main - total_main_used);

        float leading      = padding;
        float between_step = gap;

        switch (main_align) {
            case MainAxisAlignment::Start:
                break; // defaults above
            case MainAxisAlignment::Center:
                leading += actual_free * 0.5f;
                break;
            case MainAxisAlignment::End:
                leading += actual_free;
                break;
            case MainAxisAlignment::SpaceBetween:
                between_step += (n > 1 ? actual_free / static_cast<float>(n - 1) : 0.0f);
                break;
            case MainAxisAlignment::SpaceAround: {
                float per = n > 0 ? actual_free / static_cast<float>(n) : 0.0f;
                leading      += per * 0.5f;
                between_step += per;
                break;
            }
            case MainAxisAlignment::SpaceEvenly: {
                float per = actual_free / static_cast<float>(n + 1);
                leading      += per;
                between_step += per;
                break;
            }
        }

        // Compute max cross extent for sizing the container
        float max_cross_seen = 0.0f;
        for (int i = 0; i < n; ++i) {
            auto* c = children[i].get();
            float cs = crossOf(sizes[i]) + mcs(c) + mce(c);
            max_cross_seen = std::max(max_cross_seen, cs);
        }

        // Center/End must align children within our ACTUAL cross size, not the
        // full available cross. A flex container shrink-wraps its cross extent to
        // its content (max_cross_seen), clamped to the incoming min/max — which is
        // exactly what we return below. Aligning against avail_cross instead
        // double-offsets children whenever we end up narrower than the parent
        // offered (a Center column inside a full-width Stack, or the 100000
        // unbounded-cross sentinel), flinging them toward the far corner.
        const float min_cross = axis == 0 ? constraints.min_height_
                                          : constraints.min_width_;
        const float container_outer =
            std::max(min_cross, std::min(max_cross_seen + padding * 2.0f, max_cross));
        const float align_cross = std::max(0.0f, container_outer - padding * 2.0f);

        // ── Pass 4: assign offsets ────────────────────────────────────────────
        float cur_main = leading;
        for (int i = 0; i < n; ++i) {
            auto* c = children[i].get();
            float child_main  = mainOf(sizes[i]);
            float child_cross = crossOf(sizes[i]);

            float cross_pos;
            switch (cross_align) {
                case CrossAxisAlignment::Start:
                    cross_pos = padding + mcs(c);
                    break;
                case CrossAxisAlignment::Center:
                    cross_pos = padding + (align_cross - child_cross) * 0.5f;
                    break;
                case CrossAxisAlignment::End:
                    cross_pos = padding + align_cross - child_cross - mce(c);
                    break;
                case CrossAxisAlignment::Stretch:
                    cross_pos = padding + mcs(c);
                    break;
            }

            float main_pos = cur_main + mms(c);
            if (axis == 0) c->setOffset({main_pos, cross_pos});
            else           c->setOffset({cross_pos, main_pos});

            cur_main += child_main + mms(c) + mme(c) + between_step;
        }

        // Container size: main = last position − trailing between_step + end padding
        float final_main  = cur_main - between_step + padding;
        float final_cross = max_cross_seen + padding * 2.0f;

        if (axis == 0) return constraints.clamp({final_main, final_cross});
        else           return constraints.clamp({final_cross, final_main});
    }
};

class RenderBox : public RenderNode {
public:
    RenderBox(float desiredWidth, float desiredHeight) noexcept
        : desiredWidth_(desiredWidth), desiredHeight_(desiredHeight) {}
    // Buttons opt into: shrink-wrap to the child (+padding), centre the child,
    // and skip painting when the fill is fully transparent (so an unstyled
    // "transparent" toolbar button doesn't leave a shadow-pill behind).
    void setButtonLayout(bool b) noexcept { button_layout_ = b; }
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        if (button_layout_) {
            // Measure the child inside our padding.
            Size childSize{0.0f, 0.0f};
            if (!children_.empty()) {
                Constraints inner{
                    0.0f, std::max(0.0f, constraints.max_width_  - padding_ * 2.0f),
                    0.0f, std::max(0.0f, constraints.max_height_ - padding_ * 2.0f)};
                childSize = children_[0]->layout(inner);
            }
            // Shrink-wrap to content + padding unless an explicit size was set.
            // desiredHeight_ acts only as a minimum height (tap target); the
            // 100px default width is dropped so icon/clock buttons stay compact.
            // min_touch_ floors BOTH dimensions so a small icon button still
            // presents a comfortable tap target (child stays centred below).
            Size s = hasFixedSize_
                ? Size{fixedWidth_, fixedHeight_}
                : Size{std::max(childSize.width_  + padding_ * 2.0f, min_touch_),
                       std::max(std::max(childSize.height_ + padding_ * 2.0f, desiredHeight_), min_touch_)};
            s = constraints.clamp(s);
            if (!children_.empty()) {
                float ox = std::max(padding_, (s.width_  - childSize.width_)  * 0.5f);
                float oy = std::max(padding_, (s.height_ - childSize.height_) * 0.5f);
                children_[0]->setOffset({ox, oy});
            }
            return s;
        }
        // Lay out children for their side effects; each child's size is read
        // back later via getSize(), so the returned Size is intentionally dropped.
        for (auto& child : children_) (void)child->layout(constraints);
        // An explicit setFixedSize() (via apply_widget_props) overrides the
        // constructor's default box size — lets e.g. a wide button opt out of the
        // fixed 100x40 default instead of overflowing its content.
        Size s = hasFixedSize_ ? Size{fixedWidth_, fixedHeight_}
                               : Size{desiredWidth_, desiredHeight_};
        return constraints.clamp(s);
    }
    void paintSelf(fbx::xrender::DisplayList& displayList, Offset globalOffset) noexcept override {
        auto theme = fbx::xkit::ThemeManager::getInstance().getCurrentTheme();
        Color c = hasBackground_ ? backgroundColor_ : theme.surface;
        // A transparent button paints nothing — no fill and, crucially, no shadow
        // (the shadow was the source of the empty "pill" behind toolbar icons).
        if (button_layout_ && c.a <= 0.001f) return;
        float radius = borderRadius_ > 0.0f ? borderRadius_ : 12.0f;
        fbx::shared::RectData rect = {
            .x_ = globalOffset.x_, .y_ = globalOffset.y_, .width_ = size_.width_, .height_ = size_.height_,
            .r_ = c.r, .g_ = c.g, .b_ = c.b, .a_ = c.a,
            .border_radius_ = radius, .shadow_blur_ = 10.0f, .shadow_alpha_ = 0.3f
        };
        displayList.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&rect), sizeof(rect)});
    }
    void setDesiredSize(float width, float height) noexcept { desiredWidth_ = width; desiredHeight_ = height; }
private:
    float desiredWidth_, desiredHeight_;
    bool  button_layout_{false};
};

class RenderFlexColumn : public RenderNode {
public:
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        return flexLayout(children_, constraints, /*axis=*/1,
                          padding_, gap_,
                          main_axis_alignment_, cross_axis_alignment_);
    }
    void paintSelf(fbx::xrender::DisplayList& displayList, Offset globalOffset) noexcept override {
        if (hasBackground_) {
            fbx::shared::RectData rect = {
                .x_ = globalOffset.x_, .y_ = globalOffset.y_,
                .width_ = size_.width_, .height_ = size_.height_,
                .r_ = backgroundColor_.r, .g_ = backgroundColor_.g,
                .b_ = backgroundColor_.b, .a_ = backgroundColor_.a,
                .border_radius_ = borderRadius_, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
            };
            displayList.addCommand(fbx::shared::CommandType::DrawRect,
                {reinterpret_cast<const uint8_t*>(&rect), sizeof(rect)});
        }
    }
};

class RenderFlexRow : public RenderNode {
public:
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        return flexLayout(children_, constraints, /*axis=*/0,
                          padding_, gap_,
                          main_axis_alignment_, cross_axis_alignment_);
    }
    void paintSelf(fbx::xrender::DisplayList& displayList, Offset globalOffset) noexcept override {
        if (hasBackground_) {
            fbx::shared::RectData rect = {
                .x_ = globalOffset.x_, .y_ = globalOffset.y_,
                .width_ = size_.width_, .height_ = size_.height_,
                .r_ = backgroundColor_.r, .g_ = backgroundColor_.g,
                .b_ = backgroundColor_.b, .a_ = backgroundColor_.a,
                .border_radius_ = borderRadius_, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
            };
            displayList.addCommand(fbx::shared::CommandType::DrawRect,
                {reinterpret_cast<const uint8_t*>(&rect), sizeof(rect)});
        }
    }
};

class RenderText : public RenderNode {
public:
    RenderText(std::string text, float fontSize) noexcept
        : text_(std::move(text)), fontSize_(fontSize) {}

    // ── Setters ────────────────────────────────────────────────────────────
    void setText(const std::string& text)  noexcept { text_    = text; }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    void setColor(Color c)                           { customColor_ = c; hasCustomColor_ = true; }
    void setFontWeight(fbx::xrender::FontWeight w)   noexcept { font_weight_ = w; }
    void setFontStyle(fbx::xrender::FontStyle   s)   noexcept { font_style_  = s; }
    void setFontFamily(std::string family)            noexcept { font_family_ = std::move(family); }

    // ── Layout ────────────────────────────────────────────────────────────
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        float textWidth = fbx::xrender::FontManager::measureText(
            text_, fontSize_, font_family_, font_weight_, font_style_);
        return constraints.clamp({textWidth, fontSize_});
    }

    // ── Paint ─────────────────────────────────────────────────────────────
    void paintSelf(fbx::xrender::DisplayList& displayList, Offset globalOffset) noexcept override {
        auto theme = fbx::xkit::ThemeManager::getInstance().getCurrentTheme();
        Color c = hasCustomColor_ ? customColor_ : theme.text;

        fbx::shared::TextData td{};
        td.x_             = globalOffset.x_;
        td.y_             = globalOffset.y_;
        td.font_size_     = fontSize_;
        td.r_             = c.r;  td.g_ = c.g;  td.b_ = c.b;  td.a_ = c.a;
        td.string_length_ = static_cast<uint32_t>(text_.size());
        td.font_weight_   = static_cast<uint16_t>(font_weight_);
        td.font_style_    = static_cast<uint8_t>(font_style_);
        // Copy family name (null-terminated, truncated to 31 chars).
        if (!font_family_.empty()) {
            std::size_t n = std::min(font_family_.size(), std::size_t(31));
            std::memcpy(td.font_family_, font_family_.c_str(), n);
            td.font_family_[n] = '\0';
        }

        std::vector<uint8_t> payload(sizeof(td) + td.string_length_);
        std::memcpy(payload.data(), &td, sizeof(td));
        std::memcpy(payload.data() + sizeof(td), text_.data(), td.string_length_);
        displayList.addCommand(fbx::shared::CommandType::DrawText, payload);
    }

private:
    std::string text_;
    float       fontSize_;
    Color       customColor_{1, 1, 1, 1};
    bool        hasCustomColor_{false};
    fbx::xrender::FontWeight font_weight_{fbx::xrender::FontWeight::Regular};
    fbx::xrender::FontStyle  font_style_ {fbx::xrender::FontStyle::Normal};
    std::string              font_family_;
};

class RenderScrollView : public RenderNode {
public:
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        if (children_.empty()) return {0, 0};
        // Honor an explicit setFixedSize: lay the content out at that width (so
        // wrap/flow children get a real bound to work within) and clamp our own
        // box to it. Without this the scroll view balloons to its content's
        // natural width, and the declared size is silently ignored.
        float child_max_w = hasFixedSize_ ? fixedWidth_ : constraints.max_width_;
        Size childSize = children_[0]->layout({hasFixedSize_ ? fixedWidth_ : constraints.min_width_,
                                               child_max_w, 0.0f, 100000.0f});
        Size ourSize = hasFixedSize_ ? Size{fixedWidth_, fixedHeight_} : constraints.clamp(childSize);
        // Clamp scroll so we never show empty space below content.
        float max_scroll = std::max(0.0f, childSize.height_ - ourSize.height_);
        scroll_y_ = std::min(scroll_y_, max_scroll);
        children_[0]->setOffset({0.0f, -scroll_y_});
        return ourSize;
    }

    // Override paint() to bracket children with a scissor rect, then reset it.
    void paint(fbx::xrender::DisplayList& displayList, Offset parentOffset = {0,0}) noexcept override {
        if (!visible_) return;
        Offset go = {parentOffset.x_ + offset_.x_, parentOffset.y_ + offset_.y_};

        // Draw our own background panel (rounded if requested) before clipping
        // and painting the scrolling content. The base paint() we override would
        // normally do this via paintSelf, so a scroll view with a background
        // color would otherwise be invisible.
        if (hasBackground_) {
            fbx::shared::RectData bg = {
                .x_ = go.x_, .y_ = go.y_, .width_ = size_.width_, .height_ = size_.height_,
                .r_ = backgroundColor_.r, .g_ = backgroundColor_.g,
                .b_ = backgroundColor_.b, .a_ = backgroundColor_.a,
                .border_radius_ = borderRadius_, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
            };
            displayList.addCommand(fbx::shared::CommandType::DrawRect,
                {reinterpret_cast<const uint8_t*>(&bg), sizeof(bg)});
        }

        fbx::shared::ScissorData sd = {go.x_, go.y_, size_.width_, size_.height_};
        displayList.addCommand(fbx::shared::CommandType::SetScissor,
            {reinterpret_cast<const uint8_t*>(&sd), sizeof(sd)});

        for (const auto& child : children_) {
            child->paint(displayList, go);
        }

        displayList.addCommand(fbx::shared::CommandType::ResetScissor, {});
    }

    void paintSelf(fbx::xrender::DisplayList&, Offset) noexcept override {}

    void setScrollY(float y) noexcept { scroll_y_ = std::max(0.0f, y); }
    [[nodiscard]] float getScrollY() const noexcept { return scroll_y_; }
private:
    float scroll_y_{0.0f};
};

// Horizontal counterpart of RenderScrollView: lays its single child out at its
// natural (unbounded) width, clamps its own box to the available width, and
// translates the child by -scroll_x_ (scissor-clipped). Height passes through so
// an outer vertical scroll still works. Used by TableWidget for wide tables.
class RenderHScroll : public RenderNode {
public:
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        if (children_.empty()) return {0, 0};
        Size childSize = children_[0]->layout({0.0f, 100000.0f,
                                               constraints.min_height_, constraints.max_height_});
        Size ourSize{constraints.max_width_, childSize.height_};
        ourSize = constraints.clamp(ourSize);
        float max_scroll = std::max(0.0f, childSize.width_ - ourSize.width_);
        scroll_x_ = std::min(scroll_x_, max_scroll);
        children_[0]->setOffset({-scroll_x_, 0.0f});
        return ourSize;
    }

    void paint(fbx::xrender::DisplayList& displayList, Offset parentOffset = {0,0}) noexcept override {
        if (!visible_) return;
        Offset go = {parentOffset.x_ + offset_.x_, parentOffset.y_ + offset_.y_};
        fbx::shared::ScissorData sd = {go.x_, go.y_, size_.width_, size_.height_};
        displayList.addCommand(fbx::shared::CommandType::SetScissor,
            {reinterpret_cast<const uint8_t*>(&sd), sizeof(sd)});
        for (const auto& child : children_) child->paint(displayList, go);
        displayList.addCommand(fbx::shared::CommandType::ResetScissor, {});
    }
    void paintSelf(fbx::xrender::DisplayList&, Offset) noexcept override {}

    void setScrollX(float x) noexcept { scroll_x_ = std::max(0.0f, x); }
    [[nodiscard]] float getScrollX() const noexcept { return scroll_x_; }
private:
    float scroll_x_{0.0f};
};

class RenderImage : public RenderNode {
public:
    RenderImage(std::vector<uint8_t> pixels, uint32_t pw, uint32_t ph, float width, float height,
                fbx::xkit::ImageFitMode fit = fbx::xkit::ImageFitMode::Stretch) noexcept
        : pixels_(std::move(pixels)), pixelWidth_(pw), pixelHeight_(ph),
          width_(width), height_(height), fitMode_(fit) {}

    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        if (fitMode_ == fbx::xkit::ImageFitMode::Contain && pixelWidth_ > 0 && pixelHeight_ > 0) {
            // Scale to fit within constraints, preserving aspect ratio
            float scale = std::min(constraints.max_width_ / pixelWidth_,
                                   constraints.max_height_ / pixelHeight_);
            return constraints.clamp({pixelWidth_ * scale, pixelHeight_ * scale});
        }
        if (fitMode_ == fbx::xkit::ImageFitMode::Center && pixelWidth_ > 0) {
            return constraints.clamp({static_cast<float>(pixelWidth_),
                                      static_cast<float>(pixelHeight_)});
        }
        return constraints.clamp({width_, height_});
    }

    void paintSelf(fbx::xrender::DisplayList& displayList, Offset globalOffset) noexcept override {
        float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
        if (fitMode_ == fbx::xkit::ImageFitMode::Cover &&
                pixelWidth_ > 0 && pixelHeight_ > 0 &&
                size_.width_ > 0 && size_.height_ > 0) {
            float img_asp  = static_cast<float>(pixelWidth_) / pixelHeight_;
            float disp_asp = size_.width_ / size_.height_;
            if (img_asp > disp_asp) {
                // Image wider than display — crop left/right
                float crop = disp_asp / img_asp;
                u0 = (1.0f - crop) * 0.5f;
                u1 = 1.0f - u0;
            } else {
                // Image taller than display — crop top/bottom
                float crop = img_asp / disp_asp;
                v0 = (1.0f - crop) * 0.5f;
                v1 = 1.0f - v0;
            }
        }
        fbx::shared::ImageData imgData = {
            .x_ = globalOffset.x_, .y_ = globalOffset.y_,
            .width_ = size_.width_, .height_ = size_.height_,
            .texture_id_ = 0,
            .pixel_ptr_ = pixels_.data(),
            .pixel_width_ = pixelWidth_,
            .pixel_height_ = pixelHeight_,
            .uv_x0_ = u0, .uv_y0_ = v0,
            .uv_x1_ = u1, .uv_y1_ = v1
        };
        displayList.addCommand(fbx::shared::CommandType::DrawImage,
            {reinterpret_cast<const uint8_t*>(&imgData), sizeof(imgData)});
    }
    const std::vector<uint8_t>& getPixels() const { return pixels_; }
    uint32_t getPixelWidth() const { return pixelWidth_; }
    uint32_t getPixelHeight() const { return pixelHeight_; }
private:
    std::vector<uint8_t>    pixels_;
    uint32_t                pixelWidth_, pixelHeight_;
    float                   width_, height_;
    fbx::xkit::ImageFitMode fitMode_;
};

class RenderWrapRow : public RenderNode {
public:
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        const float avail = constraints.max_width_;
        // Phase 1 — measure every child and break them into rows, tracking the
        // content width each row actually uses.
        std::vector<Size> sizes(children_.size());
        std::vector<size_t> rowStarts;   // index of first child in each row
        std::vector<float>  rowWidths;   // content width (no leading/trailing pad)
        std::vector<float>  rowHeights;
        float currentX = padding_, maxRowHeight = 0.0f;
        size_t rowStart = 0;
        for (size_t i = 0; i < children_.size(); ++i) {
            Size cs = children_[i]->layout({0, avail - (padding_ * 2), 0, constraints.max_height_});
            sizes[i] = cs;
            if (currentX + cs.width_ > avail - padding_ && currentX > padding_) {
                rowStarts.push_back(rowStart);
                rowWidths.push_back(currentX - padding_ - padding_); // strip trailing gap + leading pad
                rowHeights.push_back(maxRowHeight);
                rowStart = i;
                currentX = padding_;
                maxRowHeight = 0.0f;
            }
            currentX += cs.width_ + padding_;
            maxRowHeight = std::max(maxRowHeight, cs.height_);
        }
        rowStarts.push_back(rowStart);
        rowWidths.push_back(currentX - padding_ - padding_);
        rowHeights.push_back(maxRowHeight);

        // Phase 2 — place children, centering each row within the available width.
        float currentY = padding_;
        for (size_t r = 0; r < rowStarts.size(); ++r) {
            size_t begin = rowStarts[r];
            size_t end   = (r + 1 < rowStarts.size()) ? rowStarts[r + 1] : children_.size();
            float slack  = std::max(0.0f, (avail - padding_ * 2) - rowWidths[r]);
            float x = padding_ + slack * 0.5f;
            for (size_t i = begin; i < end; ++i) {
                children_[i]->setOffset({x, currentY});
                x += sizes[i].width_ + padding_;
            }
            currentY += rowHeights[r] + padding_;
        }
        return constraints.clamp({avail, currentY + padding_});
    }
    void paintSelf(fbx::xrender::DisplayList& displayList, Offset globalOffset) noexcept override {
        if (hasBackground_) {
            fbx::shared::RectData rect = {
                .x_ = globalOffset.x_, .y_ = globalOffset.y_, .width_ = size_.width_, .height_ = size_.height_,
                .r_ = backgroundColor_.r, .g_ = backgroundColor_.g, .b_ = backgroundColor_.b, .a_ = backgroundColor_.a,
                .border_radius_ = borderRadius_, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
            };
            displayList.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&rect), sizeof(rect)});
        }
    }
};

class RenderSpacer : public RenderNode {
public:
    // flex=true → participate in flex free-space distribution (flex_grow=1)
    RenderSpacer(float w, float h, bool flex = false) : w_(w), h_(h) {
        if (flex) flex_grow_ = 1.0f;
    }
    [[nodiscard]] Size performLayout(const Constraints& c) noexcept override { return c.clamp({w_, h_}); }
    void paintSelf(fbx::xrender::DisplayList&, Offset) noexcept override {}
private:
    float w_, h_;
};

class RenderStack : public RenderNode {
public:
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        Size maxSize = {0, 0};
        // Pass LOOSE constraints to children (min 0). Tight constraints would
        // clamp a fixed-size child UP to the stack's full size (see
        // RenderNode::layout), forcing menus/panels/the launcher to fill the
        // screen instead of keeping their declared size. Flexible children (the
        // desktop) still expand to the max and fill as before.
        Constraints childC{0.0f, constraints.max_width_, 0.0f, constraints.max_height_};
        for (auto& child : children_) {
            Size s = child->layout(childC);
            maxSize.width_ = std::max(maxSize.width_, s.width_);
            maxSize.height_ = std::max(maxSize.height_, s.height_);

            // Resolve this child's anchor: an explicit StackAnchor wins; otherwise
            // inherit the stack's alignment (Center → centred, else top-left).
            StackAnchor anchor = child->stackAnchor();
            if (anchor == StackAnchor::Inherit)
                anchor = (alignment_ == Alignment::Center) ? StackAnchor::Center
                                                           : StackAnchor::TopLeft;
            const float freeW = constraints.max_width_  - s.width_;
            const float freeH = constraints.max_height_ - s.height_;
            // Corner anchors inset by the child's margin so panels can float clear
            // of the screen edge (matching the taskbar's inset); Center ignores it.
            const float ml = child->marginLeft(),  mr = child->marginRight();
            const float mt = child->marginTop(),   mb = child->marginBottom();
            float xOff = 0, yOff = 0;
            switch (anchor) {
                case StackAnchor::Center:      xOff = freeW / 2.0f;  yOff = freeH / 2.0f;  break;
                case StackAnchor::TopRight:    xOff = freeW - mr;    yOff = mt;            break;
                case StackAnchor::BottomLeft:  xOff = ml;            yOff = freeH - mb;    break;
                case StackAnchor::BottomRight: xOff = freeW - mr;    yOff = freeH - mb;    break;
                case StackAnchor::TopLeft:
                case StackAnchor::Inherit:     xOff = ml;            yOff = mt;            break;
            }
            child->setOffset({xOff, yOff});
        }
        return constraints.clamp(maxSize);
    }
    void paintSelf(fbx::xrender::DisplayList& displayList, Offset globalOffset) noexcept override {
        if (hasBackground_) {
            fbx::shared::RectData rect = {
                .x_ = globalOffset.x_, .y_ = globalOffset.y_, .width_ = size_.width_, .height_ = size_.height_,
                .r_ = backgroundColor_.r, .g_ = backgroundColor_.g, .b_ = backgroundColor_.b, .a_ = backgroundColor_.a,
                .border_radius_ = borderRadius_, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
            };
            displayList.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&rect), sizeof(rect)});
        }
    }
};

// RenderAdaptive — picks ONE child by available width (responsive layout). The
// child with the largest min-width <= the constraint width is laid out, sized,
// painted, hit-tested and sent events; the rest are collapsed to 0×0 and hidden,
// so the base paint/hitTest/spatial-event machinery skips them automatically.
class RenderAdaptive : public RenderNode {
public:
    void setMinWidths(std::vector<float> mw) noexcept { min_widths_ = std::move(mw); }
    [[nodiscard]] int activeIndex() const noexcept { return active_; }

    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        if (children_.empty()) return {0, 0};
        active_ = pickActive(constraints.max_width_);
        Size result{0, 0};
        for (size_t i = 0; i < children_.size(); ++i) {
            if (static_cast<int>(i) == active_) {
                children_[i]->setVisibility(true);
                children_[i]->setOffset({0, 0});
                result = children_[i]->layout(constraints);
            } else {
                children_[i]->setVisibility(false);
                (void)children_[i]->layout(Constraints::tight({0, 0}));  // collapse
            }
        }
        return result;
    }
    void paintSelf(fbx::xrender::DisplayList&, Offset) noexcept override {}

private:
    [[nodiscard]] int pickActive(float avail) const noexcept {
        int best = 0; float best_mw = -1.0f;
        for (size_t i = 0; i < min_widths_.size() && i < children_.size(); ++i) {
            if (min_widths_[i] <= avail && min_widths_[i] >= best_mw) {
                best_mw = min_widths_[i]; best = static_cast<int>(i);
            }
        }
        return best;
    }
    std::vector<float> min_widths_;
    int active_{0};
};

// Root overlay layer: child[0] is the app content (laid out to fill the
// viewport); children[1..] are popups/menus/dialogs placed above it at
// anchor-relative positions. The element sets one `Placement` per overlay child
// each frame; performLayout measures each overlay then positions it via
// overlay::placePopup and records the resolved bounds for hit-testing.
class RenderOverlayHost : public RenderNode {
public:
    struct Placement { overlay::Rect anchor; overlay::Side side; bool centered; bool bottom; bool modal; };
    std::vector<Placement>     placement_;   // one per overlay child (children_[1..])
    std::vector<overlay::Rect> resolved_;     // resolved bounds, read back by element

    [[nodiscard]] Size performLayout(const Constraints& c) noexcept override {
        Size vp{c.max_width_, c.max_height_};
        resolved_.clear();
        if (!children_.empty()) {
            (void)children_[0]->layout(Constraints::tight(vp));  // content fills viewport
            children_[0]->setOffset({0.0f, 0.0f});
        }
        for (size_t i = 1; i < children_.size(); ++i) {
            Size sz = children_[i]->layout({0.0f, vp.width_, 0.0f, vp.height_});  // loose
            Placement pl{};
            pl.side = overlay::Side::Below;
            if (i - 1 < placement_.size()) pl = placement_[i - 1];
            float x, y;
            if (pl.centered) {   // dialogs: centre in the viewport, ignore anchor
                x = overlay::clampf((vp.width_  - sz.width_)  * 0.5f, 0.0f, vp.width_  - sz.width_);
                y = overlay::clampf((vp.height_ - sz.height_) * 0.5f, 0.0f, vp.height_ - sz.height_);
            } else if (pl.bottom) {   // toasts: bottom-centre with a small margin
                constexpr float kMargin = 24.0f;
                x = overlay::clampf((vp.width_ - sz.width_) * 0.5f, 0.0f, vp.width_ - sz.width_);
                y = overlay::clampf(vp.height_ - sz.height_ - kMargin, 0.0f, vp.height_ - sz.height_);
            } else {
                auto p = overlay::placePopup(pl.anchor, sz.width_, sz.height_, vp.width_, vp.height_, pl.side);
                x = p.x; y = p.y;
            }
            children_[i]->setOffset({x, y});
            resolved_.push_back({x, y, sz.width_, sz.height_});
        }
        return c.clamp(vp);
    }

    // Paint content, then a dim scrim under any modal overlay, then the overlays.
    void paint(fbx::xrender::DisplayList& dl, Offset parentOffset = {0, 0}) noexcept override {
        if (!visible_) return;
        Offset go{parentOffset.x_ + offset_.x_, parentOffset.y_ + offset_.y_};
        if (!children_.empty()) children_[0]->paint(dl, go);   // app content

        bool modal = false;
        for (const auto& pl : placement_) modal = modal || pl.modal;
        if (modal) {
            fbx::shared::RectData scrim = {
                .x_ = go.x_, .y_ = go.y_, .width_ = size_.width_, .height_ = size_.height_,
                .r_ = 0.0f, .g_ = 0.0f, .b_ = 0.0f, .a_ = 0.45f,
                .border_radius_ = 0.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
            };
            dl.addCommand(fbx::shared::CommandType::DrawRect,
                          {reinterpret_cast<const uint8_t*>(&scrim), sizeof(scrim)});
        }
        for (size_t i = 1; i < children_.size(); ++i) children_[i]->paint(dl, go);
    }
    void paintSelf(fbx::xrender::DisplayList&, Offset) noexcept override {}
};

// Transparent single-child node that carries flex child properties.
// The parent flex container reads flexGrow() / flexBasis() from this node
// and allocates the appropriate main-axis space; the inner child fills it.
class RenderFlexible : public RenderNode {
public:
    explicit RenderFlexible(float grow = 1.0f,
                            float shrink = 1.0f,
                            float basis  = -1.0f) noexcept {
        flex_grow_   = grow;
        flex_shrink_ = shrink;
        flex_basis_  = basis;
    }
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        if (children_.empty()) return constraints.clamp({0.0f, 0.0f});
        Size s = children_[0]->layout(constraints);
        children_[0]->setOffset({0.0f, 0.0f});
        return s;
    }
    void paintSelf(fbx::xrender::DisplayList&, Offset) noexcept override {}
};

class RenderCheckbox : public RenderNode {
public:
    RenderCheckbox() noexcept { setFixedSize(20.0f, 20.0f); }
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        return constraints.clamp({20.0f, 20.0f});
    }
    void paintSelf(fbx::xrender::DisplayList& dl, Offset go) noexcept override {
        auto theme = fbx::xkit::ThemeManager::getInstance().getCurrentTheme();
        // Outer border rect
        fbx::shared::RectData border = {
            .x_ = go.x_, .y_ = go.y_,
            .width_ = 20.0f, .height_ = 20.0f,
            .r_ = theme.text.r, .g_ = theme.text.g, .b_ = theme.text.b, .a_ = theme.text.a,
            .border_radius_ = 4.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
        };
        dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&border), sizeof(border)});
        // Inner surface (clear the interior)
        fbx::shared::RectData inner_bg = {
            .x_ = go.x_ + 3.0f, .y_ = go.y_ + 3.0f,
            .width_ = 14.0f, .height_ = 14.0f,
            .r_ = theme.surface.r, .g_ = theme.surface.g, .b_ = theme.surface.b, .a_ = theme.surface.a,
            .border_radius_ = 2.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
        };
        dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&inner_bg), sizeof(inner_bg)});
        if (checked_) {
            fbx::shared::RectData fill = {
                .x_ = go.x_ + 4.0f, .y_ = go.y_ + 4.0f,
                .width_ = 12.0f, .height_ = 12.0f,
                .r_ = theme.accent.r, .g_ = theme.accent.g, .b_ = theme.accent.b, .a_ = theme.accent.a,
                .border_radius_ = 2.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
            };
            dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&fill), sizeof(fill)});
        }
    }
    void setChecked(bool c) noexcept { checked_ = c; }
    bool checked() const noexcept { return checked_; }
private:
    bool checked_{false};
};

class RenderSwitch : public RenderNode {
public:
    RenderSwitch(bool value) noexcept : value_(value), thumb_x_(value ? 22.0f : 2.0f) {
        setFixedSize(44.0f, 24.0f);
    }
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        return constraints.clamp({44.0f, 24.0f});
    }
    void paintSelf(fbx::xrender::DisplayList& dl, Offset go) noexcept override {
        auto theme = fbx::xkit::ThemeManager::getInstance().getCurrentTheme();
        // Animate thumb_x_ toward target
        float target = value_ ? 22.0f : 2.0f;
        thumb_x_ += (target - thumb_x_) * 0.3f;

        // Track
        Color track_color = value_ ? theme.accent : Color{0.7f, 0.7f, 0.7f, 1.0f};
        fbx::shared::RectData track = {
            .x_ = go.x_, .y_ = go.y_,
            .width_ = 44.0f, .height_ = 24.0f,
            .r_ = track_color.r, .g_ = track_color.g, .b_ = track_color.b, .a_ = track_color.a,
            .border_radius_ = 12.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
        };
        dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&track), sizeof(track)});
        // Thumb
        fbx::shared::RectData thumb = {
            .x_ = go.x_ + thumb_x_, .y_ = go.y_ + 2.0f,
            .width_ = 20.0f, .height_ = 20.0f,
            .r_ = 1.0f, .g_ = 1.0f, .b_ = 1.0f, .a_ = 1.0f,
            .border_radius_ = 10.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
        };
        dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&thumb), sizeof(thumb)});
    }
    void toggle() noexcept { value_ = !value_; }
    void setValue(bool v) noexcept { value_ = v; }
    bool value() const noexcept { return value_; }
private:
    bool value_;
    mutable float thumb_x_;
};

class RenderSlider : public RenderNode {
public:
    RenderSlider(float value, float min_val, float max_val) noexcept
        : value_(value), min_val_(min_val), max_val_(max_val) {}
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        width_ = constraints.max_width_;
        return constraints.clamp({width_, 28.0f});
    }
    void paintSelf(fbx::xrender::DisplayList& dl, Offset go) noexcept override {
        auto theme = fbx::xkit::ThemeManager::getInstance().getCurrentTheme();
        float track_left = go.x_ + 14.0f;
        float track_width = width_ - 28.0f;
        float track_y = go.y_ + 12.0f;
        float range = max_val_ - min_val_;
        float t = (range > 0.0f) ? (value_ - min_val_) / range : 0.0f;
        float thumb_cx = track_left + t * track_width;

        // Background track
        fbx::shared::RectData bg = {
            .x_ = track_left, .y_ = track_y,
            .width_ = track_width, .height_ = 4.0f,
            .r_ = 0.8f, .g_ = 0.8f, .b_ = 0.8f, .a_ = 1.0f,
            .border_radius_ = 2.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
        };
        dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&bg), sizeof(bg)});
        // Fill
        if (thumb_cx > track_left) {
            fbx::shared::RectData fill = {
                .x_ = track_left, .y_ = track_y,
                .width_ = thumb_cx - track_left, .height_ = 4.0f,
                .r_ = theme.accent.r, .g_ = theme.accent.g, .b_ = theme.accent.b, .a_ = theme.accent.a,
                .border_radius_ = 2.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
            };
            dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&fill), sizeof(fill)});
        }
        // Thumb
        fbx::shared::RectData thumb = {
            .x_ = thumb_cx - 10.0f, .y_ = go.y_ + 4.0f,
            .width_ = 20.0f, .height_ = 20.0f,
            .r_ = 1.0f, .g_ = 1.0f, .b_ = 1.0f, .a_ = 1.0f,
            .border_radius_ = 10.0f, .shadow_blur_ = 4.0f, .shadow_alpha_ = 0.3f
        };
        dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&thumb), sizeof(thumb)});
    }
    void setValue(float v) noexcept { value_ = std::clamp(v, min_val_, max_val_); }
    float value() const noexcept { return value_; }
    float minVal() const noexcept { return min_val_; }
    float maxVal() const noexcept { return max_val_; }
    float width() const noexcept { return width_; }
    void startDrag() noexcept { dragging_ = true; }
    void endDrag() noexcept { dragging_ = false; }
    bool dragging() const noexcept { return dragging_; }
    float computeValue(float x) const noexcept {
        float track_left = 14.0f;
        float track_width = width_ - 28.0f;
        float t = (track_width > 0.0f) ? (x - track_left) / track_width : 0.0f;
        return std::clamp(min_val_ + t * (max_val_ - min_val_), min_val_, max_val_);
    }
private:
    float value_, min_val_, max_val_;
    float width_{200.0f};
    bool dragging_{false};
};

class RenderProgressBar : public RenderNode {
public:
    explicit RenderProgressBar(float value) noexcept : value_(value) {}
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        return constraints.clamp({constraints.max_width_, 6.0f});
    }
    void paintSelf(fbx::xrender::DisplayList& dl, Offset go) noexcept override {
        auto theme = fbx::xkit::ThemeManager::getInstance().getCurrentTheme();
        float w = size_.width_;
        // Background track
        fbx::shared::RectData bg = {
            .x_ = go.x_, .y_ = go.y_,
            .width_ = w, .height_ = 6.0f,
            .r_ = 0.8f, .g_ = 0.8f, .b_ = 0.8f, .a_ = 1.0f,
            .border_radius_ = 3.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
        };
        dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&bg), sizeof(bg)});
        // Fill
        float fill_w = w * value_;
        if (fill_w > 0.0f) {
            fbx::shared::RectData fill = {
                .x_ = go.x_, .y_ = go.y_,
                .width_ = fill_w, .height_ = 6.0f,
                .r_ = theme.accent.r, .g_ = theme.accent.g, .b_ = theme.accent.b, .a_ = theme.accent.a,
                .border_radius_ = 3.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
            };
            dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&fill), sizeof(fill)});
        }
    }
    void setValue(float v) noexcept { value_ = std::clamp(v, 0.0f, 1.0f); }
    float value() const noexcept { return value_; }
private:
    float value_;
};

// An indeterminate activity spinner: `count` dots arranged in a ring, each
// faded by how far a rotating "head" has moved past it (a comet tail). phase_ is
// the head position in [0,count) and is advanced from wall-clock time by
// SpinnerElement each frame. No rotation transform — per-dot opacity is a pure
// function of phase, so the look is fully determined by phase_ and testable.
class RenderSpinner : public RenderNode {
public:
    RenderSpinner(float diameter, int count, Color color) noexcept
        : diameter_(diameter), count_(count < 1 ? 1 : count), color_(color) {}

    [[nodiscard]] Size performLayout(const Constraints& c) noexcept override {
        return c.clamp({diameter_, diameter_});
    }

    void  setPhase(float p) noexcept { phase_ = p; }
    [[nodiscard]] float phase()    const noexcept { return phase_; }
    void  setColor(Color c) noexcept { color_ = c; }
    [[nodiscard]] int   dotCount() const noexcept { return count_; }

    // Brightness of dot i for a continuous head position in [0,count). The head
    // dot is brightest (→1.0); dots fade toward minA the farther the head is
    // *ahead* of them, so the bright comet tail trails behind the head. Pure.
    static float dotAlpha(int i, float head, int count, float minA = kMinAlpha) noexcept {
        if (count < 1) return 1.0f;
        float n    = static_cast<float>(count);
        float lead = std::fmod((head - static_cast<float>(i)) + n, n);   // 0..count
        float t    = 1.0f - lead / n;                                    // 1 at head → ~0 ahead
        return minA + (1.0f - minA) * t;
    }

    void paintSelf(fbx::xrender::DisplayList& dl, Offset go) noexcept override {
        const float cx    = go.x_ + size_.width_  * 0.5f;
        const float cy    = go.y_ + size_.height_ * 0.5f;
        const float dotR  = diameter_ * 0.11f;
        const float ringR = size_.width_ * 0.5f - dotR;
        constexpr float kPi = 3.14159265358979323846f;
        for (int i = 0; i < count_; ++i) {
            float ang = -kPi * 0.5f + 2.0f * kPi * static_cast<float>(i) / static_cast<float>(count_);
            float dx  = cx + ringR * std::cos(ang);
            float dy  = cy + ringR * std::sin(ang);
            fbx::shared::RectData dot = {
                .x_ = dx - dotR, .y_ = dy - dotR,
                .width_ = dotR * 2.0f, .height_ = dotR * 2.0f,
                .r_ = color_.r, .g_ = color_.g, .b_ = color_.b,
                .a_ = color_.a * dotAlpha(i, phase_, count_),
                .border_radius_ = dotR, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
            };
            dl.addCommand(fbx::shared::CommandType::DrawRect,
                {reinterpret_cast<const uint8_t*>(&dot), sizeof(dot)});
        }
    }

private:
    static constexpr float kMinAlpha = 0.15f;
    float diameter_;
    int   count_;
    Color color_;
    float phase_{0.0f};
};

class RenderRadioButton : public RenderNode {
public:
    RenderRadioButton() noexcept { setFixedSize(20.0f, 20.0f); }
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        return constraints.clamp({20.0f, 20.0f});
    }
    void paintSelf(fbx::xrender::DisplayList& dl, Offset go) noexcept override {
        auto theme = fbx::xkit::ThemeManager::getInstance().getCurrentTheme();
        // Outer circle border
        fbx::shared::RectData outer = {
            .x_ = go.x_, .y_ = go.y_,
            .width_ = 20.0f, .height_ = 20.0f,
            .r_ = theme.text.r, .g_ = theme.text.g, .b_ = theme.text.b, .a_ = theme.text.a,
            .border_radius_ = 10.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
        };
        dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&outer), sizeof(outer)});
        // Inner surface (clears the border interior)
        fbx::shared::RectData inner_bg = {
            .x_ = go.x_ + 2.0f, .y_ = go.y_ + 2.0f,
            .width_ = 16.0f, .height_ = 16.0f,
            .r_ = theme.surface.r, .g_ = theme.surface.g, .b_ = theme.surface.b, .a_ = theme.surface.a,
            .border_radius_ = 8.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
        };
        dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&inner_bg), sizeof(inner_bg)});
        if (selected_) {
            fbx::shared::RectData dot = {
                .x_ = go.x_ + 5.0f, .y_ = go.y_ + 5.0f,
                .width_ = 10.0f, .height_ = 10.0f,
                .r_ = theme.accent.r, .g_ = theme.accent.g, .b_ = theme.accent.b, .a_ = theme.accent.a,
                .border_radius_ = 5.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
            };
            dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&dot), sizeof(dot)});
        }
    }
    void setSelected(bool s) noexcept { selected_ = s; }
    bool selected() const noexcept { return selected_; }
private:
    bool selected_{false};
};

class RenderCard : public RenderNode {
public:
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        if (!children_.empty()) {
            float pad = 16.0f;
            Constraints child_c = {
                0.0f, std::max(0.0f, constraints.max_width_ - pad * 2.0f),
                0.0f, std::max(0.0f, constraints.max_height_ - pad * 2.0f)
            };
            Size child_size = children_[0]->layout(child_c);
            children_[0]->setOffset({pad, pad});
            Size desired = {child_size.width_ + pad * 2.0f, child_size.height_ + pad * 2.0f};
            return constraints.clamp(desired);
        }
        return constraints.clamp({0.0f, 0.0f});
    }
    void paintSelf(fbx::xrender::DisplayList& dl, Offset go) noexcept override {
        auto theme = fbx::xkit::ThemeManager::getInstance().getCurrentTheme();
        // Shadow
        fbx::shared::RectData shadow = {
            .x_ = go.x_ + 2.0f, .y_ = go.y_ + 4.0f,
            .width_ = size_.width_, .height_ = size_.height_,
            .r_ = 0.0f, .g_ = 0.0f, .b_ = 0.0f, .a_ = 0.15f,
            .border_radius_ = 12.0f, .shadow_blur_ = 8.0f, .shadow_alpha_ = 0.15f
        };
        dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&shadow), sizeof(shadow)});
        // Surface
        fbx::shared::RectData card = {
            .x_ = go.x_, .y_ = go.y_,
            .width_ = size_.width_, .height_ = size_.height_,
            .r_ = theme.surface.r, .g_ = theme.surface.g, .b_ = theme.surface.b, .a_ = theme.surface.a,
            .border_radius_ = 12.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
        };
        dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&card), sizeof(card)});
    }
};

// A splitter handle between two flex panes: a bar that fills the cross axis and
// is `thickness` px along the main axis. `vertical` = a vertical bar between
// left/right panes; otherwise a horizontal bar between top/bottom panes.
class RenderSplitter : public RenderNode {
public:
    RenderSplitter(bool vertical, float thickness) noexcept
        : vertical_(vertical), thickness_(thickness) {}
    [[nodiscard]] Size performLayout(const Constraints& c) noexcept override {
        return vertical_ ? c.clamp({thickness_, c.max_height_})
                         : c.clamp({c.max_width_, thickness_});
    }
    void paintSelf(fbx::xrender::DisplayList& dl, Offset go) noexcept override {
        auto col = fbx::xkit::ThemeManager::getInstance().getCurrentTheme().text;
        fbx::shared::RectData r = {
            .x_ = go.x_, .y_ = go.y_,
            .width_ = size_.width_, .height_ = size_.height_,
            .r_ = col.r, .g_ = col.g, .b_ = col.b, .a_ = 0.18f,
            .border_radius_ = 0.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
        };
        dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&r), sizeof(r)});
    }
private:
    bool vertical_;
    float thickness_;
};

class RenderDivider : public RenderNode {
public:
    explicit RenderDivider(float thickness) noexcept : thickness_(thickness) {}
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        return constraints.clamp({constraints.max_width_, thickness_});
    }
    void paintSelf(fbx::xrender::DisplayList& dl, Offset go) noexcept override {
        auto c = fbx::xkit::ThemeManager::getInstance().getCurrentTheme().text;
        fbx::shared::RectData rect = {
            .x_ = go.x_, .y_ = go.y_,
            .width_ = size_.width_, .height_ = thickness_,
            .r_ = c.r, .g_ = c.g, .b_ = c.b, .a_ = 0.12f, // subtle tint of text color
            .border_radius_ = 0.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
        };
        dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&rect), sizeof(rect)});
    }
private:
    float thickness_;
};

class RenderListTile : public RenderNode {
public:
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        float w = constraints.max_width_;
        float content_x_start = has_leading_ ? 72.0f : 16.0f;
        float content_width = w - content_x_start - (has_trailing_ ? 56.0f : 16.0f);

        float title_h = 0.0f, subtitle_h = 0.0f;
        int ci = 0;
        if (has_leading_ && ci < static_cast<int>(children_.size())) {
            (void)children_[ci]->layout({0.0f, 40.0f, 0.0f, 40.0f});
            ci++;
        }
        if (has_title_ && ci < static_cast<int>(children_.size())) {
            Size s = children_[ci]->layout({0.0f, content_width, 0.0f, 40.0f});
            title_h = s.height_;
            ci++;
        }
        if (has_subtitle_ && ci < static_cast<int>(children_.size())) {
            // Any widget may be a subtitle (a segmented control, a field), not
            // just a 12 px line: a 30 px cap squeezed a 40 px control into a
            // slot it overflowed, drawing it over the title.
            Size s = children_[ci]->layout({0.0f, content_width, 0.0f, 400.0f});
            subtitle_h = s.height_;
            ci++;
        }
        if (has_trailing_ && ci < static_cast<int>(children_.size())) {
            (void)children_[ci]->layout({0.0f, 40.0f, 0.0f, 40.0f});
            ci++;
        }

        float text_block_h = title_h + subtitle_h;
        float total_h = std::max(56.0f, text_block_h + 16.0f);
        float text_start_y = (total_h - text_block_h) / 2.0f;

        ci = 0;
        if (has_leading_ && ci < static_cast<int>(children_.size())) {
            float lh = children_[ci]->getSize().height_;
            children_[ci]->setOffset({16.0f, (total_h - lh) / 2.0f});
            ci++;
        }
        if (has_title_ && ci < static_cast<int>(children_.size())) {
            children_[ci]->setOffset({content_x_start, text_start_y});
            ci++;
        }
        if (has_subtitle_ && ci < static_cast<int>(children_.size())) {
            children_[ci]->setOffset({content_x_start, text_start_y + title_h});
            ci++;
        }
        if (has_trailing_ && ci < static_cast<int>(children_.size())) {
            float th = children_[ci]->getSize().height_;
            children_[ci]->setOffset({w - 56.0f, (total_h - th) / 2.0f});
            ci++;
        }
        return constraints.clamp({w, total_h});
    }
    void paintSelf(fbx::xrender::DisplayList& dl, Offset go) noexcept override {
        if (pressed_) {
            fbx::shared::RectData highlight = {
                .x_ = go.x_, .y_ = go.y_,
                .width_ = size_.width_, .height_ = size_.height_,
                .r_ = 0.5f, .g_ = 0.5f, .b_ = 0.5f, .a_ = 0.1f,
                .border_radius_ = 0.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
            };
            dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&highlight), sizeof(highlight)});
        }
    }
    void setHasLeading(bool v) noexcept { has_leading_ = v; }
    void setHasTitle(bool v) noexcept { has_title_ = v; }
    void setHasSubtitle(bool v) noexcept { has_subtitle_ = v; }
    void setHasTrailing(bool v) noexcept { has_trailing_ = v; }
    void setPressed(bool v) noexcept { pressed_ = v; }
    bool pressed() const noexcept { return pressed_; }
private:
    bool has_leading_{false}, has_title_{false}, has_subtitle_{false}, has_trailing_{false};
    bool pressed_{false};
};

class RenderBadge : public RenderNode {
public:
    explicit RenderBadge(int count) noexcept : count_(count) {}
    [[nodiscard]] Size performLayout(const Constraints& constraints) noexcept override {
        if (!children_.empty()) {
            Size s = children_[0]->layout(constraints);
            children_[0]->setOffset({0.0f, 0.0f});
            return constraints.clamp(s);
        }
        return constraints.clamp({0.0f, 0.0f});
    }
    void paintSelf(fbx::xrender::DisplayList& dl, Offset go) noexcept override {
        if (count_ <= 0) return;
        auto theme = fbx::xkit::ThemeManager::getInstance().getCurrentTheme();
        float bx = go.x_ + size_.width_ - 14.0f;
        float by = go.y_ - 4.0f;
        // Badge circle
        fbx::shared::RectData badge = {
            .x_ = bx, .y_ = by,
            .width_ = 18.0f, .height_ = 18.0f,
            .r_ = theme.accent.r, .g_ = theme.accent.g, .b_ = theme.accent.b, .a_ = theme.accent.a,
            .border_radius_ = 9.0f, .shadow_blur_ = 0.0f, .shadow_alpha_ = 0.0f
        };
        dl.addCommand(fbx::shared::CommandType::DrawRect, {reinterpret_cast<const uint8_t*>(&badge), sizeof(badge)});
        // Count text
        std::string count_str = std::to_string(count_);
        fbx::shared::TextData td = {
            .x_ = bx + 3.0f, .y_ = by + 2.0f,
            .font_size_ = 11.0f,
            .r_ = 1.0f, .g_ = 1.0f, .b_ = 1.0f, .a_ = 1.0f,
            .string_length_ = static_cast<uint32_t>(count_str.length())
        };
        std::vector<uint8_t> payload(sizeof(td) + td.string_length_);
        std::memcpy(payload.data(), &td, sizeof(td));
        std::memcpy(payload.data() + sizeof(td), count_str.data(), td.string_length_);
        dl.addCommand(fbx::shared::CommandType::DrawText, payload);
    }
    void setCount(int c) noexcept { count_ = c; }
    int count() const noexcept { return count_; }
private:
    int count_;
};

} // namespace fbx::xkit
