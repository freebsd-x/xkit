// Integration test for DatePickerWidget on the overlay layer: a press opens a
// month-grid calendar; picking a day fires onChange + dismisses; ‹ › page the
// month. Drives the element + widget logic directly (no compositor).

#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include "xkit/overlay.h"
#include "shared/protocol.h"
#include <gtest/gtest.h>
#include <memory>
#include <cctype>

using namespace fbx::xkit;
namespace shared = fbx::shared;

namespace {
shared::InputMessage mousePress(int x, int y) {
    shared::InputMessage m{};
    m.type_ = shared::InputEventType::MouseButton;
    m.char_value_ = 1;
    m.value_a_ = x; m.value_b_ = y;
    return m;
}
std::shared_ptr<FlexColumn> openCard() {
    return std::dynamic_pointer_cast<FlexColumn>(
        OverlayManager::instance().overlays().back().content);
}
// Day cells are numeric-labelled ButtonWidgets in the week rows (children[2..]);
// blanks are Spacers, and the ‹/› nav buttons live in the header row (0).
std::vector<std::shared_ptr<ButtonWidget>> dayButtons(const std::shared_ptr<FlexColumn>& col) {
    std::vector<std::shared_ptr<ButtonWidget>> out;
    for (size_t r = 2; r < col->childCount(); ++r) {
        auto row = std::dynamic_pointer_cast<FlexRow>(col->childAt(r));
        if (!row) continue;
        for (size_t c = 0; c < row->childCount(); ++c)
            if (auto b = std::dynamic_pointer_cast<ButtonWidget>(row->childAt(c)))
                if (!b->getText().empty() && std::isdigit((unsigned char)b->getText()[0]))
                    out.push_back(b);
    }
    return out;
}
std::shared_ptr<ButtonWidget> dayBtn(const std::shared_ptr<FlexColumn>& col, const std::string& label) {
    for (auto& b : dayButtons(col)) if (b->getText() == label) return b;
    return nullptr;
}
}  // namespace

class DatePickerTest : public ::testing::Test {
protected:
    void SetUp() override    { OverlayManager::instance().dismissAll(); }
    void TearDown() override { OverlayManager::instance().dismissAll(); }
};

TEST_F(DatePickerTest, FormatsValueIsoAndCustom) {
    DatePickerWidget d(Date{2026, 10, 11});
    EXPECT_EQ(d.formatValue(), "2026-10-11");
    EXPECT_EQ(d.value(), (Date{2026, 10, 11}));
    d.setFormatter([](Date x){ return std::to_string(x.day) + "/" + std::to_string(x.month); });
    EXPECT_EQ(d.formatValue(), "11/10");
}

TEST_F(DatePickerTest, CtorClampsOutOfRangeDay) {
    DatePickerWidget d(Date{2023, 2, 31});   // Feb non-leap
    EXPECT_EQ(d.value(), (Date{2023, 2, 28}));
}

TEST_F(DatePickerTest, PressOpensCalendarWithCorrectDayCount) {
    auto dp = std::make_shared<DatePickerWidget>(Date{2026, 2, 10});  // Feb 2026 = 28 days
    auto el = dp->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 500, 0, 500});

    ASSERT_TRUE(OverlayManager::instance().empty());
    EXPECT_TRUE(el->handleEvent(mousePress(10, 10)));
    ASSERT_EQ(OverlayManager::instance().count(), 1u);

    auto col = openCard();
    ASSERT_TRUE(col);
    EXPECT_EQ(dayButtons(col).size(), 28u);   // one button per day of the month
    ASSERT_TRUE(dayBtn(col, "1"));
    ASSERT_TRUE(dayBtn(col, "28"));
    EXPECT_FALSE(dayBtn(col, "29"));          // Feb 2026 has no 29th
}

TEST_F(DatePickerTest, PickingDayFiresOnChangeAndDismisses) {
    Date got{}; int fired = 0;
    auto dp = std::make_shared<DatePickerWidget>(Date{2026, 10, 1},
        [&](Date d){ got = d; ++fired; });
    auto el = dp->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 500, 0, 500});
    el->handleEvent(mousePress(10, 10));       // open

    auto b15 = dayBtn(openCard(), "15");
    ASSERT_TRUE(b15);
    ASSERT_TRUE(b15->onPressed());
    b15->onPressed()();                        // pick the 15th

    EXPECT_EQ(fired, 1);
    EXPECT_EQ(got, (Date{2026, 10, 15}));
    EXPECT_EQ(dp->value(), (Date{2026, 10, 15}));   // self-managed
    EXPECT_TRUE(OverlayManager::instance().empty()); // dismissed
}

TEST_F(DatePickerTest, NavNextPagesToFollowingMonth) {
    auto dp = std::make_shared<DatePickerWidget>(Date{2026, 2, 10});  // Feb (28)
    auto el = dp->createElement();
    el->mount(nullptr);
    (void)el->getRenderNode()->layout(Constraints{0, 500, 0, 500});
    el->handleEvent(mousePress(10, 10));
    EXPECT_EQ(dayButtons(openCard()).size(), 28u);

    // Header row = [‹, title, ›]; click › to page to March (31 days).
    auto header = std::dynamic_pointer_cast<FlexRow>(openCard()->childAt(0));
    ASSERT_TRUE(header);
    auto nextBtn = std::dynamic_pointer_cast<ButtonWidget>(header->childAt(2));
    ASSERT_TRUE(nextBtn && nextBtn->onPressed());
    nextBtn->onPressed()();

    ASSERT_EQ(OverlayManager::instance().count(), 1u);   // still exactly one calendar
    EXPECT_EQ(dayButtons(openCard()).size(), 31u);       // March 2026
    EXPECT_EQ(dp->value(), (Date{2026, 2, 10}));         // navigating doesn't pick
}
