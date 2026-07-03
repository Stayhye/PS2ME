// PS2 JavaCall port — contract layer (ABI boundary).
//
// extern "C" javacall_lcd_* symbols the MIDP LCDUI subsystem links against. All
// behaviour lives in hal::LcdDevice; these are thin shims.
//
// Note: javacall_lcd_flush and javacall_lcd_get_full_screen_mode are NOT defined
// here — the MIDP highlevelui port already provides them, so redefining would
// collide at link time. We own the single built-in portrait display; the many
// query functions describe it (primary, built-in, no pen, no rotation, no native
// soft buttons), mirroring the phoneME single-display reference behaviour.
#include "javacall_lcd.h"   // phoneME contract header (provided via -I at build)

#include "../hal/LcdDevice.hpp"

extern "C" {

javacall_result javacall_lcd_init(void) {
    return ps2::hal::LcdDevice::instance().init() ? JAVACALL_OK : JAVACALL_FAIL;
}

javacall_result javacall_lcd_finalize(void) {
    ps2::hal::LcdDevice::instance().finalize();
    return JAVACALL_OK;
}

javacall_pixel* javacall_lcd_get_screen(int /*hardwareId*/,
                                        int* screenWidth,
                                        int* screenHeight,
                                        javacall_lcd_color_encoding_type* colorEncoding) {
    // RGB565 is mandatory: jcapp_export.c aborts the platform otherwise.
    if (colorEncoding != 0) {
        *colorEncoding = JAVACALL_LCD_COLOR_RGB565;
    }
    return ps2::hal::LcdDevice::instance().screen(screenWidth, screenHeight);
}

javacall_result javacall_lcd_set_full_screen_mode(int /*hardwareId*/,
                                                  javacall_bool useFullScreen) {
    ps2::hal::LcdDevice::instance().setFullScreenMode(useFullScreen == JAVACALL_TRUE);
    return JAVACALL_OK;
}

javacall_result javacall_lcd_flush_partial(int /*hardwareId*/, int ystart, int yend) {
    ps2::hal::LcdDevice::instance().flushPartial(ystart, yend);
    return JAVACALL_OK;
}

int javacall_lcd_get_screen_width(int /*hardwareId*/) {
    return ps2::hal::LcdDevice::instance().width();
}

int javacall_lcd_get_screen_height(int /*hardwareId*/) {
    return ps2::hal::LcdDevice::instance().height();
}

int javacall_lcd_get_current_hardwareId(void) {
    return ps2::hal::LcdDevice::PRIMARY_ID;
}

char* javacall_lcd_get_display_name(int /*hardwareId*/) {
    static char name[] = "PS2 LCD";
    return name;
}

int javacall_lcd_get_display_capabilities(int /*hardwareId*/) {
    return 0;
}

int* javacall_lcd_get_display_device_ids(int* n) {
    // One built-in display. Must return a non-NULL, non-empty array: MIDP's
    // DisplayDeviceContainer builds its device list from this (ids.length /
    // new DisplayDevice(ids[i])), and an empty/NULL list leaves
    // getPrimaryDisplayDevice() == null -> NPE in Display init.
    static int ids[1] = { ps2::hal::LcdDevice::PRIMARY_ID };
    if (n != 0) {
        *n = 1;
    }
    return ids;
}

javacall_bool javacall_lcd_is_display_primary(int /*hardwareId*/) {
    return JAVACALL_TRUE;
}

javacall_bool javacall_lcd_is_display_buildin(int /*hardwareId*/) {
    return JAVACALL_TRUE;
}

javacall_bool javacall_lcd_is_display_pen_supported(int /*hardwareId*/) {
    return JAVACALL_FALSE;
}

javacall_bool javacall_lcd_is_display_pen_motion_supported(int /*hardwareId*/) {
    return JAVACALL_FALSE;
}

javacall_bool javacall_lcd_is_native_softbutton_layer_supported(void) {
    return JAVACALL_FALSE;
}

javacall_result javacall_lcd_set_native_softbutton_label(const javacall_utf16* /*label*/,
                                                         int /*len*/, int /*index*/) {
    return JAVACALL_FAIL;   // no native soft-button layer
}

javacall_bool javacall_lcd_reverse_orientation(int /*hardwareId*/) {
    return JAVACALL_FALSE;  // rotation not supported; orientation unchanged
}

javacall_bool javacall_lcd_get_reverse_orientation(int /*hardwareId*/) {
    return JAVACALL_FALSE;
}

void javacall_lcd_handle_clamshell(void) {
    // No clamshell on PS2.
}

} // extern "C"
