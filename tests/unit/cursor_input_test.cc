// Integration test for the single-line TextInputWidget cursor/selection model:
// drives real key events through the element and inspects the painted DisplayList
// (text split around the caret, caret bar, selection highlight).

#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include "xkit/application.h"
#include "xrender/display_list.h"
#include "xrender/font_manager.h"
#include "shared/protocol.h"
#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace fbx::xkit;
namespace shared = fbx::shared;

namespace {
constexpr uint32_t kShift = 1, kCtrl = 2;

shared::InputMessage key(int keycode, int unicode = 0, uint32_t mods = 0) {
    shared::InputMessage m{};
    m.type_       = shared::InputEventType::KeyPress;
    m.value_a_    = keycode;      // Linux evdev keycode
    m.value_b_    = 1;            // pressed
    m.modifiers_  = mods;
    m.char_value_ = unicode;      // printable codepoint, 0 for special keys
    return m;
}

// A post-processed MouseButton-down at window (x,y). In the real loop the app
// packs the button code into modifiers_'s high bits and puts button state in
// char_value_; we replicate the shape the element sees.
shared::InputMessage mouse(float x, float y, bool shift = false) {
    shared::InputMessage m{};
    m.type_       = shared::InputEventType::MouseButton;
    m.value_a_    = static_cast<int32_t>(x);
    m.value_b_    = static_cast<int32_t>(y);
    m.char_value_ = 1;                       // button down
    m.modifiers_  = shift ? kShift : 0u;
    return m;
}

shared::InputMessage moveTo(float x, float y) {
    shared::InputMessage m{};
    m.type_    = shared::InputEventType::MouseMove;
    m.value_a_ = static_cast<int32_t>(x);
    m.value_b_ = static_cast<int32_t>(y);
    return m;
}

// Number of distinct DrawText y positions == number of rendered visual lines.
int visualLineCount(const fbx::xrender::DisplayList& dl) {
    std::vector<float> ys;
    auto cmds = dl.getCommands();
    auto buf  = dl.getDataBuffer();
    for (const auto& c : cmds) {
        if (c.type_ != shared::CommandType::DrawText) continue;
        shared::TextData td{};
        std::memcpy(&td, buf.data() + c.data_offset_, sizeof(td));
        bool seen = false;
        for (float y : ys) if (std::abs(y - td.y_) < 0.5f) { seen = true; break; }
        if (!seen) ys.push_back(td.y_);
    }
    return static_cast<int>(ys.size());
}

// x of the first DrawText in paint order (the text origin when the caret is at 0).
float firstTextX(const fbx::xrender::DisplayList& dl) {
    auto cmds = dl.getCommands();
    auto buf  = dl.getDataBuffer();
    for (const auto& c : cmds) {
        if (c.type_ != shared::CommandType::DrawText) continue;
        shared::TextData td{};
        std::memcpy(&td, buf.data() + c.data_offset_, sizeof(td));
        return td.x_;
    }
    return 0.0f;
}

// Concatenate all DrawText strings in paint order == the field's display text.
std::string paintedText(const fbx::xrender::DisplayList& dl) {
    std::string out;
    auto cmds = dl.getCommands();
    auto buf  = dl.getDataBuffer();
    for (const auto& c : cmds) {
        if (c.type_ != shared::CommandType::DrawText) continue;
        shared::TextData td{};
        std::memcpy(&td, buf.data() + c.data_offset_, sizeof(td));
        size_t off = c.data_offset_ + sizeof(td);
        if (off + td.string_length_ <= buf.size())
            out.append(reinterpret_cast<const char*>(buf.data() + off), td.string_length_);
    }
    return out;
}

struct Rect { float x, w, a; };
std::vector<Rect> rects(const fbx::xrender::DisplayList& dl) {
    std::vector<Rect> out;
    auto cmds = dl.getCommands();
    auto buf  = dl.getDataBuffer();
    for (const auto& c : cmds) {
        if (c.type_ != shared::CommandType::DrawRect) continue;
        shared::RectData r{};
        std::memcpy(&r, buf.data() + c.data_offset_, sizeof(r));
        out.push_back({r.x_, r.width_, r.a_});
    }
    return out;
}

// A caret is a thin (~1.5px) opaque bar.
bool hasCaret(const std::vector<Rect>& rs) {
    for (auto& r : rs) if (r.w > 1.0f && r.w < 2.5f && r.a > 0.9f) return true;
    return false;
}

struct Field {
    std::shared_ptr<TextInputWidget> w;
    std::shared_ptr<Element> el;
    void send(const shared::InputMessage& m) { (void)el->handleEvent(m); }
    fbx::xrender::DisplayList paint() {
        fbx::xrender::DisplayList dl;
        (void)el->getRenderNode()->layout(Constraints{0, 300, 0, 40});
        el->getRenderNode()->paint(dl, {0.0f, 0.0f});
        return dl;
    }
};
Field makeField(bool focus = true) {
    auto w = std::make_shared<TextInputWidget>();
    auto el = w->createElement();
    el->mount(nullptr);
    if (focus) XApplication::requestFocus(el);
    return {w, el};
}
void type(Field& f, const std::string& s) {
    for (char c : s) f.send(key(/*keycode=*/0, /*unicode=*/static_cast<unsigned char>(c)));
}
}  // namespace

