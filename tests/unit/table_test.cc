// Tests for Table/DataGrid: pure sort logic + self-managed sort + tap routing.

#include "xkit/table.h"
#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include "xkit/application.h"
#include "shared/protocol.h"
#include <gtest/gtest.h>
#include <memory>

using namespace fbx::xkit;
namespace shared = fbx::shared;

namespace {
shared::InputMessage press(int x, int y) {
    shared::InputMessage m{};
    m.type_ = shared::InputEventType::MouseButton;
    m.char_value_ = 1;
    m.value_a_ = x;
    m.value_b_ = y;
    m.modifiers_ = 272u << 16;   // left
    return m;
}
shared::InputMessage shiftWheel(int lines) {
    shared::InputMessage m{};
    m.type_ = shared::InputEventType::MouseWheel;
    m.value_b_ = lines;
    m.modifiers_ = 1u;   // Shift
    return m;
}
using Rows = std::vector<std::vector<std::string>>;
}  // namespace

// ── cell alignment (the fixed-width FlexRow mechanism cells use) ──────────────

TEST(TableAlign, ColumnStoresAlignment) {
    TableColumn c{"CPU", 80.0f, CellAlign::Right};
    EXPECT_EQ(c.align, CellAlign::Right);
    TableColumn d{"Name", 100.0f};
    EXPECT_EQ(d.align, CellAlign::Left);   // default
}

TEST(TableAlign, FixedWidthRowAlignsChild) {
    // A 100-wide row with a 40-wide child: Start→0, Center→30, End→60.
    auto mk = [](MainAxisAlignment a) {
        auto row = std::make_shared<RenderFlexRow>();
        row->setFixedSize(100.0f, 20.0f);
        row->setMainAxisAlignment(a);
        auto child = std::make_shared<RenderBox>(40.0f, 20.0f);
        row->addChild(child);
        (void)row->layout(Constraints{0, 200, 0, 200});
        return child->getOffset().x_;
    };
    EXPECT_FLOAT_EQ(mk(MainAxisAlignment::Start), 0.0f);
    EXPECT_FLOAT_EQ(mk(MainAxisAlignment::Center), 30.0f);
    EXPECT_FLOAT_EQ(mk(MainAxisAlignment::End), 60.0f);
}

// ── RenderHScroll (the horizontal scroll primitive) ───────────────────────────

TEST(HScroll, ClampsAndTranslates) {
    auto hs = std::make_shared<RenderHScroll>();
    auto child = std::make_shared<RenderBox>(1000.0f, 50.0f);   // wider than the view
    hs->addChild(child);

    (void)hs->layout(Constraints{0, 300, 0, 300});
    EXPECT_FLOAT_EQ(hs->getSize().width_, 300.0f);   // clamped to the available width
    EXPECT_FLOAT_EQ(child->getOffset().x_, 0.0f);    // no scroll yet

    hs->setScrollX(200.0f);
    (void)hs->layout(Constraints{0, 300, 0, 300});
    EXPECT_FLOAT_EQ(child->getOffset().x_, -200.0f); // translated left

    hs->setScrollX(99999.0f);                         // past the end
    (void)hs->layout(Constraints{0, 300, 0, 300});
    EXPECT_FLOAT_EQ(child->getOffset().x_, -(1000.0f - 300.0f));  // clamped to max
}

// ── pure sort logic ───────────────────────────────────────────────────────────

TEST(TableSort, NumericColumnSortsNumerically) {
    Rows r{{"Bob", "30"}, {"Al", "9"}, {"Cy", "10"}};
    auto asc = table::sortedOrder(r, 1, true);       // by Age
    // 9, 10, 30 → rows 1, 2, 0 (not lexicographic "10" < "30" < "9")
    EXPECT_EQ(asc, (std::vector<size_t>{1, 2, 0}));
    auto desc = table::sortedOrder(r, 1, false);
    EXPECT_EQ(desc, (std::vector<size_t>{0, 2, 1}));
}

TEST(TableSort, StringColumnLexicographic) {
    Rows r{{"Bob", "30"}, {"Al", "9"}, {"Cy", "10"}};
    auto asc = table::sortedOrder(r, 0, true);       // by Name
    EXPECT_EQ(asc, (std::vector<size_t>{1, 0, 2}));  // Al, Bob, Cy
}

