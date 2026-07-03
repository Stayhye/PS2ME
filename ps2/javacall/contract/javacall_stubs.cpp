// PS2 JavaCall port — contract layer: not-yet-ported subsystems.
//
// These javacall_* symbols are referenced by libjvm.a (MIDP) but belong to
// capabilities the PS2 port does not provide at the Milestone B1 boot stage:
//   - font      : the chameleon LCDUI skin renders text through gxjport_* (already
//                 in the library); this javacall_font_* path is unused at boot.
//   - annunciator: status-bar icons / vibration / backlight — no PS2 analogue.
//   - permissions: MIDP security domains — not exercised until a suite is launched.
//   - serial    : comm ports — no JSR wired in.
//
// They return JAVACALL_NOT_IMPLEMENTED (or neutral values), mirroring the phoneME
// reference stubs (implementation/stubs/midp). Each becomes a real HAL-backed
// module when its milestone arrives. Kept in one translation unit because none of
// them carry behaviour worth a class of its own yet.
#include "javacall_font.h"
#include "javacall_annunciator.h"
#include "javacall_serial.h"
#include "javacall_security.h"   // declares javacall_permissions_*
#include "javacall_file.h"       // declares javacall_file_* (config-dump path only)

extern "C" {

// --- file ----------------------------------------------------------------
// The reused share/properties config-db compiles an INI dump/save path that
// references these, but never calls it in the static-properties build (no
// USE_PROPERTIES_FROM_FS). They exist only to satisfy the linker; real file I/O
// arrives with the storage milestone (B4).

javacall_result javacall_file_open(const javacall_utf16* /*name*/, int /*nameLen*/,
                                   int /*flags*/, javacall_handle* /*handle*/) {
    return JAVACALL_FAIL;
}

javacall_result javacall_file_close(javacall_handle /*handle*/) {
    return JAVACALL_FAIL;
}

long javacall_file_read(javacall_handle /*handle*/, unsigned char* /*buf*/, long /*size*/) {
    return -1;
}

long javacall_file_write(javacall_handle /*handle*/, const unsigned char* /*buf*/, long /*size*/) {
    return -1;
}

// --- font ----------------------------------------------------------------
// Deliberately fail: phoneME's gx_putpixel rasterizer (gxj_text.c) tries the
// platform font first (gxjport_draw_chars -> javacall_font_set_font/_draw) and,
// only when it reports failure, falls back to its own built-in bitmap font
// (FontBitmaps / drawChar). Returning JAVACALL_OK here (with an empty draw)
// makes gxjport report success, so the fallback is skipped and NO text renders
// (widgets draw as empty boxes). Returning non-OK routes every glyph through the
// built-in font, giving us real text for free until a PS2-native font is added.
// get_width returns 0 so gx_get_charswidth's own `width > 0` guard also falls
// back to the built-in metrics, keeping draw and layout consistent.

javacall_result javacall_font_set_font(javacall_font_face /*face*/,
                                       javacall_font_style /*style*/,
                                       javacall_font_size /*size*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

javacall_result javacall_font_draw(javacall_pixel /*color*/,
                                   int /*clipX1*/, int /*clipY1*/,
                                   int /*clipX2*/, int /*clipY2*/,
                                   javacall_pixel* /*destBuffer*/,
                                   int /*destBufferHoriz*/, int /*destBufferVert*/,
                                   int /*x*/, int /*y*/,
                                   const javacall_utf16* /*text*/, int /*textLen*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

javacall_result javacall_font_get_info(javacall_font_face /*face*/,
                                       javacall_font_style /*style*/,
                                       javacall_font_size /*size*/,
                                       int* ascent, int* descent, int* leading) {
    if (ascent)  *ascent  = 0;
    if (descent) *descent = 0;
    if (leading) *leading = 0;
    return JAVACALL_NOT_IMPLEMENTED;
}

int javacall_font_get_width(javacall_font_face /*face*/,
                            javacall_font_style /*style*/,
                            javacall_font_size /*size*/,
                            const javacall_utf16* /*text*/, int /*textLen*/) {
    return 0;
}

// --- annunciator ---------------------------------------------------------

javacall_result javacall_annunciator_vibrate(javacall_bool /*enableVibrate*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

javacall_result javacall_annunciator_flash_backlight(javacall_bool /*enableBrightBack*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

javacall_result javacall_annunciator_display_trusted_icon(javacall_bool /*enable*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

javacall_result javacall_annunciator_display_network_icon(javacall_bool /*enable*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

javacall_result javacall_annunciator_display_home_icon(javacall_bool /*enable*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

javacall_result javacall_annunciator_play_audible_tone(javacall_audible_tone_type /*soundType*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

// --- permissions ---------------------------------------------------------

int javacall_permissions_load_domain_list(javacall_utf8_string* /*array*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

int javacall_permissions_load_group_list(javacall_utf8_string* /*array*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

int javacall_permissions_load_group_permissions(javacall_utf8_string* /*list*/,
                                                javacall_utf8_string /*group_name*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

int javacall_permissions_get_default_value(javacall_utf8_string /*domain_name*/,
                                           javacall_utf8_string /*group_name*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

int javacall_permissions_get_max_value(javacall_utf8_string /*domain_name*/,
                                       javacall_utf8_string /*group_name*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

int javacall_permissions_load_group_messages(javacall_utf8_string* /*list*/,
                                             javacall_utf8_string /*group_name*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

void javacall_permissions_loading_finished(void) {
}

// --- serial --------------------------------------------------------------

javacall_result javacall_serial_open_start(const char* /*devName*/, int /*baudRate*/,
                                           unsigned int /*options*/,
                                           javacall_handle* /*pHandle*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

javacall_result javacall_serial_open_finish(javacall_handle /*handle*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

javacall_result javacall_serial_configure(javacall_handle /*handle*/, int /*baudRate*/,
                                          int /*options*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

javacall_result javacall_serial_close_start(javacall_handle /*hPort*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

javacall_result javacall_serial_close_finish(javacall_handle /*hPort*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

javacall_result javacall_serial_read_start(javacall_handle /*hPort*/, unsigned char* /*buffer*/,
                                           int /*size*/, int* /*bytesRead*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

javacall_result javacall_serial_read_finish(javacall_handle /*hPort*/, unsigned char* /*buffer*/,
                                            int /*size*/, int* /*bytesRead*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

javacall_result javacall_serial_write_start(javacall_handle /*hPort*/, unsigned char* /*buffer*/,
                                            int /*size*/, int* /*bytesWritten*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

javacall_result javacall_serial_write_finish(javacall_handle /*hPort*/, unsigned char* /*buffer*/,
                                             int /*size*/, int* /*bytesWritten*/) {
    return JAVACALL_NOT_IMPLEMENTED;
}

} // extern "C"