TEST(CursorInput, InsertsAtCaretNotEnd) {
    auto f = makeField();
    type(f, "Hello");
    f.send(key(102));            // Home → caret to start
    type(f, "X");                // insert at front
    EXPECT_EQ(f.w->text(), "XHello");
    EXPECT_EQ(paintedText(f.paint()), "XHello");
}

TEST(CursorInput, BackspaceDeletesBeforeCaret) {
    auto f = makeField();
    type(f, "Hello");
    f.send(key(102));            // Home (caret at 0)
    f.send(key(106));            // Right (caret at 1)
    f.send(key(14));             // Backspace → removes 'H' at index 0
    EXPECT_EQ(f.w->text(), "ello");
}

TEST(CursorInput, DeleteRemovesAtCaret) {
    auto f = makeField();
    type(f, "Hello");
    f.send(key(102));            // Home
    f.send(key(111));            // Delete → removes 'H'
    EXPECT_EQ(f.w->text(), "ello");
}

TEST(CursorInput, CaretShownOnlyWhenFocused) {
    auto focused = makeField(/*focus=*/true);
    type(focused, "Hi");
    EXPECT_TRUE(hasCaret(rects(focused.paint()))) << "focused field must draw a caret";

    auto blurred = makeField(/*focus=*/false);
    blurred.w->setText("Hi");
    // Defensive: ensure nothing else holds focus.
    XApplication::requestFocus(nullptr);
    EXPECT_FALSE(hasCaret(rects(blurred.paint()))) << "unfocused field must not draw a caret";
}

TEST(CursorInput, SelectAllPaintsHighlightOfTextWidth) {
    auto f = makeField();
    type(f, "Hello");
    f.send(key(30, 0, kCtrl));   // Ctrl+A → select all
    auto rs = rects(f.paint());
    float want = fbx::xrender::FontManager::measureText("Hello", f.w->fontSize());
    bool found = false;
    for (auto& r : rs)
        if (r.a > 0.2f && r.a < 0.7f && std::abs(r.w - want) < 3.0f) { found = true; break; }
    EXPECT_TRUE(found) << "select-all should paint a highlight ~" << want << "px wide";
}

TEST(CursorInput, ShiftArrowSelectsAndTypeReplaces) {
    auto f = makeField();
    type(f, "abcde");
    f.send(key(102));                 // Home
    f.send(key(106, 0, kShift));      // Shift+Right  → select "a"
    f.send(key(106, 0, kShift));      // Shift+Right  → select "ab"
    type(f, "X");                     // replaces selection
    EXPECT_EQ(f.w->text(), "Xcde");
}

// ── Click-to-position (single line) ───────────────────────────────────────────────
TEST(CursorInput, ClickAtLeftEdgePlacesCaretAtStart) {
    auto f = makeField();
    type(f, "Hello");
    f.paint();                       // establish node offsets
    f.send(mouse(0, 10));            // click far left → caret to start
    type(f, "Z");
    EXPECT_EQ(f.w->text(), "ZHello");
}

TEST(CursorInput, ClickPastEndPlacesCaretAtEnd) {
    auto f = makeField();
    type(f, "Hello");
    f.send(mouse(0, 10));            // caret to start
    f.paint();
    f.send(mouse(100000, 10));       // click far right → caret to end
    type(f, "Z");
    EXPECT_EQ(f.w->text(), "HelloZ");
}

