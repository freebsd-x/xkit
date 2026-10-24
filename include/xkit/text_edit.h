#pragma once
// text_edit.h — pure text-editing model for xkit's text fields.
//
// Holds a UTF-8 buffer, a cursor (insertion point) and a selection anchor, and
// the operations a caret needs: insert/delete at the cursor, codepoint-aware
// movement, line-aware Home/End/Up/Down, and shift-to-extend selection. No
// rendering or input plumbing, so it is exhaustively unit-testable.
//
// Indices are BYTE offsets into `text`. Movement never lands in the middle of a
// UTF-8 codepoint. The selection is the range [selBegin, selEnd); it is empty
// when anchor == cursor.

#include <string>
#include <algorithm>
#include <cstddef>
#include <vector>
#include <functional>

namespace fbx::xkit {

// One visual line produced by soft-wrapping. Rendered text is text[start, end).
// `newline` marks a segment terminated by a real '\n' at position `end` (that byte
// is not rendered and the next segment starts at end+1); a soft-wrap segment has
// newline=false and the next segment starts at `end`.
struct WrapSegment { std::size_t start, end; bool newline; };

namespace detail {
inline std::size_t wrapNextCp(const std::string& s, std::size_t i) noexcept {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}
}  // namespace detail

// ── Undo / redo ──────────────────────────────────────────────────────────────────
// A snapshot of the editable state, and two stacks of them. The element records a
// pre-edit checkpoint at the start of each undo group (coalescing runs of the same
// edit kind); undo/redo swap the live state against the stacks.
struct EditSnapshot { std::string text; std::size_t cursor = 0, anchor = 0; };
enum class EditKind { Insert, Delete, Boundary };

class UndoHistory {
public:
    // Record the pre-edit state as a new undo checkpoint; a fresh edit invalidates
    // the redo stack.
    void push(const EditSnapshot& pre) { undo_.push_back(pre); redo_.clear(); }
    [[nodiscard]] bool canUndo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !redo_.empty(); }
    void clear() noexcept { undo_.clear(); redo_.clear(); }

    // Move one step back/forward: stash `cur` on the opposite stack and return the
    // popped state in `out`. False (out untouched) when the stack is empty.
    bool undo(const EditSnapshot& cur, EditSnapshot& out) {
        if (undo_.empty()) return false;
        redo_.push_back(cur); out = undo_.back(); undo_.pop_back(); return true;
    }
    bool redo(const EditSnapshot& cur, EditSnapshot& out) {
        if (redo_.empty()) return false;
        undo_.push_back(cur); out = redo_.back(); redo_.pop_back(); return true;
    }
private:
    std::vector<EditSnapshot> undo_, redo_;
};

// Greedy word-wrap `text` to `avail` px, measuring candidate substrings with
// `measure`. Breaks between words; char-breaks a single word too wide to fit.
// avail <= 0 disables wrapping (splits on '\n' only). Always returns >= 1 segment.
inline std::vector<WrapSegment> wrapText(
    const std::string& text, float avail,
    const std::function<float(const std::string&)>& measure) {
    std::vector<WrapSegment> out;
    const std::size_t n = text.size();
    std::size_t lineStart = 0;
    while (true) {
        std::size_t nl = text.find('\n', lineStart);
        bool hasNl = (nl != std::string::npos);
        std::size_t lineEnd = hasNl ? nl : n;

        if (lineStart == lineEnd) {               // empty logical line
            out.push_back({lineStart, lineEnd, hasNl});
        } else if (avail <= 0.0f) {               // wrapping disabled
            out.push_back({lineStart, lineEnd, hasNl});
        } else {
            std::size_t segStart = lineStart;
            while (segStart < lineEnd) {
                std::size_t segEnd = segStart;
                // Extend token by token while it still fits.
                while (segEnd < lineEnd) {
                    std::size_t tokEnd = segEnd;
                    while (tokEnd < lineEnd && text[tokEnd] != ' ') ++tokEnd;
                    while (tokEnd < lineEnd && text[tokEnd] == ' ') ++tokEnd;
                    if (measure(text.substr(segStart, tokEnd - segStart)) <= avail)
                        segEnd = tokEnd;
                    else
                        break;
                }
                if (segEnd == segStart) {          // first word too wide → char-break
                    std::size_t e = detail::wrapNextCp(text, segStart);   // >= 1 codepoint
                    while (e < lineEnd) {
                        std::size_t j = detail::wrapNextCp(text, e);
                        if (measure(text.substr(segStart, j - segStart)) > avail) break;
                        e = j;
                    }
                    segEnd = e;
                }
                bool lastOfLine = (segEnd == lineEnd);
                out.push_back({segStart, segEnd, lastOfLine && hasNl});
                segStart = segEnd;
            }
        }
        if (!hasNl) break;
        lineStart = nl + 1;
    }
    if (out.empty()) out.push_back({0, 0, false});
    return out;
}

