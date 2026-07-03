// PS2 JavaCall port — HAL layer.
//
// IFramebuffer: the presentation backend LcdDevice draws through. It abstracts
// "where the MIDP off-screen raster lives and how it reaches the display" so the
// LCDUI logic in LcdDevice never touches ps2sdk. The MIDP software rasterizer
// (gxj) renders into the RGB565 buffer returned by map(); present()/presentRegion()
// push it to the screen.
//
// Milestone B1 uses a RAM-only backend whose present() is a no-op (the goal is a
// clean link + boot, not pixels). Milestone B2 swaps in a backend that uploads the
// buffer to the GS via libdraw, converting RGB565 -> RGBA5551 in the flush.
#ifndef PS2_JAVACALL_HAL_IFRAMEBUFFER_HPP
#define PS2_JAVACALL_HAL_IFRAMEBUFFER_HPP

#include <javacall_defs.h>   // javacall_pixel

namespace ps2 {
namespace hal {

class IFramebuffer {
public:
    virtual ~IFramebuffer() {}

    /// Provide (allocating if needed) the off-screen RGB565 raster for a
    /// @p width x @p height screen. Returns the top-left pixel, or nullptr on
    /// failure. Repeated calls with the same dimensions return the same buffer.
    virtual javacall_pixel* map(int width, int height) = 0;

    /// Release the raster obtained from map(). Called at LCD finalize.
    virtual void unmap() = 0;

    /// Push the whole raster to the display.
    virtual void present() = 0;

    /// Push the scanline band [ystart, yend] to the display (partial flush).
    virtual void presentRegion(int ystart, int yend) = 0;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_IFRAMEBUFFER_HPP
