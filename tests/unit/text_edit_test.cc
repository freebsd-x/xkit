// Unit tests for the pure text-editing model (cursor + selection).
#include "xkit/text_edit.h"
#include <gtest/gtest.h>

using fbx::xkit::TextEditState;

namespace {
TextEditState at(const std::string& s, std::size_t cur) {
    TextEditState t; t.text = s; t.cursor = cur; t.anchor = cur; return t;
}
}

// ── Insertion at the cursor ──────────────────────────────────────────────────────
TEST(TextEdit, InsertsAtCursorNotEnd) {
    auto t = at("ac", 1);
    t.insert('b');
    EXPECT_EQ(t.text, "abc");
    EXPECT_EQ(t.cursor, 2u);
    EXPECT_FALSE(t.hasSelection());
}

TEST(TextEdit, InsertStringAdvancesCursor) {
    auto t = at("", 0);
    t.insert(std::string("hello"));
    EXPECT_EQ(t.text, "hello");
    EXPECT_EQ(t.cursor, 5u);
}

// ── Backspace / delete honour the cursor ─────────────────────────────────────────
TEST(TextEdit, BackspaceDeletesBeforeCursor) {
    auto t = at("abc", 2);
    t.backspace();
    EXPECT_EQ(t.text, "ac");
    EXPECT_EQ(t.cursor, 1u);
}

TEST(TextEdit, BackspaceAtStartIsNoop) {
    auto t = at("abc", 0);
    t.backspace();
    EXPECT_EQ(t.text, "abc");
    EXPECT_EQ(t.cursor, 0u);
}

TEST(TextEdit, DeleteForwardRemovesAtCursor) {
    auto t = at("abc", 1);
    t.deleteForward();
    EXPECT_EQ(t.text, "ac");
    EXPECT_EQ(t.cursor, 1u);
}

// ── UTF-8: movement and deletion step whole codepoints ───────────────────────────
TEST(TextEdit, MovesByWholeCodepoint) {
    // "é" is 2 bytes (0xC3 0xA9); "aé b" → indices a=0, é=1..2, ' '=3, b=4
    TextEditState t; t.text = "aé b"; t.cursor = 0; t.anchor = 0;
    t.moveRight(false); EXPECT_EQ(t.cursor, 1u);   // past 'a'
    t.moveRight(false); EXPECT_EQ(t.cursor, 3u);   // past whole 'é' (2 bytes)
    t.moveLeft(false);  EXPECT_EQ(t.cursor, 1u);   // back over whole 'é'
}

TEST(TextEdit, BackspaceDeletesWholeCodepoint) {
    TextEditState t; t.text = "aé"; t.cursor = t.text.size(); t.anchor = t.cursor;
    t.backspace();
    EXPECT_EQ(t.text, "a") << "backspace must remove the full 2-byte codepoint";
}

// ── Line-aware Home / End ────────────────────────────────────────────────────────
TEST(TextEdit, HomeEndActOnCurrentLine) {
    auto t = at("ab\ncde", 5);            // cursor between 'd' and 'e' on line 2
    t.moveHome(false); EXPECT_EQ(t.cursor, 3u);   // start of "cde"
    t.moveEnd(false);  EXPECT_EQ(t.cursor, 6u);   // end of "cde"
}

// ── Vertical movement preserves column ───────────────────────────────────────────
TEST(TextEdit, UpDownPreserveColumn) {
    auto t = at("abcd\nefgh", 7);         // line 2, column 2 (between 'f' and 'g')
    t.moveUp(false);
    EXPECT_EQ(t.cursor, 2u) << "up keeps column 2 on line 1";
    t.moveDown(false);
    EXPECT_EQ(t.cursor, 7u) << "down returns to column 2 on line 2";
}

TEST(TextEdit, DownClampsToShorterLine) {
    auto t = at("longline\nhi", 6);       // column 6 on line 1
    t.moveDown(false);
    EXPECT_EQ(t.cursor, t.text.size()) << "clamps to end of the short second line";
}

TEST(TextEdit, UpOnFirstLineGoesToStart) {
    auto t = at("abc\ndef", 2);
    t.moveUp(false);
    EXPECT_EQ(t.cursor, 0u);
}