TEST(CursorInput, ClickMidPlacesCaretAtNearestBoundary) {
    auto f = makeField();
    type(f, "Hello");
    f.send(mouse(0, 10));            // caret 0 so the text origin is measurable
    float origin = firstTextX(f.paint());
    float target = origin + fbx::xrender::FontManager::measureText("Hel", f.w->fontSize());
    f.send(mouse(target, 10));       // click just after "Hel" → caret at index 3
    type(f, "Z");
    EXPECT_EQ(f.w->text(), "HelZlo");
}

TEST(CursorInput, DragSelectsRangeThenTypeReplaces) {
    auto f = makeField();
    type(f, "Hello");
    f.send(mouse(0, 10));                 // press at far left → caret 0, captures pointer
    float origin = firstTextX(f.paint());
    float he = fbx::xrender::FontManager::measureText("He", f.w->fontSize());
    f.send(moveTo(origin + he, 10));      // drag right past "He" → selection [0,2]
    type(f, "Z");                         // replaces the selection
    EXPECT_EQ(f.w->text(), "Zllo");
}

TEST(CursorInput, DoubleClickDragExtendsBySelectedWords) {
    auto f = makeField();
    type(f, "the quick brown fox");
    float origin = firstTextX(f.paint());
    float xq = origin + fbx::xrender::FontManager::measureText("the qu", f.w->fontSize());
    float xb = origin + fbx::xrender::FontManager::measureText("the quick bro", f.w->fontSize());
    f.send(mouse(xq, 10));               // click 1 in "quick"
    f.send(mouse(xq, 10));               // click 2 → select word "quick", drag by word
    f.send(moveTo(xb, 10));              // drag into "brown" → selection snaps to "quick brown"
    type(f, "Z");                        // replaces the whole word range
    EXPECT_EQ(f.w->text(), "the Z fox");
}

TEST(CursorInput, DragEndsWithButtonUp) {
    auto f = makeField();
    type(f, "Hello");
    f.send(mouse(0, 10));                 // press → captures
    ASSERT_EQ(XApplication::getPointerCapture().get(), f.el.get());
    // (The app releases capture on button-up; verify the element tolerates the up.)
    shared::InputMessage up{};
    up.type_ = shared::InputEventType::MouseButton;
    up.char_value_ = 0;                   // button up
    (void)f.el->handleEvent(up);
    XApplication::releasePointer();       // what the app loop does after the up
    EXPECT_EQ(XApplication::getPointerCapture().get(), nullptr);
}

// ── Undo / redo (single line) ──────────────────────────────────────────────────────
namespace { constexpr int kZ = 44, kY = 21; }

TEST(CursorInput, UndoRemovesTypedRunAndRedoRestores) {
    auto f = makeField();
    type(f, "hello");                     // one insert group (no spaces)
    f.send(key(kZ, 0, kCtrl));            // Ctrl+Z
    EXPECT_EQ(f.w->text(), "");
    f.send(key(kY, 0, kCtrl));            // Ctrl+Y (redo)
    EXPECT_EQ(f.w->text(), "hello");
}

TEST(CursorInput, UndoIsWordGranularAcrossSpaces) {
    auto f = makeField();
    type(f, "ab cd");                     // groups: "ab", " ", "cd"
    f.send(key(kZ, 0, kCtrl));
    EXPECT_EQ(f.w->text(), "ab ") << "undo removes the last word, not everything";
    f.send(key(kZ, 0, kCtrl));
    EXPECT_EQ(f.w->text(), "ab");
}

TEST(CursorInput, CaretMoveStartsANewUndoGroup) {
    auto f = makeField();
    type(f, "ab");
    f.send(key(105));                     // Left — ends the current group
    type(f, "X");                         // inserted at index 1 → "aXb"
    ASSERT_EQ(f.w->text(), "aXb");
    f.send(key(kZ, 0, kCtrl));            // undo only the "X" group
    EXPECT_EQ(f.w->text(), "ab");
}

TEST(CursorInput, ShiftZAlsoRedoes) {
    auto f = makeField();
    type(f, "hi");
    f.send(key(kZ, 0, kCtrl));            // undo → ""
    ASSERT_EQ(f.w->text(), "");
    f.send(key(kZ, 0, kCtrl | kShift));   // Ctrl+Shift+Z → redo
    EXPECT_EQ(f.w->text(), "hi");
}

// ── Double / triple click (single line) ───────────────────────────────────────────
TEST(CursorInput, DoubleClickSelectsWord) {
    auto f = makeField();
    type(f, "the quick brown");
    f.send(mouse(0, 10));                 // caret 0 so origin is measurable
    float origin = firstTextX(f.paint());
    float x = origin + fbx::xrender::FontManager::measureText("the qu", f.w->fontSize());
    f.send(mouse(x, 10));                 // click 1 (inside "quick")
    f.send(mouse(x, 10));                 // click 2 → select the word
    type(f, "Z");                         // replaces "quick"
    EXPECT_EQ(f.w->text(), "the Z brown");
}

