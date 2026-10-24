#pragma once

// Pure Gregorian-calendar date math for DatePickerWidget — no rendering, no
// dependencies, so it is unit-testable on its own (see calendar_test.cc).

namespace fbx::xkit {

struct Date {
    int year{1970};
    int month{1};    // 1..12
    int day{1};      // 1..31
};

inline bool operator==(const Date& a, const Date& b) noexcept {
    return a.year == b.year && a.month == b.month && a.day == b.day;
}
inline bool operator!=(const Date& a, const Date& b) noexcept { return !(a == b); }

namespace calendar {

constexpr bool isLeapYear(int y) noexcept {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

// Number of days in a month (1..12); 0 for an out-of-range month.
constexpr int daysInMonth(int year, int month) noexcept {
    if (month < 1 || month > 12) return 0;
    constexpr int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && isLeapYear(year)) return 29;
    return d[month - 1];
}

// Day of week, 0=Sunday .. 6=Saturday (Sakamoto's algorithm). Valid for year≥1.
inline int dayOfWeek(int year, int month, int day) noexcept {
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year;
    if (month < 3) y -= 1;
    return (((y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7) + 7) % 7;
}

// Weekday the 1st of the month falls on (0=Sun).
inline int firstWeekday(int year, int month) noexcept {
    return dayOfWeek(year, month, 1);
}

// Clamp a day into its month's valid range (e.g. day 31 in Feb → 28/29).
inline Date clampToMonth(int year, int month, int day) noexcept {
    if (month < 1) month = 1;
    if (month > 12) month = 12;
    int dim = daysInMonth(year, month);
    if (day < 1) day = 1;
    if (dim && day > dim) day = dim;
    return Date{year, month, day};
}

// Add delta months (may be negative) with year carry; the day is clamped to the
// resulting month (Jan 31 + 1 month → Feb 28/29).
inline Date addMonths(Date d, int delta) noexcept {
    int idx = d.year * 12 + (d.month - 1) + delta;
    int y = idx / 12;
    int m = idx % 12;
    if (m < 0) { m += 12; y -= 1; }     // floor-divide for negative spans
    return clampToMonth(y, m + 1, d.day);
}

// English month name (1..12); empty for out of range.
inline const char* monthName(int month) noexcept {
    static const char* n[12] = {"January", "February", "March", "April", "May",
                                "June", "July", "August", "September",
                                "October", "November", "December"};
    return (month >= 1 && month <= 12) ? n[month - 1] : "";
}

}  // namespace calendar
}  // namespace fbx::xkit
