// Pure Gregorian date math for DatePickerWidget (no rendering).

#include "xkit/calendar.h"
#include <gtest/gtest.h>

using namespace fbx::xkit;
using namespace fbx::xkit::calendar;

TEST(Calendar, LeapYears) {
    EXPECT_TRUE(isLeapYear(2000));    // divisible by 400
    EXPECT_FALSE(isLeapYear(1900));   // divisible by 100, not 400
    EXPECT_TRUE(isLeapYear(2024));
    EXPECT_FALSE(isLeapYear(2023));
}

TEST(Calendar, DaysInMonth) {
    EXPECT_EQ(daysInMonth(2023, 2), 28);
    EXPECT_EQ(daysInMonth(2024, 2), 29);   // leap
    EXPECT_EQ(daysInMonth(2026, 4), 30);
    EXPECT_EQ(daysInMonth(2026, 12), 31);
    EXPECT_EQ(daysInMonth(2026, 0), 0);    // out of range
    EXPECT_EQ(daysInMonth(2026, 13), 0);
}

TEST(Calendar, DayOfWeekKnownDates) {
    EXPECT_EQ(dayOfWeek(1970, 1, 1), 4);   // Thursday
    EXPECT_EQ(dayOfWeek(2000, 1, 1), 6);   // Saturday
    EXPECT_EQ(dayOfWeek(2024, 2, 29), 4);  // Thursday
    EXPECT_EQ(dayOfWeek(2026, 10, 11), 0); // Sunday
}

TEST(Calendar, FirstWeekday) {
    EXPECT_EQ(firstWeekday(2026, 10), dayOfWeek(2026, 10, 1));
    EXPECT_EQ(firstWeekday(2026, 2), 0);   // Feb 2026 starts Sunday
}

TEST(Calendar, ClampToMonth) {
    EXPECT_EQ(clampToMonth(2023, 2, 31), (Date{2023, 2, 28}));  // Feb non-leap
    EXPECT_EQ(clampToMonth(2024, 2, 31), (Date{2024, 2, 29}));  // Feb leap
    EXPECT_EQ(clampToMonth(2026, 4, 0),  (Date{2026, 4, 1}));   // floor to 1
    EXPECT_EQ(clampToMonth(2026, 5, 15), (Date{2026, 5, 15}));  // in range
}

TEST(Calendar, AddMonthsWrapsYear) {
    EXPECT_EQ(addMonths(Date{2026, 12, 10}, 1),  (Date{2027, 1, 10}));   // Dec→Jan +yr
    EXPECT_EQ(addMonths(Date{2026, 1, 10}, -1),  (Date{2025, 12, 10}));  // Jan→Dec -yr
    EXPECT_EQ(addMonths(Date{2026, 6, 15}, 12),  (Date{2027, 6, 15}));   // +1 year
    EXPECT_EQ(addMonths(Date{2026, 1, 31}, 1),   (Date{2026, 2, 28}));   // day clamps
    EXPECT_EQ(addMonths(Date{2026, 3, 15}, -14), (Date{2025, 1, 15}));   // big neg span
}
