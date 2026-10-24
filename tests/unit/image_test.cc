#include "xkit/widget.h"
#include "xkit/element.h"
#include "xkit/render_node.h"
#include <gtest/gtest.h>
#include <memory>

namespace fbx::xkit::testing {

class ImageTest : public ::testing::Test {};

TEST_F(ImageTest, ImageElementLayout) {
    auto image_widget = std::make_shared<ImageWidget>("/dummy/path.png");
    image_widget->setFixedSize(100.0f, 100.0f); // Mock size: no real image to decode
    auto element = image_widget->createElement();
    
    element->mount(nullptr);
    auto render_node = element->getRenderNode();

    Constraints c{0, 500, 0, 500};
    Size size = render_node->layout(c);

    // Mock size is 100x100
    EXPECT_EQ(size.width_, 100.0f);
    EXPECT_EQ(size.height_, 100.0f);
}

} // namespace fbx::xkit::testing