TEST(CursorInput, TripleClickSelectsWholeLine) {
    auto f = makeField();
    type(f, "hello world");
    f.send(mouse(5, 10));                 // click 1
    f.send(mouse(5, 10));                 // click 2
    f.send(mouse(5, 10));                 // click 3 → select the line
    type(f, "Z");                         // replaces everything
    EXPECT_EQ(f.w->text(), "Z");
}

TEST(CursorInput, ClickAtDifferentSpotResetsClickCount) {
    auto f = makeField();
    type(f, "ab cd");
    f.send(mouse(0, 10));                 // click near start
    float origin = firstTextX(f.paint());
    float endx = origin + fbx::xrender::FontManager::measureText("ab cd", f.w->fontSize());
    f.send(mouse(endx, 10));              // click at a different spot → NOT a double-click
    type(f, "Z");                         // plain caret insert, no word replaced
    EXPECT_EQ(f.w->text(), "ab cdZ");
}

// ── Multi-line field shares the same cursor model ─────────────────────────────────
namespace {
struct MField {
    std::shared_ptr<MultilineTextInputWidget> w;
    std::shared_ptr<Element> el;
    void send(const shared::InputMessage& m) { (void)el->handleEvent(m); }
    fbx::xrender::DisplayList paint() {
        fbx::xrender::DisplayList dl;
        (void)el->getRenderNode()->layout(Constraints{0, 300, 0, 200});
        el->getRenderNode()->paint(dl, {0.0f, 0.0f});
        return dl;
    }
};
MField makeMulti() {
    auto w = std::make_shared<MultilineTextInputWidget>();
    auto el = w->createElement();
    el->mount(nullptr);
    XApplication::requestFocus(el);
    return {w, el};
}
void typeM(MField& f, const std::string& s) {
    for (char c : s) f.send(key(0, static_cast<unsigned char>(c)));
}
// A multiline field with a real width: the size must be set on the widget BEFORE
// the element is created so apply_widget_props copies it to the render node.
MField makeMultiSized(float w, float h) {
    auto widget = std::make_shared<MultilineTextInputWidget>();
    widget->setFixedSize(w, h);
    auto el = widget->createElement();
    el->mount(nullptr);
    XApplication::requestFocus(el);
    return {widget, el};
}
}  // namespace

TEST(MultilineCursor, ReturnInsertsNewlineAtCaret) {
    auto f = makeMulti();
    typeM(f, "ab");
    f.send(key(105));            // Left → caret between a and b
    f.send(key(28));             // Return
    EXPECT_EQ(f.w->text(), "a\nb");
}

TEST(MultilineCursor, UpArrowMovesToPreviousLine) {
    auto f = makeMulti();
    typeM(f, "ab\ncd");          // caret at end (line 2, column 2)
    f.send(key(103));            // Up → line 1, column 2 (index 2)
    typeM(f, "X");               // insert there
    EXPECT_EQ(f.w->text(), "abX\ncd");
}

TEST(MultilineCursor, CtrlReturnSubmitsWithoutNewline) {
    auto f = makeMulti();
    std::string got;
    f.w->setOnSubmit([&](const std::string& s) { got = s; });
    typeM(f, "hi");
    f.send(key(28, 0, kCtrl));   // Ctrl+Return
    EXPECT_EQ(got, "hi");
    EXPECT_EQ(f.w->text(), "hi") << "Ctrl+Return must not insert a newline";
}

TEST(MultilineCursor, CaretRendersOnFocusedMultiline) {
    auto f = makeMulti();
    typeM(f, "a\nb");
    EXPECT_TRUE(hasCaret(rects(f.paint()))) << "multiline caret must be painted";
}

TEST(MultilineCursor, ClickSelectsLineByY) {
    auto f = makeMulti();
    typeM(f, "ab\ncd");
    f.paint();                       // establish per-line node offsets
    // Line 0 top ≈ padding(10); line 1 top ≈ 10 + fontSize(16) + gap(2) = 28.
    f.send(mouse(0, 30));            // click on the second line, far left → index 3
    typeM(f, "Z");
    EXPECT_EQ(f.w->text(), "ab\nZcd");
}

