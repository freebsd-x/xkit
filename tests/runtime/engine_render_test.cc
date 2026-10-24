// Engine rasterisation runtime test — the "true pixel" end of xkit rendering.
//
// widget_render_test asserts the DRAW COMMANDS a widget emits; this test drives
// those commands through the real VulkanEngine (render pass → solid pipeline →
// render_target_) and reads the resulting PIXELS back to the CPU. That closes the
// last gap in the widget path: command generation *and* GPU rasterisation.
//
// Skips automatically when Vulkan is unavailable (init() returns false), so it is
// safe in headless CI without a GPU. The render target is BGRA8, so a pixel read
// as uint32_t is 0xAARRGGBB on little-endian.

#include "xkit/engine.h"
#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include <gtest/gtest.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace fbx::xkit;

namespace {
constexpr uint32_t kW = 64, kH = 64;

// Channel extractors for a BGRA8 pixel packed as 0xAARRGGBB.
uint8_t R(uint32_t p) { return (p >> 16) & 0xFF; }
uint8_t G(uint32_t p) { return (p >> 8)  & 0xFF; }
uint8_t B(uint32_t p) { return  p        & 0xFF; }

// Build a full-viewport ContainerWidget of a colour, paint it through the engine,
// and read the pixels back. Returns empty on Vulkan-unavailable.
std::vector<uint32_t> renderSolid(VulkanEngine& eng, Color c) {
    auto box = std::make_shared<ContainerWidget>();
    box->setFixedSize((float)kW, (float)kH);
    box->setBackgroundColor(c);
    auto el = box->createElement();
    el->mount(nullptr);
    auto node = el->getRenderNode();
    (void)node->layout(Constraints{0, (float)kW, 0, (float)kH});
    eng.paint(node);
    return eng.readbackPixels(kW, kH);
}

uint32_t center(const std::vector<uint32_t>& buf) { return buf[(kH / 2) * kW + (kW / 2)]; }

// A pixel is "white" when all three channels are clearly bright (glyph core over
// the dark clear colour). The clear colour is (13,13,26), so this never matches
// background — only actually-drawn light pixels.
bool isWhite(uint32_t p) { return R(p) > 128 && G(p) > 128 && B(p) > 128; }

// Paint a TextWidget into the target and read the pixels back.
std::vector<uint32_t> renderText(VulkanEngine& eng, const std::string& s, Color c, float size) {
    auto t = std::make_shared<TextWidget>(s, size);
    t->setColor(c);
    auto el = t->createElement();
    el->mount(nullptr);
    auto node = el->getRenderNode();
    (void)node->layout(Constraints{0, (float)kW, 0, (float)kH});
    eng.paint(node);
    return eng.readbackPixels(kW, kH);
}

int countWhite(const std::vector<uint32_t>& buf) {
    int n = 0; for (uint32_t p : buf) if (isWhite(p)) ++n; return n;
}

// Build a WxH texture filled with one BGRA colour (the engine uploads image
// buffers as B8G8R8A8_UNORM, so bytes are B,G,R,A in memory).
std::vector<uint8_t> solidTexture(uint32_t w, uint32_t h,
                                  uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    std::vector<uint8_t> v(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        v[i*4+0] = b; v[i*4+1] = g; v[i*4+2] = r; v[i*4+3] = a;
    }
    return v;
}

// Paint an ImageWidget (raw pixel path) stretched across the full target.
std::vector<uint32_t> renderImage(VulkanEngine& eng, std::vector<uint8_t> tex,
                                  uint32_t iw, uint32_t ih) {
    auto img = std::make_shared<ImageWidget>(std::move(tex), iw, ih);
    img->setFixedSize((float)kW, (float)kH);   // stretch the texture over the viewport
    auto el = img->createElement();
    el->mount(nullptr);
    auto node = el->getRenderNode();
    (void)node->layout(Constraints{0, (float)kW, 0, (float)kH});
    eng.paint(node);
    return eng.readbackPixels(kW, kH);
}
}  // namespace

class EngineRenderTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!eng_.init(kW, kH))
            GTEST_SKIP() << "Hardware skip: Vulkan engine failed to init (no GPU?).";
    }
    VulkanEngine eng_;
};

