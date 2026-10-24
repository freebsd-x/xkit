#include "xkit/toast.h"
#include "xkit/overlay.h"
#include "xkit/widget.h"
#include "xkit/theme.h"

#include <chrono>
#include <memory>
#include <vector>

namespace fbx::xkit {

uint64_t Toast::show(const std::string& message, int duration_ms) noexcept {
    const Theme& th = ThemeManager::getInstance().getCurrentTheme();

    auto label = std::make_shared<TextWidget>(message, 13.0f);
    label->setColor(th.text);
    auto pill = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{label});
    pill->setPadding(12.0f);
    pill->setBackgroundColor(th.surface);
    pill->setBorderRadius(10.0f);

    OverlaySpec spec;
    spec.content     = std::move(pill);
    spec.bottom      = true;    // bottom-centre
    spec.interactive = false;   // never blocks input
    spec.expires     = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(duration_ms);
    return OverlayManager::instance().show(std::move(spec));
}

}  // namespace fbx::xkit
