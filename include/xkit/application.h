#pragma once

#include "xkit/widget.h"
#include "xkit/xbus_client.h"
#include <memory>
#include <string>
#include <functional>

namespace fbx::xkit {

class XApplication {
public:
    using InitCallback = std::function<void(XBusClient&)>;

    // Bootstraps the application, connects to the compositor, and enters the event loop.
    // If 'sandboxed' is true, drops into Capsicum capability mode after connecting.
    // If 'is_shell' is true, registers as a shell overlay in the compositor.
    // If 'transparent_bg' is true, the surface is cleared to transparent instead of
    // the opaque window background — the desktop shell uses this so its empty desktop
    // area lets app windows (stacked below it) show through, while its panels/overlays
    // stay opaque. Left false for apps and the greeter (opaque backgrounds).
    // shell_surface_id: the surface id used when is_shell is true. Defaults to the
    // desktop shell id (999). The lock screen passes a distinct id (998) so it
    // doesn't collide with the still-running desktop's surface state.
    static int Run(std::shared_ptr<Widget> rootWidget, const std::string& socket_path = "/tmp/freebsdx.sock", bool sandboxed = false, bool is_shell = false, InitCallback on_init = nullptr, bool transparent_bg = false, uint32_t shell_surface_id = 999) noexcept;

    static void requestFocus(std::shared_ptr<Element> element) noexcept;
    static std::shared_ptr<Element> getFocusedElement() noexcept;

    // ── Pointer capture ────────────────────────────────────────────────────
    // While an element holds the pointer, every mouse event is routed straight
    // to it — bypassing focus and spatial hit-testing — so a drag keeps working
    // when the cursor leaves the element's bounds (e.g. a splitter or slider).
    // Capture is released explicitly, or automatically on the next button-up.
    // For MouseButton events the raw button state is preserved in the message's
    // char_value_ field (1 = down, 0 = up) since the coordinate injection
    // overwrites value_a_/value_b_.
    static void capturePointer(std::shared_ptr<Element> element) noexcept;
    static void releasePointer() noexcept;
    static std::shared_ptr<Element> getPointerCapture() noexcept;

    // Last known pointer position (window coords). Handy for opening a context
    // menu at the cursor, positioning a tooltip, etc.
    static void cursorPosition(float& x, float& y) noexcept;

    // Which pointer button a MouseButton event carries (left/right/middle). The
    // compositor delivers the raw button code; Run() packs it into the event so
    // widgets can distinguish clicks (e.g. right-click for a context menu).
    enum class PointerButton : uint32_t { Unknown = 0, Left = 272, Right = 273, Middle = 274 };
    static PointerButton pointerButton(const shared::InputMessage& event) noexcept;

    // Returns the global XBusClient if the application is connected.
    static XBusClient* getBus() noexcept;

    // Set the window title bar text.
    static void setTitle(const std::string& title) noexcept;

    // ── Drag and Drop ──────────────────────────────────────────────────────
    // Call from within a press/gesture handler to begin a content drag.
    // mime_type: "text/uri-list" or "text/plain".  data: the payload string.
    static void startDrag(const std::string& mime_type,
                          const std::string& data) noexcept;

    // Register a drop handler for a specific MIME type.  The callback is
    // invoked when another app drops compatible data onto this window.
    // (x, y) is the drop position in logical pixels within this surface.
    static void setOnDrop(const std::string& mime_type,
                          std::function<void(float x, float y,
                                            const std::string& data)> cb) noexcept;

    // Register a global keyboard-shortcut handler, checked on every KeyPress
    // BEFORE the event is routed to the focused element. Return true to consume
    // the event (e.g. Ctrl+S for save) so a focused text field never sees it.
    static void setOnShortcut(std::function<bool(const fbx::shared::InputMessage&)> cb) noexcept;

    // ── Gamepad ────────────────────────────────────────────────────────────
    // Register a callback invoked for every GamepadButton or GamepadAxis
    // event forwarded by the compositor to this app.
    // is_axis: true → axis event; false → button event.
    // code: BTN_* evdev code (buttons) or logical axis ID (axes: 0=LX,1=LY,
    //       2=RX,3=RY,4=LT,5=RT,6=DpadX,7=DpadY).
    // value: buttons: 1=pressed/0=released; axes: -1000 to +1000.
    // controller_idx: 0-based gamepad index.
    struct GamepadEvent {
        bool     is_axis;
        uint32_t code;
        int32_t  value;
        uint32_t controller_idx;
    };
    static void onGamepadEvent(std::function<void(const GamepadEvent&)> cb) noexcept;

    // ── Accessibility ──────────────────────────────────────────────────────
    // Push a UTF-8 text string to any connected screen reader.
    // Call this for live-region updates (alerts, status messages, etc.).
    // Also called automatically by requestFocus() when an element is focused.
    static void announce(const std::string& text) noexcept;

    // Serialise the element tree rooted at `root` and push it to the compositor
    // so screen readers can query the navigable widget hierarchy.
    static void updateAccessibilityTree(const std::shared_ptr<Element>& root) noexcept;
};

} // namespace fbx::xkit