// ── Selection: extend, query, replace ────────────────────────────────────────────
TEST(TextEdit, ShiftRightExtendsSelection) {
    auto t = at("abcde", 1);
    t.moveRight(true);
    t.moveRight(true);
    EXPECT_TRUE(t.hasSelection());
    EXPECT_EQ(t.selBegin(), 1u);
    EXPECT_EQ(t.selEnd(), 3u);
    EXPECT_EQ(t.selectedText(), "bc");
}

TEST(TextEdit, SelectAllThenTypeReplaces) {
    auto t = at("old text", 3);
    t.selectAll();
    EXPECT_EQ(t.selectedText(), "old text");
    t.insert(std::string("new"));
    EXPECT_EQ(t.text, "new");
    EXPECT_EQ(t.cursor, 3u);
    EXPECT_FALSE(t.hasSelection());
}

TEST(TextEdit, BackspaceDeletesSelection) {
    auto t = at("abcde", 1);
    t.moveRight(true); t.moveRight(true);   // select "bc"
    t.backspace();
    EXPECT_EQ(t.text, "ade");
    EXPECT_EQ(t.cursor, 1u);
    EXPECT_FALSE(t.hasSelection());
}

TEST(TextEdit, PlainMoveCollapsesSelectionToEdge) {
    auto t = at("abcde", 1);
    t.moveRight(true); t.moveRight(true);   // select "bc", cursor at 3
    t.moveLeft(false);                      // collapse to selection start
    EXPECT_FALSE(t.hasSelection());
    EXPECT_EQ(t.cursor, 1u);

    auto u = at("abcde", 1);
    u.moveRight(true); u.moveRight(true);
    u.moveRight(false);                     // collapse to selection end
    EXPECT_FALSE(u.hasSelection());
    EXPECT_EQ(u.cursor, 3u);
}

TEST(TextEdit, InsertOverSelectionReplacesIt) {
    auto t = at("a[X]b", 1);
    t.setCursor(1, false); t.setCursor(4, true);   // select "[X]"
    EXPECT_EQ(t.selectedText(), "[X]");
    t.insert('Y');
    EXPECT_EQ(t.text, "aYb");
    EXPECT_EQ(t.cursor, 2u);
}

TEST(TextEdit, ClampFixesOutOfRangeIndices) {
    TextEditState t; t.text = "hi"; t.cursor = 99; t.anchor = 42;
    t.clamp();
    EXPECT_EQ(t.cursor, 2u);
    EXPECT_EQ(t.anchor, 2u);
}

// ── Word / line selection ─────────────────────────────────────────────────────────
TEST(TextEdit, SelectWordPicksWholeWord) {
    auto t = at("the quick brown", 6);   // inside "quick"
    t.selectWordAt(6);
    EXPECT_EQ(t.selectedText(), "quick");
    EXPECT_EQ(t.selBegin(), 4u);
    EXPECT_EQ(t.selEnd(), 9u);
}

TEST(TextEdit, SelectWordOnWhitespaceSelectsTheBlankRun) {
    auto t = at("a   b", 2);              // in the run of spaces
    t.selectWordAt(2);
    EXPECT_EQ(t.selectedText(), "   ");
}

TEST(TextEdit, SelectWordStopsAtPunctuation) {
    auto t = at("foo.bar", 5);           // inside "bar", '.' is a separate class
    t.selectWordAt(5);
    EXPECT_EQ(t.selectedText(), "bar");
}

TEST(TextEdit, SelectWordAtEndPicksLastWord) {
    std::string s = "hello";
    auto t = at(s, s.size());            // caret past the end
    t.selectWordAt(s.size());
    EXPECT_EQ(t.selectedText(), "hello");
}

TEST(TextEdit, SelectLinePicksCurrentLineWithoutNewline) {
    auto t = at("ab\ncde\nfg", 5);       // inside "cde"
    t.selectLineAt(5);
    EXPECT_EQ(t.selectedText(), "cde");
    EXPECT_EQ(t.selBegin(), 3u);
    EXPECT_EQ(t.selEnd(), 6u);
}

// ── Soft word-wrap ─────────────────────────────────────────────────────────────────
namespace {
// Fake monospace metric: 10px per codepoint (ASCII-only in these tests).
auto mono = [](const std::string& s) { return static_cast<float>(s.size()) * 10.0f; };
std::string seg(const std::string& t, const fbx::xkit::WrapSegment& s) {
    return t.substr(s.start, s.end - s.start);
}
}

TEST(WrapText, NoWrapWhenItFits) {
    std::string t = "hello";
    auto segs = fbx::xkit::wrapText(t, 100.0f, mono);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_EQ(seg(t, segs[0]), "hello");
}

