// PS2 JavaCall port — HAL layer.
//
// LcdDevice: the display facade the MIDP LCDUI subsystem talks to through the
// javacall_lcd contract. It owns the logical screen (a fixed 240x320 portrait
// "phone" LCD), hands the MIDP rasterizer the RGB565 off-screen buffer it must
// have (jcapp_export.c aborts if get_screen returns anything else), and forwards
// flushes to an injected IFramebuffer. All geometry/state lives here; nothing in
// this class touches hardware.
#ifndef PS2_JAVACALL_HAL_LCDDEVICE_HPP
#define PS2_JAVACALL_HAL_LCDDEVICE_HPP

#include <javacall_defs.h>

namespace ps2 {
namespace hal {

class IFramebuffer;

class LcdDevice {
public:
    /// The virtual phone screen: portrait QVGA, matching the M0 video path.
    static const int WIDTH  = 240;
    static const int HEIGHT = 320;
    /// The single built-in display's hardware id.
    static const int PRIMARY_ID = 0;

    /// Process-wide instance. Constructed on first use (safe static init order
    /// with the platform framebuffer that registers itself at startup).
    static LcdDevice& instance();

    /// Install the presentation backend. Ownership stays with the caller.
    void setFramebuffer(IFramebuffer* fb) { framebuffer_ = fb; }
    IFramebuffer* framebuffer() const { return framebuffer_; }

    /// javacall_lcd_init: acquire the off-screen raster. Returns false on failure.
    bool init();
    /// javacall_lcd_finalize: release the raster.
    void finalize();

    /// javacall_lcd_get_screen: the RGB565 raster plus its dimensions. Returns
    /// nullptr before init() or on allocation failure.
    javacall_pixel* screen(int* width, int* height);

    /// javacall_lcd_flush / _flush_partial.
    void flush();
    void flushPartial(int ystart, int yend);

    int width()  const { return WIDTH; }
    int height() const { return HEIGHT; }

    /// LCDUI feature bits the MIDP DisplayDevice queries via
    /// javacall_lcd_get_display_capabilities. Mirror
    /// com.sun.midp.lcdui.DisplayDevice.DISPLAY_DEVICE_SUPPORTS_*. A value of 0
    /// makes Display.setCurrent throw "This display does not support title"
    /// (or ticker) for any Form/Screen, so the AMS can never leave the splash.
    enum Capability {
        CAP_INPUT_EVENTS = 1,
        CAP_COMMANDS     = 2,
        CAP_FORMS        = 4,
        CAP_TICKER       = 8,
        CAP_TITLE        = 16,
        CAP_ALERTS       = 32,
        CAP_LISTS        = 64,
        CAP_TEXTBOXES    = 128
    };
    /// Full high-level UI support, matching the phoneME reference value (255).
    int capabilities() const {
        return CAP_INPUT_EVENTS | CAP_COMMANDS | CAP_FORMS | CAP_TICKER |
               CAP_TITLE | CAP_ALERTS | CAP_LISTS | CAP_TEXTBOXES;
    }

    bool fullScreenMode() const { return fullScreen_; }
    void setFullScreenMode(bool on) { fullScreen_ = on; }

private:
    LcdDevice() : framebuffer_(0), raster_(0), fullScreen_(false) {}
    LcdDevice(const LcdDevice&);
    LcdDevice& operator=(const LcdDevice&);

    IFramebuffer*   framebuffer_;
    javacall_pixel* raster_;
    bool            fullScreen_;
};

} // namespace hal
} // namespace ps2

#endif // PS2_JAVACALL_HAL_LCDDEVICE_HPP
