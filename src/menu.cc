#include "xkit/menu.h"
#include "xkit/overlay.h"
#include "xkit/widget.h"
#include "xkit/theme.h"

#include <memory>
#include <utility>

namespace fbx::xkit {

namespace {
// Build the menu card + register the overlay. Shared by showAt/showBelow.
uint64_t showMenu(overlay::Rect anchor, std::vector<MenuItem> items) noexcept {
    const Theme& th = ThemeManager::getInstance().getCurrentTheme();
    auto id_holder = std::make_shared<uint64_t>(0);

    std::vector<std::shared_ptr<Widget>> rows;
    rows.reserve(items.size());
    for (auto& it : items) {
        if (it.separator) {
            rows.push_back(std::make_shared<DividerWidget>(1.0f));
            continue;
        }
        auto cb = it.on_tap;
        auto btn = std::make_shared<ButtonWidget>(it.label, [cb, id_holder]() {
            if (cb) cb();
            OverlayManager::instance().dismiss(*id_holder);
        });
        btn->setFontSize(13.0f);
        btn->setColor(th.text);
        if (it.disabled) btn->setDisabled(true);   // dimmed, swallows the press
        rows.push_back(std::move(btn));
    }

    auto card = std::make_shared<FlexColumn>(std::move(rows));
    card->setGap(2.0f);
    card->setPadding(6.0f);
    card->setBackgroundColor(th.surface);
    card->setBorderRadius(8.0f);
    card->setCrossAxisAlignment(CrossAxisAlignment::Stretch);

    OverlaySpec spec;
    spec.content   = std::move(card);
    spec.anchor    = anchor;
    spec.preferred = overlay::Side::Below;   // non-modal, non-centered
    *id_holder = OverlayManager::instance().show(std::move(spec));
    return *id_holder;
}
}  // namespace

uint64_t Menu::showAt(float x, float y, std::vector<MenuItem> items) noexcept {
    // Zero-size anchor at the cursor: placePopup positions the menu at (x, y),
    // clamped/flipped to stay on screen.
    return showMenu(overlay::Rect{x, y, 0.0f, 0.0f}, std::move(items));
}

uint64_t Menu::showBelow(overlay::Rect anchor, std::vector<MenuItem> items) noexcept {
    return showMenu(anchor, std::move(items));
}

}  // namespace fbx::xkit
