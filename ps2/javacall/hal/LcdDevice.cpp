// PS2 JavaCall port — HAL layer. LcdDevice implementation.
#include "LcdDevice.hpp"
#include "IFramebuffer.hpp"

namespace ps2 {
namespace hal {

LcdDevice& LcdDevice::instance() {
    static LcdDevice inst;
    return inst;
}

bool LcdDevice::init() {
    if (framebuffer_ == 0) {
        return false;
    }
    raster_ = framebuffer_->map(WIDTH, HEIGHT);
    return raster_ != 0;
}

void LcdDevice::finalize() {
    if (framebuffer_ != 0) {
        framebuffer_->unmap();
    }
    raster_ = 0;
}

javacall_pixel* LcdDevice::screen(int* width, int* height) {
    if (width  != 0) *width  = WIDTH;
    if (height != 0) *height = HEIGHT;
    return raster_;
}

void LcdDevice::flush() {
    if (framebuffer_ != 0) {
        framebuffer_->present();
    }
}

void LcdDevice::flushPartial(int ystart, int yend) {
    if (framebuffer_ != 0) {
        framebuffer_->presentRegion(ystart, yend);
    }
}

} // namespace hal
} // namespace ps2
