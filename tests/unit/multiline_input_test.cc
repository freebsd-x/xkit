// MultilineTextInputWidget rendering verification.
//
// The text engine shapes a single run and does not break on '\n', so the widget
// splits its content into one RenderText per line. This test paints the widget
// into a DisplayList and inspects the emitted DrawText commands to confirm the
// lines are split and stacked vertically — the core reason the widget exists.

#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include "xrender/display_list.h"
#include "shared/protocol.h"
#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace fbx::xkit;
namespace shared = fbx::shared;

namespace {
struct Line { float x, y; std::string text; };

// Decode every DrawText command (TextData header + inline UTF-8 string).
std::vector<Line> texts(const fbx::xrender::DisplayList& dl) {
    std::vector<Line> out;
    auto cmds = dl.getCommands();
    auto buf  = dl.getDataBuffer();
    for (const auto& c : cmds) {
        if (c.type_ != shared::CommandType::DrawText) continue;
        if (c.data_offset_ + sizeof(shared::TextData) > buf.size()) continue;
        shared::TextData td{};
        std::memcpy(&td, buf.data() + c.data_offset_, sizeof(td));
        size_t str_off = c.data_offset_ + sizeof(td);
        std::string s;
        if (str_off + td.string_length_ <= buf.size())
            s.assign(reinterpret_cast<const char*>(buf.data() + str_off), td.string_length_);
        out.push_back({td.x_, td.y_, s});
    }
    return out;
}

// Build a multiline widget, lay it out, and paint into dl.
void paint(std::shared_ptr<Widget> w, fbx::xrender::DisplayList& dl) {
    auto el = w->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 400, 0, 400});
    el->getRenderNode()->paint(dl, {0.0f, 0.0f});
}
}  // namespace

TEST(MultilineInput, SplitsOnNewlineIntoStackedLines) {
    auto w = std::make_shared<MultilineTextInputWidget>("Hello\nWorld");
    fbx::xrender::DisplayList dl;
    paint(w, dl);

    auto ls = texts(dl);
    ASSERT_EQ(ls.size(), 2u) << "two lines expected, got " << ls.size();
    EXPECT_EQ(ls[0].text, "Hello");
    EXPECT_EQ(ls[1].text, "World");
    EXPECT_GT(ls[1].y, ls[0].y) << "second line must be below the first";
    EXPECT_FLOAT_EQ(ls[0].x, ls[1].x) << "lines share the same left edge";
}

TEST(MultilineInput, SingleLineEmitsOneDrawText) {
    auto w = std::make_shared<MultilineTextInputWidget>("OneLine");
    fbx::xrender::DisplayList dl;
    paint(w, dl);

    auto ls = texts(dl);
    ASSERT_EQ(ls.size(), 1u);
    EXPECT_EQ(ls[0].text, "OneLine");
}

TEST(MultilineInput, ThreeLinesStackWithIncreasingY) {
    auto w = std::make_shared<MultilineTextInputWidget>("a\nb\nc");
    fbx::xrender::DisplayList dl;
    paint(w, dl);

    auto ls = texts(dl);
    ASSERT_EQ(ls.size(), 3u);
    EXPECT_EQ(ls[0].text, "a");
    EXPECT_EQ(ls[1].text, "b");
    EXPECT_EQ(ls[2].text, "c");
    EXPECT_GT(ls[1].y, ls[0].y);
    EXPECT_GT(ls[2].y, ls[1].y);
}

TEST(MultilineInput, EmptyUnfocusedShowsPlaceholder) {
    auto w = std::make_shared<MultilineTextInputWidget>("");
    w->setPlaceholder("Type your message");
    fbx::xrender::DisplayList dl;
    paint(w, dl);

    auto ls = texts(dl);
    ASSERT_EQ(ls.size(), 1u);
    EXPECT_EQ(ls[0].text, "Type your message");
}

TEST(MultilineInput, TrailingNewlineAddsBlankLineHeight) {
    // A blank line renders no glyphs but still occupies a line of height, so a
    // trailing newline makes the field taller than the same text without it.
    auto one = std::make_shared<MultilineTextInputWidget>("line1");
    auto two = std::make_shared<MultilineTextInputWidget>("line1\n");

    auto measure = [](std::shared_ptr<Widget> w) {
        auto el = w->createElement();
        el->mount(nullptr);
        return el->getRenderNode()->layout(Constraints{0, 400, 0, 400}).height_;
    };
    float h1 = measure(one), h2 = measure(two);
    EXPECT_GT(h2, h1) << "trailing newline should add a blank line's height";

    // The visible text is unchanged (only "line1" draws).
    fbx::xrender::DisplayList dl;
    paint(two, dl);
    auto ls = texts(dl);
    ASSERT_EQ(ls.size(), 1u);
    EXPECT_EQ(ls[0].text, "line1");
}
