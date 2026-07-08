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
    raster_ = framebuffer_->map(width_, height_);
    return raster_ != 0;
}

void LcdDevice::setResolution(int w, int h) {
    if (w <= 0 || h <= 0) {
        return;
    }
    width_  = w;
    height_ = h;
    if (framebuffer_ != 0) {
        // Re-map to the new geometry. Ps2Framebuffer keeps one max-size backing store, so
        // this only updates the reported dimensions -- the raster pointer stays valid, so
        // the MIDP screen buffer that points into it does not dangle.
        raster_ = framebuffer_->map(w, h);
    }
}

void LcdDevice::finalize() {
    if (framebuffer_ != 0) {
        framebuffer_->unmap();
    }
    raster_ = 0;
}

javacall_pixel* LcdDevice::screen(int* width, int* height) {
    if (width  != 0) *width  = width_;
    if (height != 0) *height = height_;
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
