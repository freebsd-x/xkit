#include "xkit/signal.h"
#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include "xrender/display_list.h"
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <iostream>
#include <memory>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace fbx::xkit;
using namespace fbx::xrender;
using namespace fbx::xserver;
using namespace fbx::shared;

class ContainerElement : public Element {
public:
    ContainerElement(std::shared_ptr<ContainerWidget> widget) noexcept {
        widget_ = widget;
        renderNode_ = std::make_shared<RenderBox>(widget->width(), widget->height());
    }

    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        std::cout << "[xkit ElementTree] ContainerElement mounted.\n";
    }

    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        auto containerWidget = std::dynamic_pointer_cast<ContainerWidget>(newWidget);
        if (containerWidget) {
            widget_ = containerWidget;
            auto renderBox = std::dynamic_pointer_cast<RenderBox>(renderNode_);
            if (renderBox) {
                renderBox->setDesiredSize(containerWidget->width(), containerWidget->height());
                std::cout << "[xkit ElementTree] ContainerElement updated (RenderBox resized).\n";
            }
        }
    }

    void unmount() noexcept override {
        std::cout << "[xkit ElementTree] ContainerElement unmounted.\n";
    }
};

std::shared_ptr<Element> ContainerWidget::createElement() noexcept {
    return std::make_shared<ContainerElement>(std::make_shared<ContainerWidget>(*this));
}

class TextElement : public Element {
public:
    TextElement(std::shared_ptr<TextWidget> widget) noexcept {
        widget_ = widget;
        renderNode_ = std::make_shared<RenderText>(widget->text(), widget->font_size());
    }

    void mount(std::shared_ptr<Element> parent) noexcept override {
        parent_ = parent;
        std::cout << "[xkit ElementTree] TextElement mounted.\n";
    }

    void update(std::shared_ptr<Widget> newWidget) noexcept override {
        auto textWidget = std::dynamic_pointer_cast<TextWidget>(newWidget);
        if (textWidget) {
            widget_ = textWidget;
            auto renderText = std::dynamic_pointer_cast<RenderText>(renderNode_);
            if (renderText) {
                renderText->setText(textWidget->text());
                std::cout << "[xkit ElementTree] TextElement updated.\n";
            }
        }
    }

    void unmount() noexcept override {
        std::cout << "[xkit ElementTree] TextElement unmounted.\n";
    }
};

std::shared_ptr<Element> TextWidget::createElement() noexcept {
    return std::make_shared<TextElement>(std::make_shared<TextWidget>(*this));
}

int main() {
    std::cout << "Starting xkit (Phase: Advanced UI & SDF Text Rendering) [C++23 Strict Mode]...\n";

    Signal<float> windowWidth(800.0f);
    Signal<float> windowHeight(600.0f);

    auto rootColumn = std::make_shared<RenderFlexColumn>();
    auto initialWidget = std::make_shared<ContainerWidget>(windowWidth.get(), windowHeight.get());
    auto rootElement = initialWidget->createElement();
    rootElement->mount(nullptr);

    auto textWidget = std::make_shared<TextWidget>("Hello FreeBSD-X!", 24.0f);
    auto textElement = textWidget->createElement();
    textElement->mount(nullptr);

    rootColumn->addChild(rootElement->getRenderNode());
    rootColumn->addChild(textElement->getRenderNode());
    rootColumn->addChild(std::make_shared<RenderBox>(1920.0f, 100.0f));

    Constraints screenConstraints = {
        .min_width_ = 0.0f, .max_width_ = 1920.0f,
        .min_height_ = 0.0f, .max_height_ = 1080.0f
    };

    [[maybe_unused]] Size s1 = rootColumn->layout(screenConstraints);
    DisplayList displayList;
    rootColumn->paint(displayList);

    windowWidth.subscribe([&](const float& w) {
        std::cout << "[xkit Signal] Window width changed to " << w << "\n";
        
        auto updatedWidget = std::make_shared<ContainerWidget>(w, windowHeight.get());
        rootElement->update(updatedWidget);

        [[maybe_unused]] Size s2 = rootColumn->layout(screenConstraints);
        displayList.clear();
        rootColumn->paint(displayList);
        std::cout << "[xkit] UI Layout instantly recalculated. DisplayList updated.\n";
    });

    int clientSocket = socket(AF_UNIX, SOCK_STREAM, 0);
    UniqueFd clientFd(clientSocket);
    IpcSocket client(std::move(clientFd));

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/freebsd-x-input.sock", sizeof(addr.sun_path) - 1);

    std::cout << "[xkit] Connecting to xserver input socket...\n";
    if (connect(clientSocket, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        std::cout << "[xkit] Connected to Input Router. Waiting for events...\n";
        
        auto msgRes = client.receiveMessage();
        if (msgRes) {
            auto& data = msgRes.value().first;
            if (data.size() == sizeof(InputMessage)) {
                const InputMessage* input = reinterpret_cast<const InputMessage*>(data.data());
                if (input->type_ == InputEventType::MouseMove) {
                    std::cout << "[xkit] Received IPC InputEvent: MouseMove to X=" << input->value_a_ << " Y=" << input->value_b_ << "\n";
                    windowWidth.set(static_cast<float>(input->value_a_));
                }
            }
        } else {
            std::cerr << "[xkit] Failed to receive input IPC: " << msgRes.error().message() << "\n";
        }
    } else {
        std::cerr << "[xkit] Failed to connect to xserver input socket.\n";
    }

    std::cout << "\n--- Teardown ---\n";
    rootElement->unmount();

    std::cout << "xkit Phase 6 completed successfully.\n";
    return 0;
}
