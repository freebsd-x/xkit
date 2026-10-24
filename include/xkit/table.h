#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <numeric>
#include <string>
#include <vector>

namespace fbx::xkit {

// Horizontal alignment of a column's cell text.
enum class CellAlign { Left, Center, Right };

// A table column: a header title, a fixed pixel width, and cell alignment
// (numeric columns usually read better right-aligned).
struct TableColumn {
    std::string title;
    float       width = 120.0f;
    CellAlign   align = CellAlign::Left;
};

namespace table {

// True if s parses fully as a number (so a column of numbers sorts numerically,
// not lexicographically — "10" after "9", not before).
inline bool isNumber(const std::string& s, double& out) noexcept {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0';
}

// Three-way compare of two cells: numeric when both are numbers, else string.
inline int compareCells(const std::string& a, const std::string& b) noexcept {
    double na, nb;
    if (isNumber(a, na) && isNumber(b, nb))
        return (na < nb) ? -1 : (na > nb) ? 1 : 0;
    return a < b ? -1 : (a > b) ? 1 : 0;
}

// Display order (row indices) sorted by column `col`, ascending or descending.
// Stable, so equal keys keep their original order. col < 0 or out of range →
// the identity order (no sort).
inline std::vector<size_t> sortedOrder(const std::vector<std::vector<std::string>>& rows,
                                       int col, bool ascending) noexcept {
    std::vector<size_t> order(rows.size());
    std::iota(order.begin(), order.end(), size_t{0});
    if (col < 0) return order;

    auto cell = [&](size_t r) -> const std::string& {
        static const std::string kEmpty;
        const auto& row = rows[r];
        return (static_cast<size_t>(col) < row.size()) ? row[static_cast<size_t>(col)] : kEmpty;
    };
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        int c = compareCells(cell(a), cell(b));
        return ascending ? c < 0 : c > 0;
    });
    return order;
}

}  // namespace table
}  // namespace fbx::xkit