// A green full-viewport container must rasterise to actual green pixels.
TEST_F(EngineRenderTest, GreenContainerRastersToGreenPixels) {
    auto buf = renderSolid(eng_, Color{0.0f, 1.0f, 0.0f, 1.0f});
    ASSERT_EQ(buf.size(), kW * kH);
    uint32_t c = center(buf);
    EXPECT_GT(G(c), 200) << "green channel should dominate; pixel=" << std::hex << c;
    EXPECT_LT(R(c), 80)  << "red channel should be low; pixel="   << std::hex << c;
    EXPECT_LT(B(c), 80)  << "blue channel should be low; pixel="  << std::hex << c;
}

// The colour actually flows through to pixels: a red frame differs from a green
// frame (rules out a constant clear-colour readback).
TEST_F(EngineRenderTest, DistinctColoursProduceDistinctPixels) {
    auto green = renderSolid(eng_, Color{0.0f, 1.0f, 0.0f, 1.0f});
    auto red   = renderSolid(eng_, Color{1.0f, 0.0f, 0.0f, 1.0f});
    ASSERT_EQ(green.size(), kW * kH);
    ASSERT_EQ(red.size(),   kW * kH);
    uint32_t rc = center(red);
    EXPECT_GT(R(rc), 200) << "red frame centre should be red; pixel=" << std::hex << rc;
    EXPECT_LT(G(rc), 80);
    EXPECT_NE(center(green), center(red))
        << "distinct widget colours must yield distinct pixels";
}

// A 32×32 container at the top-left leaves the rest of the viewport on the dark
// clear colour — proving position/extent are honoured, not a full-screen fill.
TEST_F(EngineRenderTest, PartialContainerLeavesClearBackground) {
    auto box = std::make_shared<ContainerWidget>();
    box->setFixedSize(32.0f, 32.0f);
    box->setBackgroundColor(Color{0.0f, 1.0f, 0.0f, 1.0f});   // green
    auto el = box->createElement();
    el->mount(nullptr);
    auto node = el->getRenderNode();
    (void)node->layout(Constraints{0, (float)kW, 0, (float)kH});
    eng_.paint(node);
    auto buf = eng_.readbackPixels(kW, kH);
    ASSERT_EQ(buf.size(), kW * kH);

    uint32_t inside  = buf[8 * kW + 8];    // (8,8) — inside the 32×32 box
    uint32_t outside = buf[56 * kW + 56];  // (56,56) — far corner, background
    EXPECT_GT(G(inside), 200) << "inside box should be green";
    EXPECT_LT(G(outside), 120) << "outside box should be the dark clear colour, not green";
    EXPECT_NE(inside, outside) << "box interior must differ from background";
}

// ── Text pipeline ──────────────────────────────────────────────────────────────
// These drive the glyph path (text_pipeline_ sampling the font atlas) rather than
// the solid pipeline. They rely on a system font being present; if the atlas
// failed to build the engine falls back to solid colour blocks, in which case the
// white/colour assertions still hold but the "has gaps" test below would flag it.

// White text lights up white pixels; empty text draws nothing over the clear bg.
TEST_F(EngineRenderTest, WhiteTextProducesWhitePixels) {
    auto text  = renderText(eng_, "Hi", Color{1.0f, 1.0f, 1.0f, 1.0f}, 32.0f);
    auto empty = renderText(eng_, "",   Color{1.0f, 1.0f, 1.0f, 1.0f}, 32.0f);
    ASSERT_EQ(text.size(),  kW * kH);
    ASSERT_EQ(empty.size(), kW * kH);
    EXPECT_GT(countWhite(text), 20) << "white text should light up many pixels";
    EXPECT_EQ(countWhite(empty), 0) << "empty string must not draw over the clear bg";
    EXPECT_GT(countWhite(text), countWhite(empty))
        << "the lit pixels must come from the text, not the background";
}

