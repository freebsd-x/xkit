// Tests for the indeterminate spinner: the pure phase→dot-alpha ramp and the
// emitted draw commands (one faded dot per ring position, brightest at the head).

#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include "xrender/display_list.h"
#include "shared/protocol.h"
#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include <vector>

using namespace fbx::xkit;
namespace shared = fbx::shared;

namespace {
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
}  // namespace

// ── pure ramp ────────────────────────────────────────────────────────────────

TEST(Spinner, HeadDotIsBrightest) {
    const int n = 8;
    float head = RenderSpinner::dotAlpha(0, 0.0f, n);
    for (int i = 1; i < n; ++i)
        EXPECT_LE(RenderSpinner::dotAlpha(i, 0.0f, n), head)
            << "dot " << i << " brighter than the head";
    EXPECT_FLOAT_EQ(head, 1.0f);   // head dot fully opaque
}

TEST(Spinner, TailTrailsBehindHead) {
    // With head at 0, the dot just BEHIND (index n-1) should be brighter than the
    // dot just AHEAD (index 1) — the comet tail trails the head.
    const int n = 8;
    EXPECT_GT(RenderSpinner::dotAlpha(n - 1, 0.0f, n),
              RenderSpinner::dotAlpha(1, 0.0f, n));
}

TEST(Spinner, AlphaFloorAndWrap) {
    const int n = 8;
    // The farthest-ahead dot sits at the minimum alpha (0.15), not zero/negative.
    EXPECT_GE(RenderSpinner::dotAlpha(1, 0.0f, n), 0.0f);
    // Advancing the head one step shifts the bright dot: dot 3 is brightest at head 3.
    float a_at_h0 = RenderSpinner::dotAlpha(3, 0.0f, n);
    float a_at_h3 = RenderSpinner::dotAlpha(3, 3.0f, n);
    EXPECT_GT(a_at_h3, a_at_h0);
    EXPECT_FLOAT_EQ(a_at_h3, 1.0f);
}

// ── render ───────────────────────────────────────────────────────────────────

TEST(Spinner, PaintsOneDotPerRingPosition) {
    auto sp = std::make_shared<RenderSpinner>(40.0f, 8, Color{1, 1, 1, 1});
    (void)sp->layout(Constraints{0, 200, 0, 200});
    fbx::xrender::DisplayList dl;
    sp->paintSelf(dl, {0.0f, 0.0f});

    auto rs = rects(dl);
    EXPECT_EQ(rs.size(), 8u);                 // one dot per position
    for (const auto& r : rs) {
        EXPECT_GT(r.width_, 0.0f);
        EXPECT_GT(r.border_radius_, 0.0f);    // rounded → circular dot
        EXPECT_GE(r.a_, 0.0f);
        EXPECT_LE(r.a_, 1.0f);
    }
}

TEST(Spinner, BrightestDrawnDotMovesWithPhase) {
    auto sp = std::make_shared<RenderSpinner>(40.0f, 8, Color{1, 1, 1, 1});
    (void)sp->layout(Constraints{0, 200, 0, 200});

    auto maxAlphaIndex = [&](float phase) {
        sp->setPhase(phase);
        fbx::xrender::DisplayList dl;
        sp->paintSelf(dl, {0.0f, 0.0f});
        auto rs = rects(dl);
        int best = -1; float ba = -1.0f;
        for (size_t i = 0; i < rs.size(); ++i)
            if (rs[i].a_ > ba) { ba = rs[i].a_; best = static_cast<int>(i); }
        return best;
    };
    EXPECT_EQ(maxAlphaIndex(0.0f), 0);
    EXPECT_EQ(maxAlphaIndex(4.0f), 4);        // head moved → different dot brightest
}

// ── widget/element wiring ──────────────────────────────────────────────────────

TEST(Spinner, ElementBuildsRingAndAdvancesPhaseInRange) {
    auto w  = std::make_shared<SpinnerWidget>(32.0f, 12, 0.9f);
    auto el = w->createElement();
    el->mount(nullptr);
    auto rn = std::dynamic_pointer_cast<RenderSpinner>(el->getRenderNode());
    ASSERT_TRUE(rn);
    EXPECT_EQ(rn->dotCount(), 12);

    el->syncState();                          // pulls wall-clock → phase
    EXPECT_GE(rn->phase(), 0.0f);
    EXPECT_LT(rn->phase(), 12.0f);            // head stays within [0, dotCount)
}
