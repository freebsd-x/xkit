#include "xrender/font_manager.h"
#include "xkit/application.h"
#include "xkit/element.h"
#include "xkit/animation.h"
#include "xkit/render_node.h"
#include "xkit/theme.h"
#include "xkit/translator.h"
#include "xkit/a11y_phrasing.h"
#include "xkit/engine.h"
#include "xkit/widget.h"
#include "xserver/crash_handler.h"
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include "shared/logger.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <map>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#ifdef __FreeBSD__
#include <sys/capsicum.h>
#endif

namespace fbx::xkit {

static std::shared_ptr<Element> g_focused_element = nullptr;
static std::shared_ptr<XBusClient> g_xbus = nullptr;
static uint32_t g_surface_id = 0;

// ── DnD state ─────────────────────────────────────────────────────────────────
static std::string g_drop_mime;
static std::function<void(float, float, const std::string&)> g_drop_cb;
static bool g_dnd_source_active = false;  // true while we are the drag source
static std::function<void(const XApplication::GamepadEvent&)> g_gamepad_cb;
static std::function<bool(const shared::InputMessage&)> g_shortcut_cb;
static float g_cursor_x = 0.0f, g_cursor_y = 0.0f;  // last known pointer position

void XApplication::cursorPosition(float& x, float& y) noexcept {
    x = g_cursor_x;
    y = g_cursor_y;
}

XApplication::PointerButton
XApplication::pointerButton(const shared::InputMessage& event) noexcept {
    switch (event.modifiers_ >> 16) {   // button code packed in the high bits
        case 272: return PointerButton::Left;
        case 273: return PointerButton::Right;
        case 274: return PointerButton::Middle;
        default:  return PointerButton::Unknown;
    }
}

static void handleDnDMessage(const shared::XBusMessage& msg) noexcept {
    shared::DnDEvent ev{};
    std::memcpy(&ev, msg.payload_, sizeof(ev));
    switch (static_cast<shared::DnDOpcode>(msg.opcode_)) {
    case shared::DnDOpcode::Drop:
        if (g_drop_cb &&
            (g_drop_mime.empty() ||
             std::strcmp(ev.mime_type_, g_drop_mime.c_str()) == 0)) {
            g_drop_cb(ev.x_, ev.y_, std::string(ev.data_));
        }
        break;
    case shared::DnDOpcode::DragEnd:
        g_dnd_source_active = false;
        break;
    default:
        break;
    }
}

static std::shared_ptr<Element> g_pointer_capture = nullptr;

void XApplication::capturePointer(std::shared_ptr<Element> element) noexcept {
    g_pointer_capture = std::move(element);
}
void XApplication::releasePointer() noexcept {
    g_pointer_capture = nullptr;
}
std::shared_ptr<Element> XApplication::getPointerCapture() noexcept {
    return g_pointer_capture;
}

void XApplication::requestFocus(std::shared_ptr<Element> element) noexcept {
    g_focused_element = element;
    // Auto-announce the focused element for screen readers, phrased the way a
    // screen reader speaks ("Save, button" / "Wi-Fi, switch, on").
    if (element) {
        auto tree = element->getSemanticTree();
        a11y::NodeDesc desc{.role = tree.role, .label = tree.label, .value = tree.value};
        // For toggle roles the value string carries the state; map it to the
        // tri-state so describe() says "checked"/"on"/"selected" rather than
        // echoing the raw word after the role.
        if (tree.role == "Checkbox" || tree.role == "Switch" || tree.role == "Radio") {
            desc.value.clear();
            if (tree.value == "checked" || tree.value == "on" ||
                tree.value == "selected" || tree.value == "true" || tree.value == "1")
                desc.checked = 1;
            else if (tree.value == "unchecked" || tree.value == "off" ||
                     tree.value == "not selected" || tree.value == "false" ||
                     tree.value == "0")
                desc.checked = 0;
        }
        announce(a11y::describe(desc));
    }
}

void XApplication::onGamepadEvent(
    std::function<void(const GamepadEvent&)> cb) noexcept {
    g_gamepad_cb = std::move(cb);
}

void XApplication::announce(const std::string& text) noexcept {
    auto* bus = getBus();
    if (!bus || text.empty()) return;
    shared::AccessibilityAnnounce ann{};
    ann.surface_id_ = g_surface_id;
    std::size_t len = std::min(text.size(), sizeof(ann.text_) - 1);
    std::memcpy(ann.text_, text.data(), len);
    ann.text_[len] = '\0';
    bus->Call("com.freebsdx.Accessibility",
              static_cast<uint32_t>(shared::AccessibilityOpcode::Announce),
              {reinterpret_cast<const uint8_t*>(&ann), sizeof(ann)},
              [](const shared::XBusMessage&) {});
}

// Flatten a SemanticNode tree (pre-order BFS) into a flat node list.
static void flattenSemantic(const SemanticNode& node,
                            uint32_t parent_id,
                            std::vector<shared::AccessibilityTreeNode>& out) {
    shared::AccessibilityTreeNode n{};
    n.id_        = node.id;
    n.parent_id_ = parent_id;
    std::size_t rl = std::min(node.role.size(),  std::size_t(15));
    std::size_t ll = std::min(node.label.size(), std::size_t(23));
    std::memcpy(n.role_,  node.role.data(),  rl);  n.role_[rl]  = '\0';
    std::memcpy(n.label_, node.label.data(), ll);  n.label_[ll] = '\0';
    n.x_ = node.x; n.y_ = node.y; n.w_ = node.width; n.h_ = node.height;
    out.push_back(n);
    for (const auto& child : node.children) {
        flattenSemantic(child, node.id, out);
    }
}

void XApplication::updateAccessibilityTree(const std::shared_ptr<Element>& root) noexcept {
    auto* bus = getBus();
    if (!bus || !root) return;

    std::vector<shared::AccessibilityTreeNode> flat;
    flattenSemantic(root->getSemanticTree(), 0, flat);

    uint32_t total = static_cast<uint32_t>(flat.size());
    uint32_t pages = (total + 2) / 3;
    for (uint32_t p = 0; p < pages; ++p) {
        shared::AccessibilityTreePage page{};
        page.surface_id_  = g_surface_id;
        page.total_nodes_ = total;
        page.page_index_  = p;
        uint32_t base  = p * 3;
        uint32_t count = std::min(3u, total - base);
        page.node_count_ = count;
        for (uint32_t i = 0; i < count; ++i) {
            page.nodes_[i] = flat[base + i];
        }
        bus->Call("com.freebsdx.Accessibility",
                  static_cast<uint32_t>(shared::AccessibilityOpcode::UpdateTree),
                  {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
                  [](const shared::XBusMessage&) {});
    }
}

std::shared_ptr<Element> XApplication::getFocusedElement() noexcept {
    return g_focused_element;
}

XBusClient* XApplication::getBus() noexcept {
    return g_xbus.get();
}

// FBX_LAYOUT_AUDIT=1: report nodes that laid out absurdly large (more than 4x
// the window) — the mark of a child that took a container's unbounded 100000 px
// cross axis (a FlexRow with a flexible Spacer in a column without Stretch,
// whose right-aligned buttons end up off-screen). Layout runs on hidden nodes
// too, so one pass covers every panel built at startup. Only the outermost
// offender of each subtree is reported, named by the first texts inside it.
static void auditLayout(const RenderNode& root, float window_w, float window_h) {
    auto texts = [](const RenderNode& n) {
        std::string out;
        std::vector<const RenderNode*> stack{&n};
        int found = 0;
        while (!stack.empty() && found < 3) {
            const RenderNode* cur = stack.back();
            stack.pop_back();
            if (auto* t = dynamic_cast<const RenderText*>(cur); t && !t->text().empty()) {
                out += (found++ ? " | " : "") + t->text().substr(0, 40);
            }
            const auto& kids = cur->children();
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back(it->get());
        }
        return out.empty() ? std::string("(no text)") : out;
    };
    int reported = 0;
    std::function<void(const RenderNode&, bool)> walk = [&](const RenderNode& n, bool parent_bad) {
        Size sz = n.getSize();
        bool bad = sz.width_ > 4 * window_w || sz.height_ > 4 * window_h;
        if (bad && !parent_bad) {
            ++reported;
            std::fprintf(stderr, "[layout-audit] %.0fx%.0f%s: %s\n", sz.width_, sz.height_,
                         n.isVisible() ? "" : " (hidden)", texts(n).c_str());
        }
        for (const auto& c : n.children()) walk(*c, bad || parent_bad);
    };
    walk(root, false);
    std::fprintf(stderr, "[layout-audit] %d oversized subtree(s)\n", reported);
}

int XApplication::Run(std::shared_ptr<Widget> root, const std::string& socket_path, bool allow_background, bool is_shell, InitCallback on_init, bool transparent_bg, uint32_t shell_surface_id) noexcept {
    (void)allow_background;
    fbx::shared::Logger::init(is_shell ? "xshell" : "xapp");
    // Backtrace on fatal faults, before anything can crash. Installed pre-sandbox;
    // once cap_enter() runs, the crash-time spool open() is denied and the handler
    // falls back to a stderr-only trace (still captured in the app's log).
    fbx::xserver::installCrashHandler(is_shell ? "xshell" : "xapp");
    LOG_INFO("Starting SDK Framework...");

    auto shared_socket = std::make_shared<xserver::IpcSocket>(socket_path);
    bool connected = false;
    for (int i = 0; i < 50; ++i) {
        if (shared_socket->connect()) {
            connected = true;
            break;
        }
        if (i % 10 == 0) {
            std::cout << "[xkit] Waiting for display server at " << socket_path << "... (Attempt " << i << ")\n" << std::flush;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!connected) {
        LOG_ERROR("Failed to connect to Display Server at {} after 5 seconds.", socket_path);
        return 1;
    }

    g_xbus = std::make_shared<XBusClient>(shared_socket);

    // App id is pid-derived; keep it nonzero (0 is reserved/invalid — the
    // compositor drops surface 0, since a zero id also results from a misframed
    // message). pid & 0xFFFF is 0 only for pids that are multiples of 65536.
    uint32_t app_id = static_cast<uint32_t>(getpid()) & 0xFFFF;
    if (app_id == 0) app_id = 1;
    uint32_t my_surface_id = is_shell ? shell_surface_id : app_id;
    g_surface_id = my_surface_id;
    int window_w = 800, window_h = 600;

    shared::RenderMessage handshake{
        .surface_id_ = my_surface_id,
        .status_ = is_shell ? shared::RenderMessageStatus::ShellSurfaceCreated : shared::RenderMessageStatus::SurfaceCreated,
        .width_ = static_cast<uint32_t>(window_w),
        .height_ = static_cast<uint32_t>(window_h),
        .stride_ = 0
    };

    std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&handshake), sizeof(handshake));
    if (!shared_socket->sendMessage(payload)) {
        LOG_ERROR("Failed to send handshake to Display Server.");
        return 1;
    }

    LOG_INFO("Waiting for SurfaceReady acknowledgment from Master Server...");
    bool ready = false;
    while (!ready) {
        auto msg_res = shared_socket->receiveMessage();
        if (msg_res) {
            // A single stream read can coalesce an XBus broadcast (the compositor
            // sends the Shell window-list update immediately before SurfaceReady
            // whenever another window exists) with the RenderMessage ack. Walk
            // every frame rather than only the first, skipping XBus frames (keyed
            // by the magic) so we don't misread one as a RenderMessage and drop
            // the trailing SurfaceReady — which would hang the handshake forever.
            const auto& buf = msg_res.value().first;
            size_t off = 0;
            while (off + sizeof(shared::RenderMessage) <= buf.size()) {
                if (off + sizeof(shared::XBusMessage) <= buf.size()) {
                    uint32_t magic;
                    std::memcpy(&magic, buf.data() + off, sizeof(magic));
                    if (magic == shared::XBUS_MAGIC) { off += sizeof(shared::XBusMessage); continue; }
                }
                const auto* res = reinterpret_cast<const shared::RenderMessage*>(buf.data() + off);
                if (res->status_ == shared::RenderMessageStatus::SurfaceReady) {
                    window_w = static_cast<int>(res->width_);
                    window_h = static_cast<int>(res->height_);
                    LOG_INFO("SurfaceReady Handshake Complete! Screen: {}x{}", window_w, window_h);
                    ready = true;
                    break;
                }
                off += sizeof(shared::RenderMessage);
            }
        }
        if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (on_init) on_init(*g_xbus);

    // Announce app identity to the compositor.
    // XAPP_ID is set by xshell when launching a .xapp bundle.
    {
        const char* xapp_id = std::getenv("XAPP_ID");
        if (xapp_id && xapp_id[0]) {
            shared::XBusMessage ctx_msg{};
            ctx_msg.magic_ = shared::XBUS_MAGIC;
            std::strncpy(ctx_msg.service_name_, "com.freebsdx.SecurityContext",
                         sizeof(ctx_msg.service_name_) - 1);
            ctx_msg.opcode_ = static_cast<uint32_t>(shared::SecurityContextOpcode::SetContext);
            shared::SecurityContextInfo info{};
            info.surface_id_ = 0; // socket-level identity
            std::strncpy(info.app_id_, xapp_id, sizeof(info.app_id_) - 1);
            std::strncpy(info.sandbox_engine_, "org.freebsdx.Capsicum",
                         sizeof(info.sandbox_engine_) - 1);
            // instance_id = stringified PID
            auto pid_str = std::to_string(getpid());
            std::strncpy(info.instance_id_, pid_str.c_str(), sizeof(info.instance_id_) - 1);
            std::memcpy(ctx_msg.payload_, &info, sizeof(info));
            std::span<const uint8_t> sp(reinterpret_cast<const uint8_t*>(&ctx_msg),
                                        sizeof(ctx_msg));
            (void)shared_socket->sendMessage(sp);
        }
    }

    LOG_INFO("Initializing xkit Native Vulkan Client Renderer...");
    VulkanEngine renderer;
    if (!renderer.init(window_w, window_h)) {
        LOG_ERROR("Vulkan Engine initialization failed.");
        return 1;
    }
    // The desktop shell renders a transparent desktop so app windows (stacked
    // below the shell) show through; its panels/overlays stay opaque. Apps and
    // the greeter keep an opaque background.
    renderer.setTransparentClear(transparent_bg);

    // Wrap the app's root in an OverlayHost so any widget can pop a
    // dropdown/menu/dialog via OverlayManager, rendered above all content. It is
    // transparent (pure pass-through) until an overlay is shown.
    auto host_root = std::make_shared<OverlayHostWidget>(root);
    auto root_element = host_root->createElement();
    root_element->mount(nullptr);

    // Apply default theme immediately so elements initialise their cached colors.
    root_element->onThemeChanged(ThemeManager::getInstance().getCurrentTheme());

    // Query the server for the persisted theme state (dark_mode + accent_idx).
    // The reply can be dropped during the burst of IPC traffic at startup: the
    // SOCK_STREAM transport coalesces messages and HandleIncoming only dispatches
    // the first message in each read, so a reply that shares a read with others
    // is lost. Re-issue the query (see the retry in the main loop) until it lands;
    // once the burst settles, replies arrive reliably. theme_applied stops it.
    auto theme_applied = std::make_shared<bool>(false);
    auto query_theme = [root_el = root_element, theme_applied]() {
        if (!g_xbus) return;
        g_xbus->Call("com.freebsdx.Theme",
            static_cast<uint32_t>(shared::ThemeOpcode::GetTheme),
            {},
            [root_el, theme_applied](const shared::XBusMessage& resp) {
                shared::ThemeState ts{};
                std::memcpy(&ts, resp.payload_, sizeof(ts));
                ThemeManager::getInstance().setDarkMode(ts.dark_mode_ != 0);
                ThemeManager::getInstance().setAccentIndex(static_cast<int>(ts.accent_idx_));
                root_el->onThemeChanged(ThemeManager::getInstance().getCurrentTheme());
                *theme_applied = true;
            });
    };
    query_theme();

    // Query the session locale and point the global Translator at it, so tr()
    // returns localized strings. Same retry rationale as query_theme; thereafter
    // a LocaleChanged broadcast (intercepted in the main loop) keeps it current.
    auto locale_applied = std::make_shared<bool>(false);
    auto query_locale = [locale_applied]() {
        if (!g_xbus) return;
        g_xbus->Call("com.freebsdx.Locale",
            static_cast<uint32_t>(shared::LocaleOpcode::GetLocale),
            {},
            [locale_applied](const shared::XBusMessage& resp) {
                shared::LocaleState ls{};
                std::memcpy(&ls, resp.payload_, sizeof(ls));
                Translator::global().setLocale(ls.locale_);
                *locale_applied = true;
            });
    };
    query_locale();

    int sock_fd = shared_socket->fd();
    int flags = fcntl(sock_fd, F_GETFL, 0);
    fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);

    // --- Capsicum sandbox ---
    // By this point all filesystem access is complete:
    //   - XBus socket: connected and in non-blocking mode
    //   - Vulkan device + pipelines: opened/compiled during renderer.init()
    //   - Font atlas: loaded from disk during renderer.init() (FT_New_Face fd stays open)
    //   - Mesa shader cache: written during renderer.init(); subsequent writes fail silently
    // The shell is excluded — it needs fork()/exec() to launch apps.
#ifdef __FreeBSD__
    if (!is_shell) {
        // XBus socket: restrict to I/O and event-wait only
        cap_rights_t sock_rights;
        cap_rights_init(&sock_rights,
            CAP_READ, CAP_WRITE, CAP_RECV, CAP_SEND,
            CAP_EVENT, CAP_FCNTL, CAP_GETSOCKOPT, CAP_SETSOCKOPT);
        if (cap_rights_limit(sock_fd, &sock_rights) < 0) {
            LOG_WARN("cap_rights_limit(socket) failed: {}", strerror(errno));
        }

        // stdout / stderr: write-only (logging)
        cap_rights_t io_rights;
        cap_rights_init(&io_rights, CAP_WRITE);
        (void)cap_rights_limit(STDOUT_FILENO, &io_rights);
        (void)cap_rights_limit(STDERR_FILENO, &io_rights);

        // Load the fonts text will ask for now: after cap_enter() a face can't
        // be opened any more (FreeType keeps the files it already has open),
        // so a face first needed later drew nothing — every label in a
        // sandboxed app came out blank.
        for (const char* family : {"", "monospace"})
            for (auto weight : {fbx::xrender::FontWeight::Light, fbx::xrender::FontWeight::Regular,
                                fbx::xrender::FontWeight::SemiBold, fbx::xrender::FontWeight::Bold})
                for (auto style : {fbx::xrender::FontStyle::Normal, fbx::xrender::FontStyle::Italic})
                    (void)fbx::xrender::FontManager::getFace(family, weight, style);
        (void)fbx::xrender::FontManager::getEmojiFace();

        // Enter capability mode — global namespace (filesystem, new sockets) now inaccessible.
        if (cap_enter() == 0) {
            LOG_INFO("Capsicum sandbox active.");

            // Notify the compositor so it can enforce tighter per-opcode policy.
            shared::XBusMessage sandbox_msg{};
            sandbox_msg.magic_ = shared::XBUS_MAGIC;
            std::strncpy(sandbox_msg.service_name_, "com.freebsdx.Sandbox",
                         sizeof(sandbox_msg.service_name_) - 1);
            sandbox_msg.opcode_ = static_cast<uint32_t>(shared::SandboxOpcode::EnterSandbox);
            shared::SandboxInfo info{};
            std::memcpy(sandbox_msg.payload_, &info, sizeof(info));
            std::span<const uint8_t> sp(
                reinterpret_cast<const uint8_t*>(&sandbox_msg), sizeof(sandbox_msg));
            (void)shared_socket->sendMessage(sp);
        } else if (errno != ENOSYS) {
            LOG_WARN("cap_enter() failed: {} — running unsandboxed", strerror(errno));
        }
    }
#endif

    Ticker ticker;
    auto last_time = std::chrono::steady_clock::now();
    constexpr int kActiveFrameMs = 16;  // ~60 FPS while animating / interacting
    constexpr int kIdleFrameMs   = 100; // ~10 FPS when idle — enough to refresh
                                        // polled data (clock, CPU/mem) without
                                        // re-rendering a static UI 60x/second

    // Cursor position accumulator — libinput sends relative motion; we track absolute here.
    float cursor_x = static_cast<float>(window_w) / 2.0f;
    float cursor_y = static_cast<float>(window_h) / 2.0f;

    // Last cursor shape we told the compositor to show while hovering our surface,
    // so we only send a set-shape request when the hovered widget's cursor changes.
    uint32_t last_cursor_shape = static_cast<uint32_t>(CursorShape::Default);

    // Re-issue the startup theme query until its reply lands (see query_theme).
    // Retry roughly twice a second for the first ~8s so we ride out the startup
    // IPC burst; after that, any theme change arrives via the ThemeChanged
    // broadcast, so we stop re-asking.
    int  theme_retry_frames = 0;
    int  theme_retries_left = 16;
    int  locale_retry_frames = 0;
    int  locale_retries_left = 16;

    // --- Main Application Loop ---
    while (true) {
        if (!*theme_applied && theme_retries_left > 0 && ++theme_retry_frames >= 6) {
            theme_retry_frames = 0;
            --theme_retries_left;
            query_theme();
        }
        if (!*locale_applied && locale_retries_left > 0 && ++locale_retry_frames >= 6) {
            locale_retry_frames = 0;
            --locale_retries_left;
            query_locale();
        }
        // Block until the compositor sends something (input/broadcast) or a
        // timeout fires, instead of busy-rendering at a fixed 60 FPS. This wakes
        // immediately on input (low latency), runs at 60 FPS while an animation
        // is active, and otherwise idles at ~10 FPS — so a static desktop no
        // longer re-renders and re-sends a full FrameReady 60x/second for no
        // visual change.
        {
            int timeout_ms = ticker.hasActiveAnimations() ? kActiveFrameMs : kIdleFrameMs;
            struct pollfd pfd{ shared_socket->fd(), POLLIN, 0 };
            (void)::poll(&pfd, 1, timeout_ms);
        }

        auto frame_start = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(frame_start - last_time).count();
        last_time = frame_start;

        // Drain all pending IPC messages
        while (true) {
            auto msg_res = shared_socket->receiveMessage();
            if (msg_res) {
                auto& data = msg_res.value().first;
                // Intercept DnD messages before the generic XBus handler so they
                // don't consume a broadcast slot that apps may have set.
                if (data.size() >= sizeof(shared::XBusMessage)) {
                    const auto* xm =
                        reinterpret_cast<const shared::XBusMessage*>(data.data());
                    if (xm->magic_ == shared::XBUS_MAGIC &&
                        std::strcmp(xm->service_name_, "com.freebsdx.DnD") == 0) {
                        handleDnDMessage(*xm);
                        continue;
                    }
                    // LocaleChanged: repoint the Translator live. Strings created
                    // afterwards are localized; existing widgets keep their text
                    // until rebuilt (full live re-translate needs an onLocaleChanged
                    // element hook — a follow-up).
                    if (xm->magic_ == shared::XBUS_MAGIC &&
                        std::strcmp(xm->service_name_, "com.freebsdx.Locale") == 0 &&
                        xm->opcode_ == static_cast<uint32_t>(shared::LocaleOpcode::LocaleChanged)) {
                        shared::LocaleState ls{};
                        std::memcpy(&ls, xm->payload_, sizeof(ls));
                        Translator::global().setLocale(ls.locale_);
                        continue;
                    }
                }
                // The compositor closed our window (title-bar close, Super+Q,
                // Screen Time's "Close app"): the app ends. Before, the message
                // was dropped and the process lived on with no window.
                if (data.size() == sizeof(shared::RenderMessage)) {
                    const auto* rm = reinterpret_cast<const shared::RenderMessage*>(data.data());
                    if (rm->status_ == shared::RenderMessageStatus::SurfaceClosed &&
                        rm->surface_id_ == my_surface_id && !is_shell) {
                        LOG_INFO("Window closed — exiting.");
                        return 0;
                    }
                }
                if (!g_xbus->HandleIncoming(data)) {
                    // The unframed SOCK_STREAM transport can coalesce several
                    // InputMessages into one recvmsg — a single key produces a
                    // press *and* a release, written back-to-back, and fast
                    // typing bunches more still. Dispatch every whole message in
                    // the buffer, not just the lone-message case; otherwise
                    // coalesced keystrokes are silently dropped (this is why the
                    // login greeter, which receives raw input, couldn't type).
                    if (data.size() >= sizeof(shared::InputMessage) &&
                        data.size() % sizeof(shared::InputMessage) == 0) {
                      const size_t input_count =
                          data.size() / sizeof(shared::InputMessage);
                      const auto* raws =
                          reinterpret_cast<const shared::InputMessage*>(data.data());
                      for (size_t input_i = 0; input_i < input_count; ++input_i) {
                        shared::InputMessage input_msg = raws[input_i];

                        // The compositor delivers surface-local absolute coordinates
                        // for mouse motion (it used to send relative deltas that the
                        // client accumulated, which drifted for off-centre windows and
                        // only tracked the focused surface). Clamp into the window and
                        // record as the current cursor position.
                        if (input_msg.type_ == shared::InputEventType::MouseMove) {
                            cursor_x = std::clamp(static_cast<float>(input_msg.value_a_),
                                                  0.0f, static_cast<float>(window_w));
                            cursor_y = std::clamp(static_cast<float>(input_msg.value_b_),
                                                  0.0f, static_cast<float>(window_h));
                            input_msg.value_a_ = static_cast<int32_t>(cursor_x);
                            input_msg.value_b_ = static_cast<int32_t>(cursor_y);
                            g_cursor_x = cursor_x; g_cursor_y = cursor_y;
                        } else if (input_msg.type_ == shared::InputEventType::MouseButton) {
                            // Preserve the raw button state (1=down, 0=up) in
                            // char_value_ before overwriting value_a_/value_b_ with
                            // the cursor position for elements' bounds checks.
                            input_msg.char_value_ = input_msg.value_b_;
                            // value_a_ still holds the button code (272=left/273=
                            // right/274=middle) from the compositor — pack it into
                            // modifiers_'s high bits so widgets can read it, keeping
                            // keyboard mods in the low bits (Ctrl+click still works).
                            input_msg.modifiers_ = (input_msg.modifiers_ & 0x0000FFFFu) |
                                (static_cast<uint32_t>(input_msg.value_a_) << 16);
                            input_msg.value_a_ = static_cast<int32_t>(cursor_x);
                            input_msg.value_b_ = static_cast<int32_t>(cursor_y);
                        }

                        if (input_msg.type_ == shared::InputEventType::ThemeChanged) {
                            ThemeManager::getInstance().setDarkMode(input_msg.value_a_ != 0);
                            ThemeManager::getInstance().setAccentIndex(input_msg.value_b_);
                            root_element->onThemeChanged(ThemeManager::getInstance().getCurrentTheme());
                        } else if (
                            // Touch and stylus events carry absolute coordinates —
                            // skip the cursor accumulation step and deliver directly.
                            input_msg.type_ == shared::InputEventType::TouchDown   ||
                            input_msg.type_ == shared::InputEventType::TouchMove   ||
                            input_msg.type_ == shared::InputEventType::TouchUp     ||
                            input_msg.type_ == shared::InputEventType::StylusDown  ||
                            input_msg.type_ == shared::InputEventType::StylusMove  ||
                            input_msg.type_ == shared::InputEventType::StylusUp    ||
                            input_msg.type_ == shared::InputEventType::StylusProximity ||
                            input_msg.type_ == shared::InputEventType::GesturePinchBegin  ||
                            input_msg.type_ == shared::InputEventType::GesturePinchUpdate ||
                            input_msg.type_ == shared::InputEventType::GesturePinchEnd) {
                            // Slot-0 touch: mirror the absolute position to the cursor so
                            // widgets that only check cursor_x/cursor_y stay accurate.
                            if ((input_msg.type_ == shared::InputEventType::TouchDown ||
                                 input_msg.type_ == shared::InputEventType::TouchMove) &&
                                 input_msg.modifiers_ == 0) {
                                cursor_x = static_cast<float>(input_msg.value_a_);
                                cursor_y = static_cast<float>(input_msg.value_b_);
                                g_cursor_x = cursor_x; g_cursor_y = cursor_y;
                            }
                            (void)root_element->handleEvent(input_msg);
                        } else if (input_msg.type_ == shared::InputEventType::GamepadButton ||
                                   input_msg.type_ == shared::InputEventType::GamepadAxis) {
                            if (g_gamepad_cb) {
                                GamepadEvent ge{
                                    .is_axis        = (input_msg.type_ == shared::InputEventType::GamepadAxis),
                                    .code           = static_cast<uint32_t>(input_msg.value_a_),
                                    .value          = input_msg.value_b_,
                                    .controller_idx = input_msg.modifiers_,
                                };
                                g_gamepad_cb(ge);
                            }
                        } else if (input_msg.type_ == shared::InputEventType::MouseMove ||
                                   input_msg.type_ == shared::InputEventType::MouseButton ||
                                   input_msg.type_ == shared::InputEventType::MouseWheel) {
                            // Mouse events route by position (hit-testing from the
                            // root), independent of keyboard focus — except while an
                            // element holds a pointer capture (a drag), when they go
                            // straight to it so the drag continues off-bounds.
                            if (auto capture = getPointerCapture()) {
                                (void)capture->handleEvent(input_msg);
                                // Auto-release on button-up (after the element sees it).
                                if (input_msg.type_ == shared::InputEventType::MouseButton &&
                                    input_msg.char_value_ == 0) {
                                    releasePointer();
                                }
                            } else {
                                (void)root_element->handleEvent(input_msg);
                            }
                        } else {
                            // Global shortcuts (e.g. Ctrl+S) get first refusal on
                            // key presses, before the focused element — so a text
                            // field doesn't swallow them. A true return consumes it.
                            if (g_shortcut_cb &&
                                input_msg.type_ == shared::InputEventType::KeyPress &&
                                input_msg.value_b_ != 0 && g_shortcut_cb(input_msg)) {
                                // consumed by a global accelerator
                            } else {
                                // Keyboard and other events go to the focused element.
                                auto focused = getFocusedElement();
                                if (focused) (void)focused->handleEvent(input_msg);
                                else (void)root_element->handleEvent(input_msg);
                            }
                        }

                        // Hover feedback: resolve the cursor shape for the widget
                        // under the pointer and ask the compositor to show it when
                        // it changes. Uses the previous frame's layout, which is a
                        // frame stale at worst.
                        if (input_msg.type_ == shared::InputEventType::MouseMove && g_xbus) {
                            auto rn = root_element->getRenderNode();
                            CursorShape cs = rn ? rn->resolveCursorAt({cursor_x, cursor_y})
                                                : CursorShape::Default;
                            uint32_t shape = static_cast<uint32_t>(cs);
                            if (shape != last_cursor_shape) {
                                last_cursor_shape = shape;
                                g_xbus->Call("com.freebsdx.Input", 0x03,
                                    {reinterpret_cast<const uint8_t*>(&shape), sizeof(shape)},
                                    nullptr);
                            }
                        }
                      }
                    }
                }
            } else {
                if (msg_res.error() == std::errc::resource_unavailable_try_again ||
                    msg_res.error().value() == EAGAIN || msg_res.error().value() == EWOULDBLOCK) {
                    break; // No more messages for now
                }
                LOG_ERROR("Display Server disconnected. Reason: {}", msg_res.error().message());
                return 0;
            }
        }

        ticker.tick(dt);

        Constraints c{0, (float)window_w, 0, (float)window_h};
        auto root_node = root_element->getRenderNode();
        root_element->syncState();
        (void)root_node->layout(c);

        {
            static const bool audit = [] { const char* e = std::getenv("FBX_LAYOUT_AUDIT"); return e && *e == '1'; }();
            static int audit_frame = 0;
            if (audit && ++audit_frame == 3) auditLayout(*root_node, static_cast<float>(window_w), static_cast<float>(window_h));
        }

        // The desktop shell: tell the compositor where its open popups are, so
        // a click on one that sits over an app window reaches the shell. Popups
        // are the visible layers of the shell's root stack (the overlay host's
        // child 0) that paint a background and aren't full-screen (the
        // desktop/taskbar layer is), plus xkit's own overlays (menus, dialogs).
        if (is_shell && shell_surface_id == 999 && g_xbus) {
            shared::ShellInputRegion region{};
            const float screen_area = static_cast<float>(window_w) * static_cast<float>(window_h);
            auto add = [&](const RenderNode& n, bool needs_background) {
                if (region.count_ >= std::size(region.rects_)) return;
                if (!n.isVisible() || (needs_background && !n.paintsBackground())) return;
                Size sz = n.getSize();
                if (sz.width_ <= 0 || sz.height_ <= 0 || sz.width_ * sz.height_ >= 0.9f * screen_area) return;
                Offset o = n.getAbsoluteOffset();
                region.rects_[region.count_++] = {
                    static_cast<int16_t>(o.x_), static_cast<int16_t>(o.y_),
                    static_cast<int16_t>(sz.width_), static_cast<int16_t>(sz.height_)};
            };
            const auto& host_children = root_node->children();
            if (!host_children.empty())
                for (const auto& layer : host_children[0]->children()) add(*layer, true);
            for (size_t i = 1; i < host_children.size(); ++i) add(*host_children[i], false);
            static shared::ShellInputRegion sent_region{};
            static bool sent_once = false;
            if (!sent_once || std::memcmp(&region, &sent_region, sizeof(region)) != 0) {
                sent_region = region;
                sent_once = true;
                g_xbus->Call("com.freebsdx.Shell", static_cast<uint32_t>(shared::ShellOpcode::SetInputRegion),
                             {reinterpret_cast<const uint8_t*>(&region), sizeof(region)}, nullptr);
            }
        }

        // Render and Export frame
        auto frame = renderer.paint(root_node);
        auto dmabuf_fd = frame.exportDmabuf();

        if (dmabuf_fd.isValid()) {
            shared::RenderMessage frame_msg{
                .surface_id_ = my_surface_id,
                .status_ = shared::RenderMessageStatus::FrameReady,
                .width_ = (uint32_t)window_w,
                .height_ = (uint32_t)window_h,
                .damage_x_ = frame.damage_x_,
                .damage_y_ = frame.damage_y_,
                .damage_width_ = frame.damage_width_,
                .damage_height_ = frame.damage_height_,
                .buffer_index_ = frame.buffer_index_
            };
            std::span<const uint8_t> payload(reinterpret_cast<const uint8_t*>(&frame_msg), sizeof(frame_msg));
            auto send_res = shared_socket->sendMessage(payload, dmabuf_fd.get());
            if (!send_res) {
                LOG_ERROR("Failed to send FrameReady: {}", send_res.error().message());
            }
        }

        // Pacing is handled by the poll() at the top of the loop — no fixed
        // trailing sleep, so input is serviced as soon as it arrives.
    }

    return 0;
}

void XApplication::startDrag(const std::string& mime_type,
                              const std::string& data) noexcept {
    if (!g_xbus || g_dnd_source_active) return;
    shared::XBusMessage msg{};
    msg.magic_ = shared::XBUS_MAGIC;
    std::strncpy(msg.service_name_, "com.freebsdx.DnD",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::DnDOpcode::StartDrag);
    shared::DnDStartRequest req{};
    req.surface_id_ = g_surface_id;
    std::strncpy(req.mime_type_, mime_type.c_str(), sizeof(req.mime_type_) - 1);
    std::strncpy(req.data_,      data.c_str(),      sizeof(req.data_)      - 1);
    std::memcpy(msg.payload_, &req, sizeof(req));
    g_xbus->Call("com.freebsdx.DnD",
                 static_cast<uint32_t>(shared::DnDOpcode::StartDrag),
                 {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                 nullptr);
    g_dnd_source_active = true;
    LOG_INFO("DnD drag started: mime={} data={}", mime_type, data);
}

void XApplication::setOnDrop(
    const std::string& mime_type,
    std::function<void(float, float, const std::string&)> cb) noexcept {
    g_drop_mime = mime_type;
    g_drop_cb   = std::move(cb);
}

void XApplication::setOnShortcut(
    std::function<bool(const shared::InputMessage&)> cb) noexcept {
    g_shortcut_cb = std::move(cb);
}

void XApplication::setTitle(const std::string& title) noexcept {
    if (!g_xbus || g_surface_id == 0) return;
    shared::WindowTitleRequest req{};
    req.surface_id_ = g_surface_id;
    std::strncpy(req.title_, title.c_str(), sizeof(req.title_) - 1);
    g_xbus->Call("com.freebsdx.Window",
                 static_cast<uint32_t>(shared::WindowOpcode::SetTitle),
                 {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                 nullptr);
}

} // namespace fbx::xkit
