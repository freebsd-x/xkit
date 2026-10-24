#pragma once

#include <string>
#include <utility>
#include <vector>

namespace fbx::xkit {

// Pure model for a tab strip: the ordered tab titles plus which one is active,
// with the active-index bookkeeping that tabbed apps (Browser, Terminal, …)
// routinely reimplement and get subtly wrong when closing tabs. Dependency-free
// and header-only so it is unit-testable and usable by any app or by TabBarWidget.
struct TabModel {
    std::vector<std::string> titles;
    int active = -1;   // index of the active tab; -1 when there are none

    [[nodiscard]] int  count() const noexcept { return static_cast<int>(titles.size()); }
    [[nodiscard]] bool empty() const noexcept { return titles.empty(); }

    // Append a tab; the first tab added becomes active. Returns its index.
    int add(std::string title) noexcept {
        titles.push_back(std::move(title));
        if (active < 0) active = 0;
        return count() - 1;
    }

    // Make tab i active (ignored if out of range).
    void select(int i) noexcept {
        if (i >= 0 && i < count()) active = i;
    }

    // Close tab i, fixing `active` with the conventional browser semantics
    // (see activeAfterClose). No-op if i is out of range.
    void close(int i) noexcept {
        if (i < 0 || i >= count()) return;
        int before = count();
        titles.erase(titles.begin() + i);
        active = activeAfterClose(before, active, i);
    }

    // New active index after closing `closed` from a strip of `n` tabs whose
    // active tab was `active`. Pure and static so it can be tested directly:
    //   • closing a tab left of active shifts active down by one
    //   • closing a tab right of active leaves it unchanged
    //   • closing the active tab keeps the same slot (the tab that shifts in,
    //     i.e. the next one), clamped to the new last tab
    //   • closing the only tab leaves no active tab (-1)
    [[nodiscard]] static int activeAfterClose(int n, int active, int closed) noexcept {
        if (n <= 1) return -1;
        int new_n = n - 1;
        if (closed < active) return active - 1;
        if (closed > active) return active;
        return active < new_n ? active : new_n - 1;   // closed == active
    }
};

}  // namespace fbx::xkit
