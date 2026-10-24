// dogfood_render — pixel-proof the dogfooded widgets.
//
// Renders ExpanderWidget (expanded + collapsed, showing the ▾/▸ caret),
// StepperWidget ([− value +]), and DatePickerWidget (trigger AND its opened
// month-grid popup) through the real VulkanEngine and writes PPMs so the
// appearance can be eyed directly — the controls the live Settings/Calendar use,
// isolated from their services and theme.
//
// Run:  FREEBSDX_SHADER_DIR=out/freebsdx.p LD_LIBRARY_PATH=./out \
//         ./out/sdk/xkit/xkit_dogfood_render
// Outputs /tmp/dogfood.ppm (overview) and /tmp/dogfood-popup.ppm (date popup).

#include "xkit/engine.h"
#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include "xkit/calendar.h"
#include "xkit/overlay.h"
#include "shared/protocol.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace fbx::xkit;

static std::shared_ptr<TextWidget> label(const char* s, float sz, Color c) {
    auto t = std::make_shared<TextWidget>(s, sz);
    t->setColor(c);
    return t;
}

int main() {
    constexpr uint32_t W = 560, H = 680;
    VulkanEngine eng;
    if (!eng.init(W, H)) {
        std::fprintf(stderr, "dogfood_render: engine init failed (no GPU?)\n");
        return 2;
    }

    // Paint a widget subtree and write the pixels to a PPM (BGRA8 → RGB).
    auto dump = [&](const std::shared_ptr<Widget>& root, const char* path) -> bool {
        auto el = root->createElement();
        el->mount(nullptr);
        auto node = el->getRenderNode();
        (void)node->layout(Constraints{0, (float)W, 0, (float)H});
        eng.paint(node);
        auto buf = eng.readbackPixels(W, H);
        if (buf.size() != static_cast<size_t>(W) * H) {
            std::fprintf(stderr, "dogfood_render: readback failed for %s\n", path);
            return false;
        }
        FILE* f = std::fopen(path, "wb");
        if (!f) { std::fprintf(stderr, "dogfood_render: cannot open %s\n", path); return false; }
        std::fprintf(f, "P6\n%u %u\n255\n", W, H);
        for (uint32_t p : buf) {
            unsigned char rgb[3] = {
                static_cast<unsigned char>((p >> 16) & 0xFF),
                static_cast<unsigned char>((p >> 8)  & 0xFF),
                static_cast<unsigned char>( p        & 0xFF),
            };
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
        std::fprintf(stderr, "dogfood_render: wrote %s\n", path);
        return true;
    };

    // ── Overview: light card + dark text (how Settings uses these widgets). ──────
    const Color card {0.96f, 0.97f, 0.98f, 1.0f};
    const Color ink  {0.14f, 0.16f, 0.20f, 1.0f};
    const Color gray {0.40f, 0.43f, 0.48f, 1.0f};
    const Color blue {0.10f, 0.40f, 0.85f, 1.0f};

    auto exp1 = std::make_shared<ExpanderWidget>(
        label("APPEARANCE", 15.0f, ink),
        std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
            label("Dark Mode", 13.0f, gray),
            label("Accent Colour", 13.0f, gray),
        }), /*expanded=*/true);

    auto exp2 = std::make_shared<ExpanderWidget>(
        label("DISPLAY", 15.0f, ink),
        std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
            label("Resolution", 13.0f, gray),
        }), /*expanded=*/false);

    auto stepper = std::make_shared<StepperWidget>(2.0f, 0.0f, 4.0f, 1.0f);
    stepper->setFormatter([](float v) {
        static const char* kOpts[] = {"Never", "5 m", "15 m", "30 m", "1 hr"};
        int i = static_cast<int>(v);
        return std::string((i >= 0 && i < 5) ? kOpts[i] : "?");
    });
    auto sleepRow = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        label("Sleep After", 14.0f, ink), std::make_shared<Spacer>(24, 1), stepper,
    });

    auto dp = std::make_shared<DatePickerWidget>(Date{2026, 10, 18});
    auto dateRow = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        label("Date", 14.0f, ink), std::make_shared<Spacer>(24, 1), dp,
    });

    auto heading = [&](const char* s) { return label(s, 11.0f, blue); };
    auto col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        heading("ExpanderWidget (expanded)"),  exp1,
        std::make_shared<Spacer>(1, 16),
        heading("ExpanderWidget (collapsed)"), exp2,
        std::make_shared<Spacer>(1, 16),
        heading("StepperWidget"),              sleepRow,
        std::make_shared<Spacer>(1, 16),
        heading("DatePickerWidget"),           dateRow,
    });
    col->setPadding(28);
    auto overview = std::make_shared<ContainerWidget>(col);
    overview->setFixedSize((float)W, (float)H);
    overview->setBackgroundColor(card);
    dump(overview, "/tmp/dogfood.ppm");

    // ── DatePicker popup: open the month grid and render it. ─────────────────────
    // A MouseButton press on the DatePicker element calls openCalendar(), which
    // registers the month-grid card with the OverlayManager. The default theme is
    // light (white surface / dark text), so the card is self-visible.
    OverlayManager::instance().dismissAll();
    auto dp2 = std::make_shared<DatePickerWidget>(Date{2026, 10, 18});
    auto dp2El = dp2->createElement();
    dp2El->mount(nullptr);
    auto dp2Node = dp2El->getRenderNode();
    (void)dp2Node->layout(Constraints{0, 200.0f, 0, 40.0f});
    fbx::shared::InputMessage ev{};
    ev.type_ = fbx::shared::InputEventType::MouseButton;
    ev.char_value_ = 1;   // pressed (DatePickerElement opens on char_value_ != 0)
    (void)dp2El->handleEvent(ev);

    auto& ovs = OverlayManager::instance().overlays();
    if (!ovs.empty() && ovs.back().content) {
        dump(ovs.back().content, "/tmp/dogfood-popup.ppm");
    } else {
        std::fprintf(stderr, "dogfood_render: no overlay — popup did not open\n");
        return 5;
    }

    // ── Dropdown popup: open the options menu and render it. ─────────────────────
    OverlayManager::instance().dismissAll();
    auto dd = std::make_shared<DropdownWidget>(
        std::vector<std::string>{"Cover", "Contain", "Stretch", "Center"}, 0);
    auto ddEl = dd->createElement();
    ddEl->mount(nullptr);
    (void)ddEl->getRenderNode()->layout(Constraints{0, 200.0f, 0, 40.0f});
    fbx::shared::InputMessage dev{};
    dev.type_ = fbx::shared::InputEventType::MouseButton;
    dev.char_value_ = 1;   // pressed → DropdownElement::openPopup()
    (void)ddEl->handleEvent(dev);
    auto& dov = OverlayManager::instance().overlays();
    if (!dov.empty() && dov.back().content) {
        dump(dov.back().content, "/tmp/dogfood-dropdown.ppm");
    } else {
        std::fprintf(stderr, "dogfood_render: dropdown popup did not open\n");
        return 6;
    }
    return 0;
}
