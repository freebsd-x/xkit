// Unit tests for the pure tab-strip model (active-index bookkeeping).

#include "xkit/tab_model.h"
#include <gtest/gtest.h>

using fbx::xkit::TabModel;

// ── add / select ──────────────────────────────────────────────────────────────

TEST(TabModel, AddFirstBecomesActive) {
    TabModel m;
    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.active, -1);
    EXPECT_EQ(m.add("A"), 0);
    EXPECT_EQ(m.active, 0);
    EXPECT_EQ(m.add("B"), 1);
    EXPECT_EQ(m.active, 0);       // adding doesn't steal focus
    EXPECT_EQ(m.count(), 2);
}

TEST(TabModel, SelectClampsToRange) {
    TabModel m;
    m.add("A"); m.add("B"); m.add("C");
    m.select(2);
    EXPECT_EQ(m.active, 2);
    m.select(99);                 // out of range → ignored
    EXPECT_EQ(m.active, 2);
    m.select(-1);
    EXPECT_EQ(m.active, 2);
}

// ── close: active-index semantics ─────────────────────────────────────────────

TEST(TabModel, CloseTabLeftOfActiveShiftsDown) {
    TabModel m; m.add("A"); m.add("B"); m.add("C"); m.select(1);  // active=B
    m.close(0);                    // remove A
    EXPECT_EQ(m.count(), 2);
    EXPECT_EQ(m.titles[m.active], "B");   // still B, now at index 0
    EXPECT_EQ(m.active, 0);
}

TEST(TabModel, CloseTabRightOfActiveKeepsActive) {
    TabModel m; m.add("A"); m.add("B"); m.add("C"); m.select(1);  // active=B
    m.close(2);                    // remove C
    EXPECT_EQ(m.active, 1);
    EXPECT_EQ(m.titles[m.active], "B");
}

TEST(TabModel, CloseActiveSelectsNext) {
    TabModel m; m.add("A"); m.add("B"); m.add("C"); m.select(1);  // active=B
    m.close(1);                    // remove B (active)
    EXPECT_EQ(m.active, 1);
    EXPECT_EQ(m.titles[m.active], "C");   // the tab that shifted into the slot
}

TEST(TabModel, CloseActiveLastSelectsPrevious) {
    TabModel m; m.add("A"); m.add("B"); m.add("C"); m.select(2);  // active=C (last)
    m.close(2);
    EXPECT_EQ(m.active, 1);
    EXPECT_EQ(m.titles[m.active], "B");
}

TEST(TabModel, CloseOnlyTabLeavesNoneActive) {
    TabModel m; m.add("A"); m.select(0);
    m.close(0);
    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.active, -1);
}

TEST(TabModel, CloseOutOfRangeIsNoOp) {
    TabModel m; m.add("A"); m.add("B"); m.select(0);
    m.close(5);
    EXPECT_EQ(m.count(), 2);
    EXPECT_EQ(m.active, 0);
}

// ── activeAfterClose direct (the pure helper) ─────────────────────────────────

TEST(TabModel, ActiveAfterCloseTable) {
    using T = TabModel;
    EXPECT_EQ(T::activeAfterClose(1, 0, 0), -1);   // only tab
    EXPECT_EQ(T::activeAfterClose(3, 1, 0), 0);    // left of active
    EXPECT_EQ(T::activeAfterClose(3, 1, 2), 1);    // right of active
    EXPECT_EQ(T::activeAfterClose(3, 1, 1), 1);    // active, middle → next
    EXPECT_EQ(T::activeAfterClose(3, 2, 2), 1);    // active, last → prev
    EXPECT_EQ(T::activeAfterClose(2, 0, 0), 0);    // active=first of 2 → new first
}
