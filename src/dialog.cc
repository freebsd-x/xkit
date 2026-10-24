#include "xkit/dialog.h"
#include "xkit/overlay.h"
#include "xkit/widget.h"
#include "xkit/theme.h"

#include <memory>
#include <utility>

namespace fbx::xkit {

uint64_t Dialog::show(const std::string& title, const std::string& message,
                      std::vector<DialogAction> actions) noexcept {
    const Theme& th = ThemeManager::getInstance().getCurrentTheme();
    auto id_holder = std::make_shared<uint64_t>(0);

    auto title_w = std::make_shared<TextWidget>(title, 17.0f);
    title_w->setColor(th.text);
    auto msg_w = std::make_shared<TextWidget>(message, 13.0f);
    msg_w->setColor(th.text);

    // Action buttons, right-aligned along the bottom.
    std::vector<std::shared_ptr<Widget>> btns;
    btns.reserve(actions.size());
    for (auto& a : actions) {
        auto cb = a.on_tap;
        auto btn = std::make_shared<ButtonWidget>(a.label, [cb, id_holder]() {
            if (cb) cb();
            OverlayManager::instance().dismiss(*id_holder);
        });
        btn->setFontSize(13.0f);
        btn->setColor(a.primary ? th.accent : th.text);
        btns.push_back(std::move(btn));
    }
    auto btn_row = std::make_shared<FlexRow>(std::move(btns));
    btn_row->setGap(8.0f);
    btn_row->setMainAxisAlignment(MainAxisAlignment::End);

    auto card = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        title_w, msg_w, btn_row});
    card->setGap(12.0f);
    card->setPadding(20.0f);
    card->setBackgroundColor(th.surface);
    card->setBorderRadius(12.0f);
    card->setCrossAxisAlignment(CrossAxisAlignment::Stretch);  // so End-aligned row spans the card

    OverlaySpec spec;
    spec.content  = std::move(card);
    spec.centered = true;
    spec.modal    = true;
    *id_holder = OverlayManager::instance().show(std::move(spec));
    return *id_holder;
}

uint64_t Dialog::confirm(const std::string& title, const std::string& message,
                         const std::string& confirm_label,
                         std::function<void()> on_confirm,
                         std::function<void()> on_cancel) noexcept {
    std::vector<DialogAction> actions;
    actions.push_back({"Cancel", std::move(on_cancel), false});
    actions.push_back({confirm_label, std::move(on_confirm), true});
    return show(title, message, std::move(actions));
}

uint64_t Dialog::alert(const std::string& title, const std::string& message,
                       std::function<void()> on_ok) noexcept {
    std::vector<DialogAction> actions;
    actions.push_back({"OK", std::move(on_ok), true});
    return show(title, message, std::move(actions));
}

}  // namespace fbx::xkit