TEST(MultilineCursor, ClickOnFirstLinePastEnd) {
    auto f = makeMulti();
    typeM(f, "ab\ncd");
    f.paint();
    f.send(mouse(100000, 12));       // first line, far right → end of "ab" (index 2)
    typeM(f, "Z");
    EXPECT_EQ(f.w->text(), "abZ\ncd");
}

TEST(MultilineCursor, DoubleClickSelectsWordOnLine) {
    auto f = makeMulti();
    typeM(f, "foo bar\nbaz");
    float origin = firstTextX(f.paint());
    float x = origin + fbx::xrender::FontManager::measureText("foo ", 16.0f);
    f.send(mouse(x, 12));                 // click 1 on line 0, inside "bar"
    f.paint();                            // frame repaints between clicks (offsets valid)
    f.send(mouse(x, 12));                 // click 2 → select "bar"
    typeM(f, "Z");
    EXPECT_EQ(f.w->text(), "foo Z\nbaz");
}

TEST(MultilineCursor, UndoRemovesNewlineGroup) {
    auto f = makeMulti();
    typeM(f, "ab");
    f.send(key(28));                     // Return → newline (its own undo group)
    ASSERT_EQ(f.w->text(), "ab\n");
    f.send(key(kZ, 0, kCtrl));           // undo the newline
    EXPECT_EQ(f.w->text(), "ab");
    f.send(key(kZ, 0, kCtrl));           // undo the "ab" typing
    EXPECT_EQ(f.w->text(), "");
}

TEST(MultilineCursor, VerticalMovePreservesPixelColumnAcrossShortLine) {
    // Caret starts at the end of a long line, goes up through a short line and back
    // down. With a sticky pixel goal it returns to the original column on the long
    // bottom line; a byte-column impl would collapse to the short line's width (2).
    auto f = makeMulti();
    typeM(f, "aaaa\nbb\naaaa");   // caret at end (line 2, column 4)
    f.send(key(103));            // Up   → line 1 "bb" (clamped)
    f.send(key(103));            // Up   → line 0, back at pixel column of "aaaa" end
    f.send(key(108));            // Down → line 1 "bb" (clamped)
    f.send(key(108));            // Down → line 2, pixel column restored to the end
    typeM(f, "Z");               // insert at the caret
    EXPECT_EQ(f.w->text(), "aaaa\nbb\naaaaZ")
        << "pixel-column Up/Down must restore the end column on the bottom line";
}

TEST(MultilineCursor, SoftWrapsLongLineToMultipleVisualLines) {
    auto f = makeMultiSized(90.0f, 200.0f);  // narrow field (width set before element)
    typeM(f, "alpha bravo charlie delta");   // one long logical line
    f.paint();                               // layout establishes the content width
    f.el->syncState();                       // re-wrap now that the width is known
    auto dl = f.paint();
    EXPECT_GT(visualLineCount(dl), 1) << "a long line should wrap to several visual lines";
    EXPECT_EQ(f.w->text(), "alpha bravo charlie delta") << "wrapping must not alter the text";
}

TEST(MultilineCursor, DoubleClickDragExtendsByWordsAcrossLines) {
    auto f = makeMultiSized(260.0f, 200.0f);   // wide enough that "foo bar" does not wrap
    typeM(f, "foo bar\nbaz qux");
    float origin = firstTextX(f.paint());
    float xbar = origin + fbx::xrender::FontManager::measureText("foo ", 16.0f);
    float xbaz = origin + fbx::xrender::FontManager::measureText("ba", 16.0f);
    f.send(mouse(xbar, 4));              // click 1 in "bar" on line 0
    f.paint();                           // frame repaints between clicks
    f.send(mouse(xbar, 4));             // click 2 → select "bar", drag by word
    f.send(moveTo(xbaz, 20));            // drag to "baz" on line 1 → selects "bar\nbaz"
    typeM(f, "Z");
    EXPECT_EQ(f.w->text(), "foo Z qux");
}

TEST(MultilineCursor, DragAcrossLinesSelectsThenReplaces) {
    auto f = makeMulti();
    typeM(f, "ab\ncd");
    f.paint();                       // establish line offsets before the drag
    f.send(mouse(0, 12));            // press on line 0, far left → caret 0 (snapshot taken)
    f.send(moveTo(0, 30));           // drag onto line 2, far left → select "ab\n" [0,3]
    typeM(f, "Z");                   // replaces the selection
    EXPECT_EQ(f.w->text(), "Zcd");
}