struct TextEditState {
    std::string text;
    std::size_t cursor = 0;   // insertion point, in [0, text.size()]
    std::size_t anchor = 0;   // selection anchor; == cursor means no selection

    // ── Selection queries ──────────────────────────────────────────────────────
    [[nodiscard]] bool        hasSelection() const noexcept { return anchor != cursor; }
    [[nodiscard]] std::size_t selBegin()     const noexcept { return std::min(anchor, cursor); }
    [[nodiscard]] std::size_t selEnd()       const noexcept { return std::max(anchor, cursor); }
    [[nodiscard]] std::string selectedText() const {
        return hasSelection() ? text.substr(selBegin(), selEnd() - selBegin()) : std::string{};
    }
    void clearSelection() noexcept { anchor = cursor; }
    void selectAll() noexcept { anchor = 0; cursor = text.size(); }

    // Keep indices valid (e.g. after the buffer is replaced externally).
    void clamp() noexcept {
        cursor = std::min(cursor, text.size());
        anchor = std::min(anchor, text.size());
    }

    // ── UTF-8 helpers ───────────────────────────────────────────────────────────
    // Byte index of the codepoint boundary before / after i.
    [[nodiscard]] std::size_t prevIndex(std::size_t i) const noexcept {
        if (i == 0) return 0;
        --i;
        while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) --i;
        return i;
    }
    [[nodiscard]] std::size_t nextIndex(std::size_t i) const noexcept {
        if (i >= text.size()) return text.size();
        ++i;
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) ++i;
        return i;
    }

    // ── Line helpers (lines split on '\n') ──────────────────────────────────────
    [[nodiscard]] std::size_t lineStart(std::size_t i) const noexcept {
        while (i > 0 && text[i - 1] != '\n') --i;
        return i;
    }
    [[nodiscard]] std::size_t lineEnd(std::size_t i) const noexcept {
        while (i < text.size() && text[i] != '\n') ++i;
        return i;
    }
    // Codepoint distance from the start of the cursor's line (the visual column
    // in a monospace sense; used to preserve column across Up/Down).
    [[nodiscard]] std::size_t column() const noexcept {
        std::size_t ls = lineStart(cursor), col = 0;
        for (std::size_t i = ls; i < cursor; i = nextIndex(i)) ++col;
        return col;
    }
    // Byte index `col` codepoints into the line beginning at `ls`, clamped to the
    // line's end.
    [[nodiscard]] std::size_t indexAtColumn(std::size_t ls, std::size_t col) const noexcept {
        std::size_t le = lineEnd(ls), i = ls;
        for (std::size_t c = 0; c < col && i < le; ++c) i = nextIndex(i);
        return i;
    }

    // ── Movement (extend = shift held → grow the selection) ─────────────────────
    void setCursor(std::size_t index, bool extend) noexcept {
        cursor = std::min(index, text.size());
        if (!extend) anchor = cursor;
    }
    void moveLeft(bool extend) noexcept {
        // Collapsing a selection leftwards jumps to its start, not one char back.
        if (hasSelection() && !extend) { cursor = selBegin(); anchor = cursor; return; }
        setCursor(prevIndex(cursor), extend);
    }
    void moveRight(bool extend) noexcept {
        if (hasSelection() && !extend) { cursor = selEnd(); anchor = cursor; return; }
        setCursor(nextIndex(cursor), extend);
    }
    void moveHome(bool extend) noexcept { setCursor(lineStart(cursor), extend); }
    void moveEnd(bool extend)  noexcept { setCursor(lineEnd(cursor),   extend); }
    void moveToStart(bool extend) noexcept { setCursor(0, extend); }
    void moveToEnd(bool extend)   noexcept { setCursor(text.size(), extend); }
    void moveUp(bool extend) noexcept {
        std::size_t ls = lineStart(cursor);
        if (ls == 0) { setCursor(0, extend); return; }       // already on first line
        std::size_t col = column();
        std::size_t prevLs = lineStart(ls - 1);
        setCursor(indexAtColumn(prevLs, col), extend);
    }
    void moveDown(bool extend) noexcept {
        std::size_t le = lineEnd(cursor);
        if (le == text.size()) { setCursor(text.size(), extend); return; } // last line
        std::size_t col = column();
        std::size_t nextLs = le + 1;                          // skip the '\n'
        setCursor(indexAtColumn(nextLs, col), extend);
    }

    // ── Editing ─────────────────────────────────────────────────────────────────
    // Remove the selection if there is one; returns true if anything was removed.
    bool deleteSelection() {
        if (!hasSelection()) return false;
        std::size_t b = selBegin(), e = selEnd();
        text.erase(b, e - b);
        cursor = b; anchor = b;
        return true;
    }
    void insert(const std::string& s) {
        deleteSelection();
        text.insert(cursor, s);
        cursor += s.size();
        anchor = cursor;
    }
    void insert(char c) { insert(std::string(1, c)); }
    void backspace() {
        if (deleteSelection()) return;
        if (cursor == 0) return;
        std::size_t prev = prevIndex(cursor);
        text.erase(prev, cursor - prev);
        cursor = prev; anchor = prev;
    }
    void deleteForward() {
        if (deleteSelection()) return;
        if (cursor >= text.size()) return;
        std::size_t nxt = nextIndex(cursor);
        text.erase(cursor, nxt - cursor);
        anchor = cursor;
    }

    // ── Word / line selection (double / triple click) ────────────────────────────
    // Character class for word segmentation: 1 = word (alnum, '_', or any non-ASCII
    // byte), 2 = blank (space/tab), 3 = other (punctuation). A "word" is a maximal
    // run of one class.
    static int charClass(unsigned char c) noexcept {
        if (c >= 0x80) return 1;                                   // UTF-8 lead/continuation
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') return 1;
        if (c == ' ' || c == '\t') return 2;
        return 3;
    }
    struct Range { std::size_t begin, end; };
    [[nodiscard]] Range wordBounds(std::size_t i) const noexcept {
        if (text.empty()) return {0, 0};
        if (i >= text.size()) i = text.size() - 1;
        int k = charClass(static_cast<unsigned char>(text[i]));
        std::size_t b = i, e = i + 1;
        while (b > 0 && charClass(static_cast<unsigned char>(text[b - 1])) == k) --b;
        while (e < text.size() && charClass(static_cast<unsigned char>(text[e])) == k) ++e;
        while (b > 0 && (static_cast<unsigned char>(text[b]) & 0xC0) == 0x80) --b;  // snap to codepoint
        return {b, e};
    }
    void selectWordAt(std::size_t i) noexcept {
        Range r = wordBounds(i);
        anchor = r.begin; cursor = r.end;
    }
    void selectLineAt(std::size_t i) noexcept {
        i = std::min(i, text.size());
        anchor = lineStart(i); cursor = lineEnd(i);
    }
};

}  // namespace fbx::xkit