TEST(WrapText, BreaksBetweenWords) {
    std::string t = "aa bb cc";            // 8 chars
    auto segs = fbx::xkit::wrapText(t, 55.0f, mono);   // fits 5 chars/line
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(seg(t, segs[0]), "aa ");
    EXPECT_EQ(seg(t, segs[1]), "bb cc");
}

TEST(WrapText, CharBreaksAWordTooWide) {
    std::string t = "abcdefg";             // no spaces, 7 chars
    auto segs = fbx::xkit::wrapText(t, 35.0f, mono);   // 3 chars/line
    ASSERT_EQ(segs.size(), 3u);
    EXPECT_EQ(seg(t, segs[0]), "abc");
    EXPECT_EQ(seg(t, segs[1]), "def");
    EXPECT_EQ(seg(t, segs[2]), "g");
}

TEST(WrapText, PreservesHardNewlines) {
    std::string t = "ab\ncd";
    auto segs = fbx::xkit::wrapText(t, 1000.0f, mono);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(seg(t, segs[0]), "ab");
    EXPECT_TRUE(segs[0].newline) << "first segment is newline-terminated";
    EXPECT_EQ(seg(t, segs[1]), "cd");
    EXPECT_FALSE(segs[1].newline);
}

TEST(WrapText, WrapWithinEachParagraph) {
    std::string t = "aa bb\ncc dd ee";      // two paragraphs
    auto segs = fbx::xkit::wrapText(t, 55.0f, mono);   // 5 chars/line
    // "aa bb" fits one line (newline-terminated); "cc dd ee" wraps to "cc " + "dd ee".
    ASSERT_EQ(segs.size(), 3u);
    EXPECT_EQ(seg(t, segs[0]), "aa bb");
    EXPECT_TRUE(segs[0].newline);
    EXPECT_EQ(seg(t, segs[1]), "cc ");
    EXPECT_EQ(seg(t, segs[2]), "dd ee");
}

// ── Undo history ────────────────────────────────────────────────────────────────
namespace {
using fbx::xkit::UndoHistory;
using fbx::xkit::EditSnapshot;
EditSnapshot snap(const std::string& s) { return {s, s.size(), s.size()}; }
}

TEST(UndoHistory, UndoAndRedoWalkTheStates) {
    UndoHistory h;
    h.push(snap(""));        // before typing "a"
    h.push(snap("a"));       // before typing "b"
    EditSnapshot cur = snap("ab"), out;

    ASSERT_TRUE(h.undo(cur, out));
    EXPECT_EQ(out.text, "a");
    cur = out;
    ASSERT_TRUE(h.undo(cur, out));
    EXPECT_EQ(out.text, "");
    cur = out;
    EXPECT_FALSE(h.undo(cur, out)) << "nothing left to undo";

    // Redo back up
    ASSERT_TRUE(h.redo(cur, out));
    EXPECT_EQ(out.text, "a");
    cur = out;
    ASSERT_TRUE(h.redo(cur, out));
    EXPECT_EQ(out.text, "ab");
    cur = out;
    EXPECT_FALSE(h.redo(cur, out));
}

TEST(UndoHistory, NewEditClearsRedo) {
    UndoHistory h;
    h.push(snap(""));
    EditSnapshot cur = snap("a"), out;
    ASSERT_TRUE(h.undo(cur, out));       // cur "a" → "", redo has "a"
    EXPECT_TRUE(h.canRedo());
    h.push(snap(""));                    // a fresh edit
    EXPECT_FALSE(h.canRedo()) << "a new edit must invalidate redo";
}

TEST(UndoHistory, EmptyStacksReportCorrectly) {
    UndoHistory h;
    EXPECT_FALSE(h.canUndo());
    EXPECT_FALSE(h.canRedo());
    EditSnapshot cur = snap("x"), out;
    EXPECT_FALSE(h.undo(cur, out));
    EXPECT_FALSE(h.redo(cur, out));
}

TEST(WrapText, EmptyAndBlankLines) {
    EXPECT_EQ(fbx::xkit::wrapText("", 100.0f, mono).size(), 1u);
    auto segs = fbx::xkit::wrapText("a\n\nb", 100.0f, mono);
    ASSERT_EQ(segs.size(), 3u);      // "a", "", "b"
    EXPECT_EQ(seg(std::string("a\n\nb"), segs[1]), "");
}
