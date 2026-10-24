// Widget rendering verification: build a widget → element → RenderNode, lay it
// out, paint into a DisplayList, and assert the emitted draw commands (position,
// size, colour). This covers the widget pipeline (layout → RenderNode::paint →
// DrawCommands) without Vulkan — the GPU rasterisation of those commands is
// generic and covered separately by the compositor offscreen_render test.

#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include "xrender/display_list.h"
#include "shared/protocol.h"
#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

using namespace fbx::xkit;
namespace shared = fbx::shared;

namespace {
// Decode all DrawRect commands from a DisplayList.
std::vector<shared::RectData> rects(const fbx::xrender::DisplayList& dl) {
    std::vector<shared::RectData> out;
    auto cmds = dl.getCommands();
    auto buf  = dl.getDataBuffer();
    for (const auto& c : cmds) {
        if (c.type_ != shared::CommandType::DrawRect) continue;
        if (c.data_offset_ + sizeof(shared::RectData) > buf.size()) continue;
        shared::RectData r{};
        std::memcpy(&r, buf.data() + c.data_offset_, sizeof(r));
        out.push_back(r);
    }
    return out;
}

// Paint a widget's laid-out tree into a fresh DisplayList.
fbx::xrender::DisplayList paintWidget(std::shared_ptr<Widget> w, float vw, float vh,
                                      fbx::xrender::DisplayList& dl) {
    auto el = w->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, vw, 0, vh});
    el->getRenderNode()->paint(dl, {0.0f, 0.0f});
    return {};  // dl filled by reference
}

bool near(float a, float b, float eps = 0.5f) { return std::abs(a - b) < eps; }
}  // namespace

TEST(WidgetRender, ContainerEmitsColouredRectOfItsSize) {
    auto box = std::make_shared<ContainerWidget>();
    box->setFixedSize(100.0f, 50.0f);
    box->setBackgroundColor(Color{1.0f, 0.0f, 0.0f, 1.0f});   // red

    fbx::xrender::DisplayList dl;
    paintWidget(box, 500, 500, dl);

    auto rs = rects(dl);
    // At least one red rect of the container's size at the origin.
    bool found = false;
    for (const auto& r : rs)
        if (near(r.width_, 100) && near(r.height_, 50) &&
            near(r.r_, 1) && near(r.g_, 0) && near(r.b_, 0)) { found = true; break; }
    EXPECT_TRUE(found) << "no 100x50 red rect among " << rs.size() << " rects";
}

TEST(WidgetRender, FlexColumnStacksChildrenVertically) {
    auto a = std::make_shared<ContainerWidget>();
    a->setFixedSize(100.0f, 30.0f);
    a->setBackgroundColor(Color{1.0f, 0.0f, 0.0f, 1.0f});
    auto b = std::make_shared<ContainerWidget>();
    b->setFixedSize(100.0f, 30.0f);
    b->setBackgroundColor(Color{0.0f, 0.0f, 1.0f, 1.0f});   // blue
    auto col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{a, b});

    fbx::xrender::DisplayList dl;
    paintWidget(col, 500, 500, dl);

    auto rs = rects(dl);
    // The blue child must be painted below the red one (stacked, y ≈ 30).
    std::optional<float> red_y, blue_y;
    for (const auto& r : rs) {
        if (near(r.r_, 1) && near(r.b_, 0) && near(r.height_, 30)) red_y = r.y_;
        if (near(r.b_, 1) && near(r.r_, 0) && near(r.height_, 30)) blue_y = r.y_;
    }
    ASSERT_TRUE(red_y.has_value());
    ASSERT_TRUE(blue_y.has_value());
    EXPECT_TRUE(near(*red_y, 0));
    EXPECT_TRUE(near(*blue_y, 30)) << "blue at y=" << *blue_y << " (expected ~30)";
    EXPECT_GT(*blue_y, *red_y);
}

TEST(WidgetRender, ParentOffsetTranslatesDraws) {
    auto box = std::make_shared<ContainerWidget>();
    box->setFixedSize(40.0f, 40.0f);
    box->setBackgroundColor(Color{0.0f, 1.0f, 0.0f, 1.0f});   // green

    auto el = box->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 500, 0, 500});

    fbx::xrender::DisplayList dl;
    el->getRenderNode()->paint(dl, {25.0f, 60.0f});   // painted at a parent offset

    bool found = false;
    for (const auto& r : rects(dl))
        if (near(r.width_, 40) && near(r.height_, 40) && near(r.x_, 25) && near(r.y_, 60))
            { found = true; break; }
    EXPECT_TRUE(found) << "green rect not translated to (25,60)";
}