// The text colour flows through the glyph pipeline's push constant: red text
// produces red pixels and never white ones.
TEST_F(EngineRenderTest, TextColourIsHonoured) {
    auto buf = renderText(eng_, "Hi", Color{1.0f, 0.0f, 0.0f, 1.0f}, 32.0f);
    ASSERT_EQ(buf.size(), kW * kH);
    int red = 0, white = 0;
    for (uint32_t p : buf) {
        if (R(p) > 128 && G(p) < 100 && B(p) < 100) ++red;
        if (isWhite(p)) ++white;
    }
    EXPECT_GT(red, 20)   << "red text should light up red pixels";
    EXPECT_EQ(white, 0)  << "red text must not produce white pixels";
}

// Shaped glyphs have interior gaps — proving this is the glyph pipeline and not a
// solid fill. "H H" forces a wide space; on the row with the most text pixels
// there must be dark background between the first and last lit column.
TEST_F(EngineRenderTest, ShapedGlyphsHaveInternalGaps) {
    auto buf = renderText(eng_, "H H", Color{1.0f, 1.0f, 1.0f, 1.0f}, 28.0f);
    ASSERT_EQ(buf.size(), kW * kH);

    // Row with the most white pixels — the text's main horizontal band.
    uint32_t best_row = 0; int best_count = 0;
    for (uint32_t y = 0; y < kH; ++y) {
        int c = 0;
        for (uint32_t x = 0; x < kW; ++x) if (isWhite(buf[y * kW + x])) ++c;
        if (c > best_count) { best_count = c; best_row = y; }
    }
    ASSERT_GE(best_count, 2) << "expected a row with several text pixels";

    int first = -1, last = -1;
    for (uint32_t x = 0; x < kW; ++x)
        if (isWhite(buf[best_row * kW + x])) { if (first < 0) first = (int)x; last = (int)x; }
    ASSERT_GE(last - first, 2) << "text should span several columns";

    int dark_between = 0;
    for (int x = first + 1; x < last; ++x)
        if (!isWhite(buf[best_row * kW + x])) ++dark_between;
    EXPECT_GT(dark_between, 0)
        << "shaped glyphs must leave gaps between marks (not a solid block)";
}

// ── Image / texture pipeline ─────────────────────────────────────────────────────
// Drives image_pipeline_ (a sampled texture uploaded via getOrCreateImageDescriptor).
// Uses green/black texels, which are unaffected by any R/B channel swap, so the
// assertions hold regardless of the exact upload byte order.

// A solid-green texture stretched over the viewport rasterises to green pixels.
TEST_F(EngineRenderTest, SolidImageRastersToItsColour) {
    auto buf = renderImage(eng_, solidTexture(8, 8, 0, 255, 0, 255), 8, 8); // green
    ASSERT_EQ(buf.size(), kW * kH);
    uint32_t c = center(buf);
    // Green (G>200) over the dark clear colour (G~13) already proves the texture
    // reached the framebuffer; the image fills the whole viewport so there is no
    // background pixel left to compare against.
    EXPECT_GT(G(c), 200) << "image centre should be green; pixel=" << std::hex << c;
    EXPECT_LT(R(c), 80)  << "red channel should be low; pixel="   << std::hex << c;
    EXPECT_LT(B(c), 80)  << "blue channel should be low; pixel="  << std::hex << c;
}

// A two-region texture (top green, bottom black) proves UV sampling maps texels
// to screen positions — a solid fill would make top and bottom identical.
TEST_F(EngineRenderTest, ImageSamplesSpatially) {
    constexpr uint32_t iw = 8, ih = 8;
    auto tex = solidTexture(iw, ih, 0, 0, 0, 255);   // start all black
    for (uint32_t y = 0; y < ih / 2; ++y)            // top half → green
        for (uint32_t x = 0; x < iw; ++x) {
            size_t i = (static_cast<size_t>(y) * iw + x) * 4;
            tex[i+1] = 255;                            // G
        }
    auto buf = renderImage(eng_, std::move(tex), iw, ih);
    ASSERT_EQ(buf.size(), kW * kH);

    uint32_t top    = buf[8  * kW + kW / 2];   // (32, 8)  — green region
    uint32_t bottom = buf[56 * kW + kW / 2];   // (32, 56) — black region
    EXPECT_GT(G(top), 200)   << "top of image should sample the green texels";
    EXPECT_LT(G(bottom), 80) << "bottom of image should sample the black texels";
    EXPECT_NE(top, bottom)   << "distinct texture regions must map to distinct pixels";
}