TEST(TableSort, StableAndIdentityForNoColumn) {
    Rows r{{"a"}, {"a"}, {"a"}};
    EXPECT_EQ(table::sortedOrder(r, 0, true), (std::vector<size_t>{0, 1, 2}));  // stable
    EXPECT_EQ(table::sortedOrder(r, -1, true), (std::vector<size_t>{0, 1, 2})); // no sort
    EXPECT_EQ(table::sortedOrder(r, 9, true), (std::vector<size_t>{0, 1, 2}));  // out of range col → empty cells, stable
}

TEST(TableSort, CompareCellsNumericVsString) {
    EXPECT_LT(table::compareCells("9", "10"), 0);    // numeric
    EXPECT_GT(table::compareCells("9", "10x"), 0);   // "9" > "10x" lexicographically
    EXPECT_EQ(table::compareCells("x", "x"), 0);
}

// ── self-managed sort state ───────────────────────────────────────────────────

TEST(TableWidget_, ToggleSortDirection) {
    TableWidget t({{"A", 100}, {"B", 100}}, {{"1", "2"}}, nullptr);
    EXPECT_EQ(t.sortCol(), -1);
    t.toggleSort(1);
    EXPECT_EQ(t.sortCol(), 1);
    EXPECT_TRUE(t.ascending());
    t.toggleSort(1);                 // same column → flip
    EXPECT_FALSE(t.ascending());
    t.toggleSort(0);                 // new column → asc
    EXPECT_EQ(t.sortCol(), 0);
    EXPECT_TRUE(t.ascending());
}

// ── element: header tap sorts, row tap reports original index ──────────────────

TEST(TableWidget_, HeaderTapSortsAndRowTapReportsOriginalIndex) {
    int tapped = -1;
    auto tbl = std::make_shared<TableWidget>(
        std::vector<TableColumn>{{"Name", 100}, {"Age", 100}},
        Rows{{"Bob", "30"}, {"Al", "9"}, {"Cy", "10"}},
        [&](int orig) { tapped = orig; });
    auto el = tbl->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 500, 0, 500});

    // Tap the "Age" header (column 1, x in [100,200), header y in [0,30)).
    EXPECT_TRUE(el->handleEvent(press(150, 15)));
    EXPECT_EQ(tbl->sortCol(), 1);
    EXPECT_TRUE(tbl->ascending());

    el->syncState();                                   // rebuild in sorted order
    (void)el->getRenderNode()->layout(Constraints{0, 500, 0, 500});

    // Sorted by Age asc: Al(9,row1), Cy(10,row2), Bob(30,row0). First data row is
    // "Al" (original index 1). Rows: header(30) + divider(1) → row 0 at y≈31..59.
    EXPECT_TRUE(el->handleEvent(press(50, 45)));
    EXPECT_EQ(tapped, 1);                               // original index of "Al"
    EXPECT_EQ(tbl->selectedRow(), 1);                   // tap self-selects that row
}

TEST(TableWidget_, SetSelectedRowSeedsSelection) {
    TableWidget t({{"A", 100}}, {{"x"}, {"y"}, {"z"}}, nullptr);
    EXPECT_EQ(t.selectedRow(), -1);                     // none by default
    t.setSelectedRow(2);
    EXPECT_EQ(t.selectedRow(), 2);
}

TEST(TableWidget_, ShiftWheelScrollsHorizontally) {
    // Columns total 600 wide, shown in a 250-wide pane → horizontally scrollable.
    auto tbl = std::make_shared<TableWidget>(
        std::vector<TableColumn>{{"A", 200}, {"B", 200}, {"C", 200}},
        Rows{{"1", "2", "3"}, {"4", "5", "6"}}, nullptr);
    tbl->setSortable(false);
    auto el = tbl->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 250, 0, 300});

    auto hs = std::static_pointer_cast<RenderHScroll>(el->getRenderNode());
    EXPECT_FLOAT_EQ(hs->getScrollX(), 0.0f);
    EXPECT_TRUE(el->handleEvent(shiftWheel(-3)));    // shift+wheel consumed
    EXPECT_GT(hs->getScrollX(), 0.0f);               // scrolled right
}

TEST(TableWidget_, TapWithoutOnRowTapStillSelects) {
    auto tbl = std::make_shared<TableWidget>(
        std::vector<TableColumn>{{"A", 100}},
        Rows{{"one"}, {"two"}}, nullptr);              // no onRowTap
    tbl->setSelectedRow(0);                             // make row 0 a button (highlighted)
    auto el = tbl->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 500, 0, 500});
    // Row 0 is selectable/highlighted → it's a button; tapping it selects it.
    // header(30)+divider(1) → row 0 at y≈31..59.
    EXPECT_TRUE(el->handleEvent(press(20, 45)));
    EXPECT_EQ(tbl->selectedRow(), 0);
}
